/* =========================================================================
 * object.c - Zircon Dispatcher, StateTracker, Event & EventPair
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
                             ab_signals_t initial_signals,
                             ab_obj_hook_t on_zero_handles, ab_obj_hook_t destroy) {
    if (struct_size < sizeof(ab_object_t)) {
        struct_size = sizeof(ab_object_t);
    }
    ab_object_t *obj = (ab_object_t *)kzalloc(struct_size);
    if (!obj) return NULL;

    obj->koid                  = ab_koid_alloc();
    obj->related_koid          = AB_KOID_INVALID;
    obj->type                  = type;
    obj->ref_count             = 1;
    obj->handle_count          = 0;
    obj->state_tracker.lock.value = 0;
    obj->state_tracker.signals = initial_signals;
    obj->state_tracker.head    = NULL;
    obj->state_tracker.tail    = NULL;
    obj->on_zero_handles       = on_zero_handles;
    obj->destroy               = destroy ? destroy : default_object_destroy;
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

/* =========================================================================
 * StateTracker Implementation (models Zircon's StateTracker)
 * ========================================================================= */
void state_tracker_update_signals(ab_object_t *obj,
                                  ab_signals_t clear_mask,
                                  ab_signals_t set_mask) {
    ab_state_tracker_t *st = &obj->state_tracker;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&st->lock, &irqstate);

    ab_signals_t old_sigs = st->signals;
    ab_signals_t new_sigs = (old_sigs & ~clear_mask) | set_mask;
    st->signals = new_sigs;

    if (new_sigs != old_sigs) {
        ab_wait_observer_t *obs = st->head;
        while (obs != NULL) {
            ab_wait_observer_t *next = obs->next;
            if (new_sigs & obs->watched_signals) {
                if (obs->is_async_port && obs->port_obj) {
                    /* Unlink one-shot PortObserver and enqueue packet */
                    if (obs->prev) obs->prev->next = obs->next;
                    else st->head = obs->next;
                    if (obs->next) obs->next->prev = obs->prev;
                    else st->tail = obs->prev;

                    ab_port_packet_t pkt;
                    pkt.key       = obs->key;
                    pkt.type      = AB_PKT_TYPE_SIGNAL_ONE;
                    pkt.status    = AB_OK;
                    pkt.trigger   = obs->watched_signals;
                    pkt.observed  = new_sigs;
                    pkt.timestamp = current_time_ns();
                    pkt.user_val  = obj->koid;

                    ab_port_queue(obs->port_obj, &pkt);
                    ab_object_release(obs->port_obj);
                    kfree(obs);
                } else {
                    obs->wakeup_reasons = new_sigs;
                    event_signal(&obs->event);
                }
            }
            obs = next;
        }
    }

    spin_lock_irqrestore(&st->lock, irqstate);
}

ab_signals_t state_tracker_get_signals(ab_object_t *obj) {
    return __atomic_load_n(&obj->state_tracker.signals, __ATOMIC_SEQ_CST);
}

ab_status_t state_tracker_wait_one(ab_object_t *obj,
                                   ab_signals_t watched_signals,
                                   ab_time_t deadline_ns,
                                   ab_signals_t *observed_out) {
    ab_state_tracker_t *st = &obj->state_tracker;
    ab_wait_observer_t obs;
    obs.prev            = NULL;
    obs.next            = NULL;
    obs.watched_signals = watched_signals;
    obs.wakeup_reasons  = 0;
    obs.is_async_port   = 0;
    obs.port_obj        = NULL;
    obs.key             = 0;
    event_init(&obs.event, 0, 0);

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&st->lock, &irqstate);

    /* Fast path: watched signals are already active */
    if (st->signals & watched_signals) {
        ab_signals_t cur = st->signals;
        spin_lock_irqrestore(&st->lock, irqstate);
        if (observed_out) *observed_out = cur;
        return AB_OK;
    }

    if (!st->tail) {
        st->head = st->tail = &obs;
    } else {
        obs.prev = st->tail;
        st->tail->next = &obs;
        st->tail = &obs;
    }
    spin_lock_irqrestore(&st->lock, irqstate);

    int rc = event_wait_deadline(&obs.event, deadline_ns);

    spin_lock_irqsave(&st->lock, &irqstate);
    if (obs.prev) obs.prev->next = obs.next;
    else st->head = obs.next;
    if (obs.next) obs.next->prev = obs.prev;
    else st->tail = obs.prev;
    ab_signals_t final_sigs = obs.wakeup_reasons ? obs.wakeup_reasons : st->signals;
    spin_lock_irqrestore(&st->lock, irqstate);

    if (observed_out) *observed_out = final_sigs;
    return (rc == 0) ? AB_OK : AB_ERR_TIMED_OUT;
}

/* =========================================================================
 * Handle Table Implementation
 * ========================================================================= */
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
        sizeof(ab_process_t), AB_OBJ_TYPE_PROCESS, AB_SIGNAL_NONE, NULL, NULL);
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

/* =========================================================================
 * Dispatchers: LogDispatcher, EventDispatcher, EventPairDispatcher
 * ========================================================================= */
static void log_on_zero_handles(ab_object_t *obj) {
    dprintf(INFO, "[kernel] LogDispatcher (koid=%lu) reached 0 open handles!\n", obj->koid);
}

ab_status_t ab_log_dispatcher_create(uint32_t flags, ab_object_t **out_obj) {
    ab_log_dispatcher_t *log = (ab_log_dispatcher_t *)ab_object_alloc(
        sizeof(ab_log_dispatcher_t), AB_OBJ_TYPE_LOG, AB_SIGNAL_WRITABLE,
        log_on_zero_handles, NULL);
    if (!log) return AB_ERR_NO_MEMORY;
    log->flags = flags;
    *out_obj = &log->base;
    return AB_OK;
}

ab_status_t ab_event_dispatcher_create(uint32_t options, ab_object_t **out_obj) {
    ab_event_dispatcher_t *ev = (ab_event_dispatcher_t *)ab_object_alloc(
        sizeof(ab_event_dispatcher_t), AB_OBJ_TYPE_EVENT, AB_SIGNAL_NONE,
        NULL, NULL);
    if (!ev) return AB_ERR_NO_MEMORY;
    ev->options = options;
    *out_obj = &ev->base;
    return AB_OK;
}

static void eventpair_on_zero_handles(ab_object_t *obj) {
    ab_eventpair_dispatcher_t *self = (ab_eventpair_dispatcher_t *)obj;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&self->peer_lock, &irqstate);
    ab_eventpair_dispatcher_t *peer = self->peer;
    self->peer = NULL;
    spin_lock_irqrestore(&self->peer_lock, irqstate);

    if (peer) {
        spin_lock_irqsave(&peer->peer_lock, &irqstate);
        peer->peer = NULL;
        spin_lock_irqrestore(&peer->peer_lock, irqstate);

        dprintf(INFO, "[eventpair] Endpoint (koid=%lu) closed -> asserting AB_SIGNAL_PEER_CLOSED on peer (koid=%lu)\n",
                self->base.koid, peer->base.koid);
        state_tracker_update_signals(&peer->base, 0, AB_SIGNAL_PEER_CLOSED);
        ab_object_release(&peer->base);
        ab_object_release(&self->base);
    }
}

ab_status_t ab_eventpair_dispatcher_create(ab_object_t **out_obj0,
                                           ab_object_t **out_obj1) {
    ab_eventpair_dispatcher_t *ep0 = (ab_eventpair_dispatcher_t *)ab_object_alloc(
        sizeof(ab_eventpair_dispatcher_t), AB_OBJ_TYPE_EVENTPAIR, AB_SIGNAL_NONE,
        eventpair_on_zero_handles, NULL);
    ab_eventpair_dispatcher_t *ep1 = (ab_eventpair_dispatcher_t *)ab_object_alloc(
        sizeof(ab_eventpair_dispatcher_t), AB_OBJ_TYPE_EVENTPAIR, AB_SIGNAL_NONE,
        eventpair_on_zero_handles, NULL);
    if (!ep0 || !ep1) {
        if (ep0) ab_object_release(&ep0->base);
        if (ep1) ab_object_release(&ep1->base);
        return AB_ERR_NO_MEMORY;
    }

    ep0->base.related_koid = ep1->base.koid;
    ep1->base.related_koid = ep0->base.koid;
    ep0->peer_lock.value   = 0;
    ep1->peer_lock.value   = 0;
    ep0->peer              = ep1;
    ep1->peer              = ep0;
    ab_object_add_ref(&ep0->base);
    ab_object_add_ref(&ep1->base);

    *out_obj0 = &ep0->base;
    *out_obj1 = &ep1->base;
    return AB_OK;
}

ab_status_t ab_eventpair_signal_peer(ab_object_t *obj,
                                     ab_signals_t clear_mask,
                                     ab_signals_t set_mask) {
    ab_eventpair_dispatcher_t *self = (ab_eventpair_dispatcher_t *)obj;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&self->peer_lock, &irqstate);
    ab_eventpair_dispatcher_t *peer = self->peer;
    if (peer) {
        ab_object_add_ref(&peer->base);
    }
    spin_lock_irqrestore(&self->peer_lock, irqstate);

    if (!peer) {
        return AB_ERR_PEER_CLOSED;
    }

    state_tracker_update_signals(&peer->base, clear_mask, set_mask);
    ab_object_release(&peer->base);
        ab_object_release(&self->base);
    return AB_OK;
}

/* =========================================================================
 * PortDispatcher & state_tracker_wait_async (Lesson 09)
 * ========================================================================= */
static void port_destroy(ab_object_t *obj) {
    dprintf(SPEW, "[object] Destroying PORT dispatcher (koid=%lu) at %p\n",
            obj->koid, (void *)obj);
    kfree(obj);
}

ab_status_t ab_port_dispatcher_create(uint32_t options, ab_object_t **out_obj) {
    ab_port_dispatcher_t *port = (ab_port_dispatcher_t *)ab_object_alloc(
        sizeof(ab_port_dispatcher_t), AB_OBJ_TYPE_PORT, AB_SIGNAL_WRITABLE,
        NULL, port_destroy);
    if (!port) return AB_ERR_NO_MEMORY;

    port->options    = options;
    port->lock.value = 0;
    port->head       = 0;
    port->tail       = 0;
    port->count      = 0;
    event_init(&port->readable_event, 0, EVENT_FLAG_AUTOUNSIGNAL);

    *out_obj = &port->base;
    return AB_OK;
}

ab_status_t ab_port_queue(ab_object_t *port_obj, const ab_port_packet_t *pkt) {
    if (!port_obj || port_obj->type != AB_OBJ_TYPE_PORT || !pkt) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_port_dispatcher_t *port = (ab_port_dispatcher_t *)port_obj;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&port->lock, &irqstate);

    if (port->count >= PORT_QUEUE_CAPACITY) {
        spin_lock_irqrestore(&port->lock, irqstate);
        return AB_ERR_NO_RESOURCES;
    }

    port->packets[port->tail] = *pkt;
    port->tail = (port->tail + 1) % PORT_QUEUE_CAPACITY;
    port->count++;

    spin_lock_irqrestore(&port->lock, irqstate);

    state_tracker_update_signals(&port->base, 0, AB_SIGNAL_READABLE);
    event_signal(&port->readable_event);
    return AB_OK;
}

ab_status_t ab_port_wait(ab_object_t *port_obj, ab_time_t deadline_ns,
                         ab_port_packet_t *out_pkt) {
    if (!port_obj || port_obj->type != AB_OBJ_TYPE_PORT || !out_pkt) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_port_dispatcher_t *port = (ab_port_dispatcher_t *)port_obj;

    for (;;) {
        spin_lock_saved_state_t irqstate;
        spin_lock_irqsave(&port->lock, &irqstate);

        if (port->count > 0) {
            *out_pkt = port->packets[port->head];
            port->head = (port->head + 1) % PORT_QUEUE_CAPACITY;
            port->count--;
            int empty = (port->count == 0);
            spin_lock_irqrestore(&port->lock, irqstate);

            if (empty) {
                state_tracker_update_signals(&port->base, AB_SIGNAL_READABLE, 0);
            }
            return AB_OK;
        }

        spin_lock_irqrestore(&port->lock, irqstate);

        int rc = event_wait_deadline(&port->readable_event, deadline_ns);
        if (rc != AB_OK) {
            return AB_ERR_TIMED_OUT;
        }
    }
}

ab_status_t state_tracker_wait_async(ab_object_t *obj, ab_object_t *port_obj,
                                     uint64_t key, ab_signals_t watched_signals) {
    if (!obj || !port_obj || port_obj->type != AB_OBJ_TYPE_PORT) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_state_tracker_t *st = &obj->state_tracker;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&st->lock, &irqstate);

    /* If signals are already active, immediately enqueue a packet on port */
    if (st->signals & watched_signals) {
        ab_signals_t cur = st->signals;
        spin_lock_irqrestore(&st->lock, irqstate);

        ab_port_packet_t pkt;
        pkt.key       = key;
        pkt.type      = AB_PKT_TYPE_SIGNAL_ONE;
        pkt.status    = AB_OK;
        pkt.trigger   = watched_signals;
        pkt.observed  = cur;
        pkt.timestamp = current_time_ns();
        pkt.user_val  = obj->koid;
        return ab_port_queue(port_obj, &pkt);
    }

    ab_wait_observer_t *obs = (ab_wait_observer_t *)kzalloc(sizeof(ab_wait_observer_t));
    if (!obs) {
        spin_lock_irqrestore(&st->lock, irqstate);
        return AB_ERR_NO_MEMORY;
    }

    obs->watched_signals = watched_signals;
    obs->is_async_port   = 1;
    obs->port_obj        = port_obj;
    obs->key             = key;
    ab_object_add_ref(port_obj);

    if (!st->tail) {
        st->head = st->tail = obs;
    } else {
        obs->prev = st->tail;
        st->tail->next = obs;
        st->tail = obs;
    }

    spin_lock_irqrestore(&st->lock, irqstate);
    return AB_OK;
}

/* =========================================================================
 * Lesson 10: Handle Transfer & ChannelDispatcher Implementation
 * ========================================================================= */
ab_status_t handle_take_for_transfer(ab_process_t *proc, ab_handle_t h,
                                     ab_object_t *channel_self,
                                     ab_object_t **out_obj, ab_rights_t *out_rights) {
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

    if ((entry->rights & AB_RIGHT_TRANSFER) == 0) {
        spin_lock_irqrestore(&proc->handle_lock, irqstate);
        return AB_ERR_ACCESS_DENIED;
    }

    /* Zircon rule: cannot transfer a channel handle over itself */
    if (entry->obj == channel_self) {
        spin_lock_irqrestore(&proc->handle_lock, irqstate);
        return AB_ERR_NOT_SUPPORTED;
    }

    /* Detach from sender table while keeping obj->ref_count & obj->handle_count intact */
    *out_obj    = entry->obj;
    *out_rights = entry->rights;

    entry->obj        = NULL;
    entry->rights     = 0;
    entry->generation = (entry->generation + 1) & 0xFFFFU;
    if (entry->generation == 0) entry->generation = 1;

    spin_lock_irqrestore(&proc->handle_lock, irqstate);
    return AB_OK;
}

ab_status_t handle_install_transferred(ab_process_t *proc, ab_object_t *obj,
                                       ab_rights_t rights, ab_handle_t *out_handle) {
    if (!proc || !obj || !out_handle) return AB_ERR_INVALID_ARGS;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&proc->handle_lock, &irqstate);

    for (uint32_t slot = 0; slot < MAX_HANDLES_PER_PROCESS; slot++) {
        if (proc->handles[slot].obj == NULL) {
            /* Adopt the in-transit ref_count and handle_count directly */
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

static void message_packet_destroy(ab_message_packet_t *pkt) {
    if (!pkt) return;
    for (uint32_t i = 0; i < pkt->num_handles; i++) {
        ab_object_t *obj = pkt->objs[i];
        if (obj) {
            int32_t old_hc = __atomic_fetch_sub(&obj->handle_count, 1, __ATOMIC_ACQ_REL);
            if (old_hc == 1 && obj->on_zero_handles) {
                obj->on_zero_handles(obj);
            }
            ab_object_release(obj);
        }
    }
    kfree(pkt);
}

static void channel_destroy(ab_object_t *obj) {
    ab_channel_dispatcher_t *ch = (ab_channel_dispatcher_t *)obj;
    ab_message_packet_t *pkt = ch->msg_head;
    while (pkt) {
        ab_message_packet_t *next = pkt->next;
        message_packet_destroy(pkt);
        pkt = next;
    }
    dprintf(SPEW, "[object] Destroying CHANNEL dispatcher (koid=%lu) at %p\n",
            obj->koid, (void *)obj);
    kfree(ch);
}

static void channel_on_zero_handles(ab_object_t *obj) {
    ab_channel_dispatcher_t *self = (ab_channel_dispatcher_t *)obj;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&self->lock, &irqstate);
    ab_channel_dispatcher_t *peer = self->peer;
    self->peer = NULL;
    spin_lock_irqrestore(&self->lock, irqstate);

    if (peer) {
        spin_lock_irqsave(&peer->lock, &irqstate);
        peer->peer = NULL;
        spin_lock_irqrestore(&peer->lock, irqstate);

        state_tracker_update_signals(&peer->base, AB_SIGNAL_WRITABLE, AB_SIGNAL_PEER_CLOSED);
        ab_object_release(&peer->base);
        ab_object_release(&self->base);
    }
}

ab_status_t ab_channel_dispatcher_create(ab_object_t **out_obj0,
                                         ab_object_t **out_obj1) {
    ab_channel_dispatcher_t *ch0 = (ab_channel_dispatcher_t *)ab_object_alloc(
        sizeof(ab_channel_dispatcher_t), AB_OBJ_TYPE_CHANNEL, AB_SIGNAL_WRITABLE,
        channel_on_zero_handles, channel_destroy);
    ab_channel_dispatcher_t *ch1 = (ab_channel_dispatcher_t *)ab_object_alloc(
        sizeof(ab_channel_dispatcher_t), AB_OBJ_TYPE_CHANNEL, AB_SIGNAL_WRITABLE,
        channel_on_zero_handles, channel_destroy);
    if (!ch0 || !ch1) {
        if (ch0) ab_object_release(&ch0->base);
        if (ch1) ab_object_release(&ch1->base);
        return AB_ERR_NO_MEMORY;
    }

    ch0->base.related_koid = ch1->base.koid;
    ch1->base.related_koid = ch0->base.koid;
    ch0->lock.value = 0;
    ch1->lock.value = 0;
    ch0->peer = ch1;
    ch1->peer = ch0;
    ab_object_add_ref(&ch0->base);
    ab_object_add_ref(&ch1->base);

    *out_obj0 = &ch0->base;
    *out_obj1 = &ch1->base;
    return AB_OK;
}

ab_status_t ab_channel_write(ab_process_t *sender_proc, ab_object_t *ch_obj,
                             const void *bytes, uint32_t num_bytes,
                             const ab_handle_t *handles, uint32_t num_handles) {
    if (!ch_obj || ch_obj->type != AB_OBJ_TYPE_CHANNEL) return AB_ERR_WRONG_TYPE;
    if (num_bytes > CHANNEL_MAX_MSG_BYTES || num_handles > CHANNEL_MAX_MSG_HANDLES) {
        return AB_ERR_OUT_OF_RANGE;
    }
    if ((num_bytes > 0 && !bytes) || (num_handles > 0 && (!handles || !sender_proc))) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_channel_dispatcher_t *self = (ab_channel_dispatcher_t *)ch_obj;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&self->lock, &irqstate);
    ab_channel_dispatcher_t *peer = self->peer;
    if (peer) ab_object_add_ref(&peer->base);
    spin_lock_irqrestore(&self->lock, irqstate);

    if (!peer) return AB_ERR_PEER_CLOSED;

    ab_message_packet_t *pkt = (ab_message_packet_t *)kzalloc(sizeof(ab_message_packet_t));
    if (!pkt) {
        ab_object_release(&peer->base);
        return AB_ERR_NO_MEMORY;
    }

    pkt->num_bytes   = num_bytes;
    pkt->num_handles = 0;
    const uint8_t *src = (const uint8_t *)bytes;
    for (uint32_t i = 0; i < num_bytes; i++) {
        pkt->data[i] = src[i];
    }

    /* Detach capabilities from sender process and attach to MessagePacket */
    for (uint32_t i = 0; i < num_handles; i++) {
        ab_object_t *transferred_obj = NULL;
        ab_rights_t  transferred_rights = 0;
        ab_status_t st = handle_take_for_transfer(sender_proc, handles[i], ch_obj,
                                                  &transferred_obj, &transferred_rights);
        if (st != AB_OK) {
            message_packet_destroy(pkt);
            ab_object_release(&peer->base);
            return st;
        }
        pkt->objs[i]   = transferred_obj;
        pkt->rights[i] = transferred_rights;
        pkt->num_handles++;
    }

    spin_lock_irqsave(&peer->lock, &irqstate);
    if (!peer->msg_tail) {
        peer->msg_head = peer->msg_tail = pkt;
    } else {
        peer->msg_tail->next = pkt;
        peer->msg_tail = pkt;
    }
    peer->msg_count++;
    spin_lock_irqrestore(&peer->lock, irqstate);

    state_tracker_update_signals(&peer->base, 0, AB_SIGNAL_READABLE);
    ab_object_release(&peer->base);
    return AB_OK;
}

ab_status_t ab_channel_read(ab_process_t *receiver_proc, ab_object_t *ch_obj,
                            void *bytes, ab_handle_t *handles,
                            uint32_t max_bytes, uint32_t max_handles,
                            uint32_t *actual_bytes, uint32_t *actual_handles) {
    if (!ch_obj || ch_obj->type != AB_OBJ_TYPE_CHANNEL) return AB_ERR_WRONG_TYPE;
    ab_channel_dispatcher_t *self = (ab_channel_dispatcher_t *)ch_obj;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&self->lock, &irqstate);

    ab_message_packet_t *pkt = self->msg_head;
    if (!pkt) {
        int peer_closed = (self->peer == NULL);
        spin_lock_irqrestore(&self->lock, irqstate);
        return peer_closed ? AB_ERR_PEER_CLOSED : AB_ERR_SHOULD_WAIT;
    }

    if (actual_bytes)   *actual_bytes   = pkt->num_bytes;
    if (actual_handles) *actual_handles = pkt->num_handles;

    if (pkt->num_bytes > max_bytes || pkt->num_handles > max_handles) {
        spin_lock_irqrestore(&self->lock, irqstate);
        return AB_ERR_BUFFER_TOO_SMALL;
    }

    self->msg_head = pkt->next;
    if (!self->msg_head) self->msg_tail = NULL;
    self->msg_count--;
    int now_empty = (self->msg_count == 0);
    spin_lock_irqrestore(&self->lock, irqstate);

    if (now_empty) {
        state_tracker_update_signals(&self->base, AB_SIGNAL_READABLE, 0);
    }

    uint8_t *dst = (uint8_t *)bytes;
    for (uint32_t i = 0; i < pkt->num_bytes; i++) {
        dst[i] = pkt->data[i];
    }

    /* Install transferred capabilities into the receiver process's handle table! */
    for (uint32_t i = 0; i < pkt->num_handles; i++) {
        ab_handle_t new_h = AB_HANDLE_INVALID;
        ab_status_t st = handle_install_transferred(receiver_proc, pkt->objs[i],
                                                    pkt->rights[i], &new_h);
        if (st == AB_OK) {
            handles[i]   = new_h;
            pkt->objs[i] = NULL; /* Ownership transferred to receiver handle table */
        }
    }

    message_packet_destroy(pkt);
    return AB_OK;
}
