/* =========================================================================
 * object.c - Zircon Dispatcher Lifetime, KOIDs & Per-Process Handle Table
 * References:
 *   - zircon_org/kernel/object/dispatcher.cpp
 *   - zircon_org/kernel/object/handle.cpp
 *   - zircon_org/kernel/object/process_dispatcher.cpp
 *   - zircon_org/kernel/object/log_dispatcher.cpp
 * ========================================================================= */
#include "object.h"
#include "heap.h"
#include "debuglog.h"

static volatile uint64_t global_koid_counter = 1024ULL;

ab_koid_t ab_koid_alloc(void) {
    return __atomic_fetch_add(&global_koid_counter, 1ULL, __ATOMIC_RELAXED);
}

const char *ab_obj_type_name(ab_obj_type_t type) {
    switch (type) {
        case AB_OBJ_TYPE_NONE:      return "NONE";
        case AB_OBJ_TYPE_PROCESS:   return "PROCESS";
        case AB_OBJ_TYPE_THREAD:    return "THREAD";
        case AB_OBJ_TYPE_VMO:       return "VMO";
        case AB_OBJ_TYPE_CHANNEL:   return "CHANNEL";
        case AB_OBJ_TYPE_EVENT:     return "EVENT";
        case AB_OBJ_TYPE_EVENTPAIR: return "EVENTPAIR";
        case AB_OBJ_TYPE_PORT:      return "PORT";
        case AB_OBJ_TYPE_INTERRUPT: return "INTERRUPT";
        case AB_OBJ_TYPE_RESOURCE:  return "RESOURCE";
        case AB_OBJ_TYPE_LOG:       return "LOG";
        case AB_OBJ_TYPE_JOB:       return "JOB";
        case AB_OBJ_TYPE_VMAR:      return "VMAR";
        default:                    return "UNKNOWN";
    }
}

static void default_object_destroy(ab_object_t *obj) {
    dprintf(SPEW, "[object] Destroying %s dispatcher (koid=%lu) at %p\n",
            ab_obj_type_name(obj->type), obj->koid, (void *)obj);
    kfree(obj);
}

ab_object_t *ab_object_alloc(size_t struct_size, ab_obj_type_t type,
                             ab_obj_hook_t on_zero_handles, ab_obj_hook_t destroy) {
    if (struct_size < sizeof(ab_object_t)) {
        struct_size = sizeof(ab_object_t);
    }
    ab_object_t *obj = (ab_object_t *)kzalloc(struct_size);
    if (!obj) return NULL;

    obj->koid            = ab_koid_alloc();
    obj->type            = type;
    obj->ref_count       = 1;
    obj->handle_count    = 0;
    obj->on_zero_handles = on_zero_handles;
    obj->destroy         = destroy ? destroy : default_object_destroy;
    return obj;
}

void ab_object_add_ref(ab_object_t *obj) {
    if (!obj) return;
    __atomic_fetch_add(&obj->ref_count, 1, __ATOMIC_RELAXED);
}

void ab_object_release(ab_object_t *obj) {
    if (!obj) return;
    int32_t old = __atomic_fetch_sub(&obj->ref_count, 1, __ATOMIC_ACQ_REL);
    if (old == 1) {
        if (obj->destroy) {
            obj->destroy(obj);
        }
    }
}

/*
 * Handle encoding (modeled after Zircon's MapHandleToValue / MapValueToHandle):
 *   bits [31:28] = 0x5 (fixed non-zero sentinel so handle 0 is always invalid)
 *   bits [23:8]  = 16-bit slot generation counter (detects use-after-close!)
 *   bits [7:0]   = slot index (0 .. MAX_HANDLES_PER_PROCESS - 1)
 */
static ab_handle_t make_handle_value(uint32_t slot, uint32_t gen) {
    return (0x5U << 28) | ((gen & 0xFFFFU) << 8) | (slot & 0xFFU);
}

static int decode_handle_value(ab_handle_t h, uint32_t *out_slot, uint32_t *out_gen) {
    if ((h >> 28) != 0x5U) return -1;
    uint32_t slot = h & 0xFFU;
    if (slot >= MAX_HANDLES_PER_PROCESS) return -1;
    *out_slot = slot;
    *out_gen  = (h >> 8) & 0xFFFFU;
    return 0;
}

ab_process_t *get_current_process(void) {
    thread_t *cur = get_current_thread();
    return cur ? (ab_process_t *)cur->process_obj : NULL;
}

ab_process_t *ab_process_create_kernel(const char *name, uint16_t asid) {
    ab_process_t *proc = (ab_process_t *)ab_object_alloc(
        sizeof(ab_process_t), AB_OBJ_TYPE_PROCESS, NULL, NULL);
    if (!proc) return NULL;

    size_t i = 0;
    while (name && name[i] && i + 1 < sizeof(proc->name)) {
        proc->name[i] = name[i];
        i++;
    }
    proc->name[i] = '\0';

    proc->aspace = vmm_create_user_aspace(asid);
    proc->handle_lock.value = 0;
    for (int s = 0; s < MAX_HANDLES_PER_PROCESS; s++) {
        proc->handles[s].obj        = NULL;
        proc->handles[s].rights     = 0;
        proc->handles[s].generation = 1;
    }
    return proc;
}

/* Allocates a handle in proc's table, adopting the caller's reference on obj
 * or adding a handle reference. Here handle_alloc increments both ref_count and
 * handle_count so callers can release their creation ref or keep it symmetric.
 */
ab_status_t handle_alloc(ab_process_t *proc, ab_object_t *obj, ab_rights_t rights,
                         ab_handle_t *out_handle) {
    if (!proc || !obj || !out_handle) return AB_ERR_INVALID_ARGS;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&proc->handle_lock, &irqstate);

    for (uint32_t slot = 0; slot < MAX_HANDLES_PER_PROCESS; slot++) {
        if (proc->handles[slot].obj == NULL) {
            ab_object_add_ref(obj);
            __atomic_fetch_add(&obj->handle_count, 1, __ATOMIC_RELAXED);

            uint32_t gen = proc->handles[slot].generation;
            proc->handles[slot].obj    = obj;
            proc->handles[slot].rights = rights;
            *out_handle = make_handle_value(slot, gen);

            spin_lock_irqrestore(&proc->handle_lock, irqstate);
            return AB_OK;
        }
    }

    spin_lock_irqrestore(&proc->handle_lock, irqstate);
    return AB_ERR_NO_RESOURCES;
}

ab_status_t handle_lookup(ab_process_t *proc, ab_handle_t h, ab_obj_type_t expected_type,
                          ab_rights_t required_rights, ab_object_t **out_obj,
                          ab_rights_t *out_rights) {
    if (!proc || h == AB_HANDLE_INVALID) return AB_ERR_BAD_HANDLE;

    uint32_t slot = 0, gen = 0;
    if (decode_handle_value(h, &slot, &gen) < 0) {
        return AB_ERR_BAD_HANDLE;
    }

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&proc->handle_lock, &irqstate);

    ab_handle_entry_t *entry = &proc->handles[slot];
    if (!entry->obj || entry->generation != gen) {
        spin_lock_irqrestore(&proc->handle_lock, irqstate);
        return AB_ERR_BAD_HANDLE;
    }

    ab_object_t *obj = entry->obj;
    ab_rights_t rights = entry->rights;

    if (expected_type != AB_OBJ_TYPE_NONE && obj->type != expected_type) {
        spin_lock_irqrestore(&proc->handle_lock, irqstate);
        return AB_ERR_WRONG_TYPE;
    }

    if ((rights & required_rights) != required_rights) {
        spin_lock_irqrestore(&proc->handle_lock, irqstate);
        return AB_ERR_ACCESS_DENIED;
    }

    ab_object_add_ref(obj);
    spin_lock_irqrestore(&proc->handle_lock, irqstate);

    if (out_obj)    *out_obj    = obj;
    if (out_rights) *out_rights = rights;
    return AB_OK;
}

ab_status_t handle_close(ab_process_t *proc, ab_handle_t h) {
    if (!proc || h == AB_HANDLE_INVALID) return AB_ERR_BAD_HANDLE;

    uint32_t slot = 0, gen = 0;
    if (decode_handle_value(h, &slot, &gen) < 0) {
        return AB_ERR_BAD_HANDLE;
    }

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&proc->handle_lock, &irqstate);

    ab_handle_entry_t *entry = &proc->handles[slot];
    if (!entry->obj || entry->generation != gen) {
        spin_lock_irqrestore(&proc->handle_lock, irqstate);
        return AB_ERR_BAD_HANDLE;
    }

    ab_object_t *obj = entry->obj;
    entry->obj        = NULL;
    entry->rights     = 0;
    entry->generation = (entry->generation + 1) & 0xFFFFU;
    if (entry->generation == 0) entry->generation = 1;

    spin_lock_irqrestore(&proc->handle_lock, irqstate);

    int32_t old_hc = __atomic_fetch_sub(&obj->handle_count, 1, __ATOMIC_ACQ_REL);
    if (old_hc == 1 && obj->on_zero_handles) {
        obj->on_zero_handles(obj);
    }
    ab_object_release(obj);
    return AB_OK;
}

ab_status_t handle_duplicate(ab_process_t *proc, ab_handle_t h, ab_rights_t new_rights,
                             ab_handle_t *out_new_handle) {
    if (!out_new_handle) return AB_ERR_INVALID_ARGS;

    ab_object_t *obj = NULL;
    ab_rights_t cur_rights = 0;
    ab_status_t st = handle_lookup(proc, h, AB_OBJ_TYPE_NONE, AB_RIGHT_DUPLICATE,
                                   &obj, &cur_rights);
    if (st != AB_OK) return st;

    if (new_rights == AB_RIGHT_SAME_RIGHTS) {
        new_rights = cur_rights;
    } else if ((new_rights & ~cur_rights) != 0) {
        /* Cannot escalate rights when duplicating a handle! */
        ab_object_release(obj);
        return AB_ERR_INVALID_ARGS;
    }

    st = handle_alloc(proc, obj, new_rights, out_new_handle);
    ab_object_release(obj);
    return st;
}

ab_status_t handle_replace(ab_process_t *proc, ab_handle_t h, ab_rights_t new_rights,
                           ab_handle_t *out_new_handle) {
    if (!out_new_handle) return AB_ERR_INVALID_ARGS;

    ab_object_t *obj = NULL;
    ab_rights_t cur_rights = 0;
    ab_status_t st = handle_lookup(proc, h, AB_OBJ_TYPE_NONE, AB_RIGHT_NONE,
                                   &obj, &cur_rights);
    if (st != AB_OK) return st;

    if (new_rights == AB_RIGHT_SAME_RIGHTS) {
        new_rights = cur_rights;
    } else if ((new_rights & ~cur_rights) != 0) {
        ab_object_release(obj);
        return AB_ERR_INVALID_ARGS;
    }

    st = handle_alloc(proc, obj, new_rights, out_new_handle);
    if (st == AB_OK) {
        handle_close(proc, h);
    }
    ab_object_release(obj);
    return st;
}

static void log_on_zero_handles(ab_object_t *obj) {
    dprintf(INFO, "[kernel] LogDispatcher (koid=%lu) reached 0 open handles!\n", obj->koid);
}

ab_status_t ab_log_dispatcher_create(uint32_t flags, ab_object_t **out_obj) {
    ab_log_dispatcher_t *log = (ab_log_dispatcher_t *)ab_object_alloc(
        sizeof(ab_log_dispatcher_t), AB_OBJ_TYPE_LOG, log_on_zero_handles, NULL);
    if (!log) return AB_ERR_NO_MEMORY;
    log->flags = flags;
    *out_obj = &log->base;
    return AB_OK;
}
