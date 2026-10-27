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
    state_tracker_update_signals(&peer->base, 0, AB_SIGNAL_READABLE);
    spin_lock_irqrestore(&peer->lock, irqstate);

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
    if (self->msg_count == 0) {
        state_tracker_update_signals(&self->base, AB_SIGNAL_READABLE, 0);
    }
    spin_lock_irqrestore(&self->lock, irqstate);

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

/* =========================================================================
 * Lesson 11: VmObjectDispatcher (VMO) & VmAddressRegionDispatcher (VMAR)
 * ========================================================================= */
#include "pmm.h"

static void vmo_destroy(ab_object_t *obj) {
    ab_vmo_dispatcher_t *vmo = (ab_vmo_dispatcher_t *)obj;
    if (!vmo->is_physical) {
        for (uint32_t i = 0; i < vmo->num_pages; i++) {
            if (vmo->paddrs[i] != 0) {
                pmm_free_page(vmo->paddrs[i]);
                vmo->paddrs[i] = 0;
            }
        }
    }
    dprintf(SPEW, "[object] Destroying VMO dispatcher (koid=%lu, pages=%u) at %p\n",
            obj->koid, vmo->num_pages, (void *)obj);
    kfree(vmo);
}

ab_status_t ab_vmo_dispatcher_create(size_t size_bytes, uint32_t options,
                                     ab_object_t **out_obj) {
    (void)options;
    size_t aligned = (size_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (aligned == 0) aligned = PAGE_SIZE;
    uint32_t pages = (uint32_t)(aligned / PAGE_SIZE);
    if (pages > VMO_MAX_PAGES) return AB_ERR_OUT_OF_RANGE;

    ab_vmo_dispatcher_t *vmo = (ab_vmo_dispatcher_t *)ab_object_alloc(
        sizeof(ab_vmo_dispatcher_t), AB_OBJ_TYPE_VMO, AB_SIGNAL_NONE,
        NULL, vmo_destroy);
    if (!vmo) return AB_ERR_NO_MEMORY;

    vmo->lock.value  = 0;
    vmo->size_bytes  = aligned;
    vmo->num_pages   = pages;
    vmo->is_physical = 0;
    ab_object_set_name(&vmo->base, "vmo-anon", 8);
    *out_obj = &vmo->base;
    return AB_OK;
}

ab_status_t ab_vmo_get_or_alloc_page(ab_vmo_dispatcher_t *vmo, uint32_t page_idx,
                                     int alloc_if_missing, paddr_t *out_pa) {
    if (!vmo || page_idx >= vmo->num_pages || !out_pa) return AB_ERR_OUT_OF_RANGE;

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&vmo->lock, &irqstate);

    if (vmo->paddrs[page_idx] == 0 && alloc_if_missing && !vmo->is_physical) {
        paddr_t pa = 0;
        if (pmm_alloc_page(VM_PAGE_STATE_OBJECT, &pa) == NULL) {
            spin_lock_irqrestore(&vmo->lock, irqstate);
            return AB_ERR_NO_MEMORY;
        }
        uint8_t *kva = (uint8_t *)paddr_to_physmap(pa);
        for (size_t b = 0; b < PAGE_SIZE; b++) kva[b] = 0;
        vmo->paddrs[page_idx] = pa;
    }

    *out_pa = vmo->paddrs[page_idx];
    spin_lock_irqrestore(&vmo->lock, irqstate);
    return (*out_pa != 0) ? AB_OK : AB_ERR_NOT_FOUND;
}

ab_status_t ab_vmo_read(ab_object_t *vmo_obj, void *buf, uint64_t offset, size_t len) {
    if (!vmo_obj || vmo_obj->type != AB_OBJ_TYPE_VMO || !buf) return AB_ERR_INVALID_ARGS;
    ab_vmo_dispatcher_t *vmo = (ab_vmo_dispatcher_t *)vmo_obj;
    if (offset + len > vmo->size_bytes) return AB_ERR_OUT_OF_RANGE;

    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        uint64_t pos = offset + i;
        uint32_t pg  = (uint32_t)(pos / PAGE_SIZE);
        uint32_t off = (uint32_t)(pos % PAGE_SIZE);
        paddr_t pa = 0;
        if (ab_vmo_get_or_alloc_page(vmo, pg, 0, &pa) == AB_OK && pa != 0) {
            const uint8_t *kva = (const uint8_t *)paddr_to_physmap(pa);
            dst[i] = kva[off];
        } else {
            dst[i] = 0;
        }
    }
    return AB_OK;
}

ab_status_t ab_vmo_write(ab_object_t *vmo_obj, const void *buf, uint64_t offset, size_t len) {
    if (!vmo_obj || vmo_obj->type != AB_OBJ_TYPE_VMO || !buf) return AB_ERR_INVALID_ARGS;
    ab_vmo_dispatcher_t *vmo = (ab_vmo_dispatcher_t *)vmo_obj;
    if (offset + len > vmo->size_bytes) return AB_ERR_OUT_OF_RANGE;

    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        uint64_t pos = offset + i;
        uint32_t pg  = (uint32_t)(pos / PAGE_SIZE);
        uint32_t off = (uint32_t)(pos % PAGE_SIZE);
        paddr_t pa = 0;
        ab_status_t st = ab_vmo_get_or_alloc_page(vmo, pg, 1, &pa);
        if (st != AB_OK) return st;
        uint8_t *kva = (uint8_t *)paddr_to_physmap(pa);
        kva[off] = src[i];
    }
    return AB_OK;
}

ab_status_t ab_vmar_dispatcher_create(ab_process_t *proc, ab_object_t **out_obj) {
    ab_vmar_dispatcher_t *vmar = (ab_vmar_dispatcher_t *)ab_object_alloc(
        sizeof(ab_vmar_dispatcher_t), AB_OBJ_TYPE_VMAR, AB_SIGNAL_NONE,
        NULL, NULL);
    if (!vmar) return AB_ERR_NO_MEMORY;
    vmar->proc    = proc;
    vmar->base_va = 0x10000000ULL;
    vmar->size    = 0x30000000ULL;
    if (proc->next_map_va == 0) {
        proc->next_map_va = vmar->base_va;
    }
    *out_obj = &vmar->base;
    return AB_OK;
}

static uint32_t vm_perms_to_mmu_flags(uint32_t vm_perms, int is_device) {
    uint32_t flags = MMU_FLAG_PERM_USER;
    if (vm_perms & AB_VM_PERM_READ)    flags |= MMU_FLAG_PERM_READ;
    if (vm_perms & AB_VM_PERM_WRITE)   flags |= MMU_FLAG_PERM_WRITE;
    if (vm_perms & AB_VM_PERM_EXECUTE) flags |= MMU_FLAG_PERM_EXECUTE;
    if (is_device)                     flags |= MMU_FLAG_CACHE_DEVICE;
    return flags;
}

ab_status_t ab_vmar_map(ab_process_t *proc, ab_object_t *vmar_obj,
                        size_t vmar_offset, ab_object_t *vmo_obj,
                        uint64_t vmo_offset, size_t len, uint32_t vm_perms,
                        vaddr_t *out_va) {
    (void)proc;
    if (!vmar_obj || vmar_obj->type != AB_OBJ_TYPE_VMAR ||
        !vmo_obj  || vmo_obj->type  != AB_OBJ_TYPE_VMO  || !out_va) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_vmar_dispatcher_t *vmar = (ab_vmar_dispatcher_t *)vmar_obj;
    ab_process_t *target_proc  = vmar->proc;
    ab_vmo_dispatcher_t *vmo   = (ab_vmo_dispatcher_t *)vmo_obj;

    size_t aligned_len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (vmo_offset + aligned_len > vmo->size_bytes) {
        return AB_ERR_OUT_OF_RANGE;
    }

    int slot = -1;
    for (int i = 0; i < (int)MAX_VMAR_MAPPINGS; i++) {
        if (!target_proc->mappings[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return AB_ERR_NO_RESOURCES;

    if (target_proc->next_map_va == 0) {
        target_proc->next_map_va = 0x10000000ULL;
    }
    vaddr_t map_va;
    if (vmar_offset != 0) {
        map_va = vmar->base_va + (vmar_offset & ~(PAGE_SIZE - 1));
    } else {
        map_va = target_proc->next_map_va;
        target_proc->next_map_va += aligned_len + PAGE_SIZE;
    }

    ab_object_add_ref(&vmo->base);
    target_proc->mappings[slot].active     = 1;
    target_proc->mappings[slot].base_va    = map_va;
    target_proc->mappings[slot].size       = aligned_len;
    target_proc->mappings[slot].vmo_offset = (size_t)vmo_offset;
    target_proc->mappings[slot].vm_perms   = vm_perms;
    target_proc->mappings[slot].vmo        = vmo;

    /* Map any pages already committed in the VMO (uncommitted pages fault lazily!) */
    uint32_t mmu_flags = vm_perms_to_mmu_flags(vm_perms, vmo->is_physical);
    for (size_t off = 0; off < aligned_len; off += PAGE_SIZE) {
        uint32_t pg_idx = (uint32_t)((vmo_offset + off) / PAGE_SIZE);
        paddr_t pa = 0;
        if (ab_vmo_get_or_alloc_page(vmo, pg_idx, 0, &pa) == AB_OK && pa != 0) {
            vmm_map_page(target_proc->aspace, map_va + off, pa, mmu_flags);
        }
    }

    if (vm_perms & AB_VM_PERM_EXECUTE) {
        __asm__ volatile("ic ialluis\n\tdsb ish\n\tisb" ::: "memory");
    }
    *out_va = map_va;
    return AB_OK;
}

int ab_process_handle_page_fault(ab_process_t *proc, vaddr_t fault_va, int is_write) {
    if (!proc) return -1;
    vaddr_t page_va = fault_va & ~(PAGE_SIZE - 1);

    for (uint32_t i = 0; i < MAX_VMAR_MAPPINGS; i++) {
        ab_vmar_mapping_t *m = &proc->mappings[i];
        if (!m->active) continue;
        if (page_va >= m->base_va && page_va < m->base_va + m->size) {
            if (is_write && !(m->vm_perms & AB_VM_PERM_WRITE)) {
                return -1; /* Write to read-only VMAR mapping! */
            }
            size_t offset_in_vmar = page_va - m->base_va;
            uint32_t pg_idx = (uint32_t)((m->vmo_offset + offset_in_vmar) / PAGE_SIZE);
            paddr_t pa = 0;
            if (ab_vmo_get_or_alloc_page(m->vmo, pg_idx, 1, &pa) != AB_OK) {
                return -1;
            }
            uint32_t mmu_flags = vm_perms_to_mmu_flags(m->vm_perms, m->vmo->is_physical);
            vmm_map_page(proc->aspace, page_va, pa, mmu_flags);
            dprintf(INFO, "[vmm-fault] Demand-paged VMO (koid=%lu, pg=%u -> PA 0x%lx) into proc '%s' at VA 0x%lx\n",
                    m->vmo->base.koid, pg_idx, pa, proc->name, page_va);
            return 0;
        }
    }
    return -1;
}

/* =========================================================================
 * Lesson 12: JobDispatcher, ThreadDispatcher & User Process Spawning
 * ========================================================================= */
extern char _text_start[];
extern char _rodata_end[];
extern void arm64_uspace_entry(uintptr_t arg1, uintptr_t arg2, uintptr_t pc,
                               uintptr_t user_sp, vaddr_t kstack_top);
#define USER_KVA_TO_UVA(kva) ((vaddr_t)((uintptr_t)(kva) - 0xFFFFFFFF80000000ULL))

static volatile uint16_t g_next_asid = 1;

uint16_t ab_alloc_asid(void) {
    return __atomic_fetch_add(&g_next_asid, 1, __ATOMIC_RELAXED);
}

ab_status_t ab_job_dispatcher_create(ab_object_t **out_obj) {
    ab_job_dispatcher_t *job = (ab_job_dispatcher_t *)ab_object_alloc(
        sizeof(ab_job_dispatcher_t), AB_OBJ_TYPE_JOB, AB_SIGNAL_NONE,
        NULL, NULL);
    if (!job) return AB_ERR_NO_MEMORY;
    job->lock.value  = 0;
    job->child_count = 0;
    ab_object_set_name(&job->base, "root", 4);
    for (uint32_t i = 0; i < MAX_PROCS_PER_JOB; i++) {
        job->procs[i] = NULL;
    }
    *out_obj = &job->base;
    return AB_OK;
}

void ab_job_add_process(ab_job_dispatcher_t *job, ab_process_t *proc) {
    if (!job || !proc) return;
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&job->lock, &irqstate);
    proc->base.related_koid = job->base.koid;
    for (uint32_t i = 0; i < MAX_PROCS_PER_JOB; i++) {
        if (job->procs[i] == NULL || job->procs[i]->exited) {
            if (job->procs[i] != NULL) {
                ab_object_release(&job->procs[i]->base);
            } else {
                job->child_count++;
            }
            ab_object_add_ref(&proc->base);
            job->procs[i] = proc;
            break;
        }
    }
    spin_lock_irqrestore(&job->lock, irqstate);
}

ab_status_t ab_thread_dispatcher_create(ab_process_t *proc, const char *name,
                                        ab_object_t **out_obj) {
    if (!proc) return AB_ERR_INVALID_ARGS;
    ab_thread_dispatcher_t *td = (ab_thread_dispatcher_t *)ab_object_alloc(
        sizeof(ab_thread_dispatcher_t), AB_OBJ_TYPE_THREAD, AB_SIGNAL_NONE,
        NULL, NULL);
    if (!td) return AB_ERR_NO_MEMORY;

    size_t i = 0;
    while (name && name[i] && i + 1 < sizeof(td->name)) {
        td->name[i] = name[i];
        i++;
    }
    td->name[i] = '\0';

    td->base.related_koid = proc->base.koid;
    ab_object_add_ref(&proc->base);
    td->proc = proc;
    if (proc->thread_count < MAX_THREADS_PER_PROCESS) {
        ab_object_add_ref(&td->base);
        proc->threads[proc->thread_count++] = td;
    }
    *out_obj = &td->base;
    return AB_OK;
}

static int user_thread_trampoline(void *arg) {
    ab_thread_dispatcher_t *td = (ab_thread_dispatcher_t *)arg;
    ab_process_t *proc = td->proc;
    thread_t *cur = get_current_thread();

    cur->process_obj = proc;
    cur->thread_obj  = td;
    cur->aspace      = proc->aspace;
    vmm_switch_aspace(proc->aspace);

    /* Map shared EL0 code/rodata segment + allocate user stack if needed */
    for (uintptr_t kva = (uintptr_t)_text_start; kva < (uintptr_t)_rodata_end; kva += PAGE_SIZE) {
        paddr_t pa = physmap_to_paddr((const void *)kva);
        vaddr_t uva = USER_KVA_TO_UVA(kva);
        vmm_map_page(proc->aspace, uva, pa,
                     MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_EXECUTE);
    }

    if (td->user_sp == 0) {
        vaddr_t user_stack_va = 0x00800000ULL;
        size_t stack_pages = 6U; /* 24KB EL0 user stack */
        for (size_t spg = 0; spg < stack_pages; spg++) {
            paddr_t stack_pa = 0;
            pmm_alloc_page(VM_PAGE_STATE_OBJECT, &stack_pa);
            vmm_map_page(proc->aspace, user_stack_va + spg * PAGE_SIZE, stack_pa,
                         MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_WRITE);
        }
        td->user_sp          = user_stack_va + stack_pages * PAGE_SIZE;
        proc->stack_va       = user_stack_va;
        proc->stack_size     = stack_pages * PAGE_SIZE;
        proc->stack_vmo_koid = ab_koid_alloc();
    }

    vaddr_t kstack_top = (cur->stack_base + cur->stack_size) & ~15ULL;
    arm64_uspace_entry(td->arg0, td->arg1, td->user_entry, td->user_sp, kstack_top);
    return 0;
}

ab_status_t ab_process_start_user(ab_process_t *caller_proc,
                                  ab_process_t *target_proc,
                                  ab_thread_dispatcher_t *uthr,
                                  uintptr_t entry_pc, uintptr_t user_sp,
                                  ab_handle_t arg_handle, uint64_t arg2) {
    if (!target_proc || !uthr || uthr->proc != target_proc) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_handle_t child_arg_h = AB_HANDLE_INVALID;
    if (arg_handle != AB_HANDLE_INVALID && caller_proc != NULL) {
        ab_object_t *transferred_obj = NULL;
        ab_rights_t  transferred_rights = 0;
        ab_status_t st = handle_take_for_transfer(caller_proc, arg_handle, NULL,
                                                  &transferred_obj, &transferred_rights);
        if (st != AB_OK) return st;

        st = handle_install_transferred(target_proc, transferred_obj,
                                        transferred_rights, &child_arg_h);
        if (st != AB_OK) {
            ab_object_release(transferred_obj);
            return st;
        }
    } else {
        child_arg_h = arg_handle;
    }

    uthr->user_entry = entry_pc;
    uthr->user_sp    = user_sp;
    uthr->arg0       = (uint64_t)child_arg_h;
    uthr->arg1       = arg2;

    thread_t *kt = thread_create(uthr->name, user_thread_trampoline, uthr);
    if (!kt) return AB_ERR_NO_MEMORY;
    kt->process_obj = target_proc;
    kt->thread_obj  = uthr;
    kt->aspace      = target_proc->aspace;
    ab_object_add_ref(&uthr->base);
    uthr->kthread = kt;
    thread_resume(kt);
    return AB_OK;
}

/* =========================================================================
 * Lesson 13: ResourceDispatcher, Physical/Contiguous VMOs & InterruptDispatcher
 * ========================================================================= */
#include "gic.h"

#define MAX_GIC_IRQS 128
static ab_interrupt_dispatcher_t *g_irq_table[MAX_GIC_IRQS];

ab_status_t ab_resource_dispatcher_create(uint32_t kind, ab_object_t **out_obj) {
    ab_resource_dispatcher_t *res = (ab_resource_dispatcher_t *)ab_object_alloc(
        sizeof(ab_resource_dispatcher_t), AB_OBJ_TYPE_RESOURCE, AB_SIGNAL_NONE,
        NULL, NULL);
    if (!res) return AB_ERR_NO_MEMORY;
    res->kind = kind;
    *out_obj = &res->base;
    return AB_OK;
}

ab_status_t ab_vmo_create_physical(paddr_t paddr, size_t size_bytes,
                                   ab_object_t **out_obj) {
    size_t aligned = (size_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (aligned == 0) aligned = PAGE_SIZE;
    uint32_t pages = (uint32_t)(aligned / PAGE_SIZE);
    if (pages > VMO_MAX_PAGES) return AB_ERR_OUT_OF_RANGE;

    ab_vmo_dispatcher_t *vmo = (ab_vmo_dispatcher_t *)ab_object_alloc(
        sizeof(ab_vmo_dispatcher_t), AB_OBJ_TYPE_VMO, AB_SIGNAL_NONE,
        NULL, vmo_destroy);
    if (!vmo) return AB_ERR_NO_MEMORY;

    vmo->lock.value  = 0;
    vmo->size_bytes  = aligned;
    vmo->num_pages   = pages;
    vmo->is_physical = 1;
    ab_object_set_name(&vmo->base, "vmo-phys", 8);
    paddr_t base_pa  = paddr & ~(PAGE_SIZE - 1);
    for (uint32_t i = 0; i < pages; i++) {
        vmo->paddrs[i] = base_pa + (paddr_t)i * PAGE_SIZE;
    }
    *out_obj = &vmo->base;
    return AB_OK;
}

ab_status_t ab_vmo_create_contiguous(size_t size_bytes, ab_object_t **out_obj,
                                     paddr_t *out_paddr) {
    size_t aligned = (size_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (aligned == 0) aligned = PAGE_SIZE;
    uint32_t pages = (uint32_t)(aligned / PAGE_SIZE);
    if (pages > VMO_MAX_PAGES) return AB_ERR_OUT_OF_RANGE;

    paddr_t base_pa = 0;
    if (pmm_alloc_contiguous(pages, VM_PAGE_STATE_OBJECT, &base_pa) == NULL) {
        return AB_ERR_NO_MEMORY;
    }

    uint8_t *kva = (uint8_t *)paddr_to_physmap(base_pa);
    for (size_t b = 0; b < aligned; b++) kva[b] = 0;

    ab_vmo_dispatcher_t *vmo = (ab_vmo_dispatcher_t *)ab_object_alloc(
        sizeof(ab_vmo_dispatcher_t), AB_OBJ_TYPE_VMO, AB_SIGNAL_NONE,
        NULL, vmo_destroy);
    if (!vmo) {
        for (uint32_t i = 0; i < pages; i++) {
            pmm_free_page(base_pa + (paddr_t)i * PAGE_SIZE);
        }
        return AB_ERR_NO_MEMORY;
    }

    vmo->lock.value  = 0;
    vmo->size_bytes  = aligned;
    vmo->num_pages   = pages;
    vmo->is_physical = 0;
    ab_object_set_name(&vmo->base, "vmo-contig", 10);
    for (uint32_t i = 0; i < pages; i++) {
        vmo->paddrs[i] = base_pa + (paddr_t)i * PAGE_SIZE;
    }

    *out_obj = &vmo->base;
    if (out_paddr) *out_paddr = base_pa;
    return AB_OK;
}

static void interrupt_destroy(ab_object_t *obj) {
    ab_interrupt_dispatcher_t *intr = (ab_interrupt_dispatcher_t *)obj;
    if (intr->irq_num < MAX_GIC_IRQS) {
        gic_disable_interrupt(intr->irq_num);
        g_irq_table[intr->irq_num] = NULL;
    }
    kfree(intr);
}

ab_status_t ab_interrupt_dispatcher_create(uint32_t irq_num, ab_object_t **out_obj) {
    if (irq_num >= MAX_GIC_IRQS) return AB_ERR_OUT_OF_RANGE;

    ab_interrupt_dispatcher_t *intr = (ab_interrupt_dispatcher_t *)ab_object_alloc(
        sizeof(ab_interrupt_dispatcher_t), AB_OBJ_TYPE_INTERRUPT, AB_SIGNAL_NONE,
        NULL, interrupt_destroy);
    if (!intr) return AB_ERR_NO_MEMORY;

    intr->irq_num       = irq_num;
    intr->trigger_count = 0;
    event_init(&intr->irq_event, 0, EVENT_FLAG_AUTOUNSIGNAL);

    g_irq_table[irq_num] = intr;
    gic_enable_interrupt(irq_num);

    *out_obj = &intr->base;
    return AB_OK;
}

ab_status_t ab_interrupt_wait(ab_object_t *intr_obj) {
    if (!intr_obj || intr_obj->type != AB_OBJ_TYPE_INTERRUPT) {
        return AB_ERR_WRONG_TYPE;
    }
    ab_interrupt_dispatcher_t *intr = (ab_interrupt_dispatcher_t *)intr_obj;
    return (event_wait(&intr->irq_event) == 0) ? AB_OK : AB_ERR_TIMED_OUT;
}

ab_status_t ab_interrupt_ack(ab_object_t *intr_obj) {
    if (!intr_obj || intr_obj->type != AB_OBJ_TYPE_INTERRUPT) {
        return AB_ERR_WRONG_TYPE;
    }
    ab_interrupt_dispatcher_t *intr = (ab_interrupt_dispatcher_t *)intr_obj;
    state_tracker_update_signals(&intr->base, AB_SIGNAL_SIGNALED, 0);
    gic_enable_interrupt(intr->irq_num);
    return AB_OK;
}

int ab_interrupt_dispatch_hw_irq(uint32_t irq_num) {
    if (irq_num >= MAX_GIC_IRQS) return -1;
    ab_interrupt_dispatcher_t *intr = g_irq_table[irq_num];
    if (!intr) return -1;

    /* Mask IRQ at GIC until the EL0 user-space driver acknowledges it! */
    gic_disable_interrupt(irq_num);
    intr->trigger_count++;
    state_tracker_update_signals(&intr->base, 0, AB_SIGNAL_SIGNALED);
    event_signal(&intr->irq_event);
    return 0;
}

/* =========================================================================
 * Lesson 17: Zircon-Style Task & Address Space Diagnostics ('ps' & 'vmaps')
 * References:
 *   - zircon_org/kernel/object/diagnostics.cpp (DumpProcessList, GetVmAspaceMaps)
 *   - zircon_org/kernel/syscalls/object.cpp (ZX_INFO_JOB_PROCESSES, ZX_INFO_PROCESS_MAPS)
 * ========================================================================= */
static void k_copy_str(char *dst, const char *src, size_t max_len) {
    size_t i = 0;
    while (src && src[i] && i + 1 < max_len) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

ab_status_t ab_object_set_name(ab_object_t *obj, const char *name, size_t len) {
    if (!obj || !name) return AB_ERR_INVALID_ARGS;
    char *dst = NULL;
    if (obj->type == AB_OBJ_TYPE_VMO) {
        dst = ((ab_vmo_dispatcher_t *)obj)->name;
    } else if (obj->type == AB_OBJ_TYPE_PROCESS) {
        dst = ((ab_process_t *)obj)->name;
    } else if (obj->type == AB_OBJ_TYPE_THREAD) {
        dst = ((ab_thread_dispatcher_t *)obj)->name;
    } else if (obj->type == AB_OBJ_TYPE_JOB) {
        dst = ((ab_job_dispatcher_t *)obj)->name;
    } else {
        return AB_ERR_NOT_SUPPORTED;
    }

    size_t max_copy = (len < 31) ? len : 31;
    size_t i = 0;
    while (i < max_copy && name[i] != '\0') {
        dst[i] = name[i];
        i++;
    }
    dst[i] = '\0';
    return AB_OK;
}

static const char *kthread_state_to_str(thread_state_t st) {
    switch (st) {
        case THREAD_INITIAL:  return "new";
        case THREAD_READY:    return "ready";
        case THREAD_RUNNING:  return "running";
        case THREAD_BLOCKED:  return "blocked";
        case THREAD_SLEEPING: return "sleeping";
        case THREAD_DEATH:    return "dead";
        default:              return "???";
    }
}

static size_t vmo_count_committed_pages(ab_vmo_dispatcher_t *vmo, size_t offset, size_t len) {
    if (!vmo) return 0;
    size_t committed = 0;
    for (size_t off = 0; off < len; off += PAGE_SIZE) {
        uint32_t pg = (uint32_t)((offset + off) / PAGE_SIZE);
        if (pg < vmo->num_pages && vmo->paddrs[pg] != 0) {
            committed++;
        }
    }
    return committed;
}

ab_status_t ab_job_get_ps_info(ab_object_t *job_obj, ab_task_info_entry_t *out_entries,
                               uint32_t max_entries, uint32_t *actual_out) {
    if (!job_obj || job_obj->type != AB_OBJ_TYPE_JOB || !out_entries || !actual_out) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_job_dispatcher_t *job = (ab_job_dispatcher_t *)job_obj;
    size_t shared_code_bytes = (size_t)((uintptr_t)_rodata_end - (uintptr_t)_text_start);

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&job->lock, &irqstate);

    uint32_t active_procs = 0;
    for (uint32_t i = 0; i < MAX_PROCS_PER_JOB; i++) {
        if (job->procs[i] && !job->procs[i]->exited) {
            active_procs++;
        }
    }
    if (active_procs == 0) active_procs = 1;

    uint32_t count = 0;
    if (count < max_entries) {
        ab_task_info_entry_t *je = &out_entries[count++];
        for (size_t b = 0; b < sizeof(*je); b++) ((uint8_t *)je)[b] = 0;
        je->type        = 'j';
        je->depth       = 0;
        je->koid        = job->base.koid;
        je->parent_koid = 0;
        k_copy_str(je->name, job->name, sizeof(je->name));
        k_copy_str(je->state, "active", sizeof(je->state));
    }

    for (uint32_t i = 0; i < MAX_PROCS_PER_JOB; i++) {
        ab_process_t *proc = job->procs[i];
        if (!proc || proc->exited) continue;
        if (count >= max_entries) break;

        ab_task_info_entry_t *pe = &out_entries[count++];
        for (size_t b = 0; b < sizeof(*pe); b++) ((uint8_t *)pe)[b] = 0;
        pe->type        = 'p';
        pe->depth       = 1;
        pe->asid        = proc->aspace ? proc->aspace->asid : 0;
        pe->koid        = proc->base.koid;
        pe->parent_koid = job->base.koid;
        k_copy_str(pe->name, proc->name, sizeof(pe->name));
        k_copy_str(pe->state, "active", sizeof(pe->state));

        /* Compute private, shared, and PSS memory usage (models Zircon's VmCounter) */
        size_t priv_bytes = proc->stack_size;
        size_t sh_bytes   = shared_code_bytes;
        size_t scaled_sh  = shared_code_bytes / active_procs;

        for (uint32_t m_idx = 0; m_idx < MAX_VMAR_MAPPINGS; m_idx++) {
            ab_vmar_mapping_t *m = &proc->mappings[m_idx];
            if (!m->active || !m->vmo) continue;
            size_t comm_bytes = vmo_count_committed_pages(m->vmo, m->vmo_offset, m->size) * PAGE_SIZE;
            priv_bytes += comm_bytes;
        }

        pe->private_bytes = priv_bytes;
        pe->shared_bytes  = sh_bytes;
        pe->pss_bytes     = priv_bytes + scaled_sh;

        /* Accumulate into root job totals (models Zircon's ps.c job_stack) */
        out_entries[0].private_bytes += pe->private_bytes;
        out_entries[0].pss_bytes     += pe->pss_bytes;

        /* Count handles by type (models Zircon's BuildHandleStats in diagnostics.cpp) */
        spin_lock_saved_state_t h_irq;
        spin_lock_irqsave(&proc->handle_lock, &h_irq);
        for (uint32_t s = 0; s < MAX_HANDLES_PER_PROCESS; s++) {
            ab_object_t *hobj = proc->handles[s].obj;
            if (hobj != NULL) {
                pe->total_handles++;
                if ((uint32_t)hobj->type < AB_OBJ_TYPE_COUNT) {
                    pe->handle_counts[hobj->type]++;
                }
            }
        }
        spin_lock_irqrestore(&proc->handle_lock, h_irq);

        /* Emit child threads of this process */
        for (uint32_t t = 0; t < proc->thread_count; t++) {
            ab_thread_dispatcher_t *td = proc->threads[t];
            if (!td || count >= max_entries) continue;

            ab_task_info_entry_t *te = &out_entries[count++];
            for (size_t b = 0; b < sizeof(*te); b++) ((uint8_t *)te)[b] = 0;
            te->type          = 't';
            te->depth         = 2;
            te->koid          = td->base.koid;
            te->parent_koid   = proc->base.koid;
            te->private_bytes = td->kthread ? td->kthread->stack_size : 0;
            k_copy_str(te->name, td->name, sizeof(te->name));
            if (td->kthread) {
                k_copy_str(te->state, kthread_state_to_str(td->kthread->state), sizeof(te->state));
            } else {
                k_copy_str(te->state, "new", sizeof(te->state));
            }
        }
    }

    spin_lock_irqrestore(&job->lock, irqstate);
    *actual_out = count;
    return AB_OK;
}

static void add_map_entry(ab_info_maps_t *out_maps, uint32_t max_maps, uint32_t *idx,
                          const char *name, vaddr_t base, size_t size,
                          size_t depth, ab_info_maps_type_t type,
                          uint32_t mmu_flags, ab_koid_t vmo_koid,
                          uint64_t vmo_offset, size_t committed_pages, paddr_t first_paddr) {
    if (*idx >= max_maps) return;
    ab_info_maps_t *e = &out_maps[(*idx)++];
    for (size_t b = 0; b < sizeof(*e); b++) ((uint8_t *)e)[b] = 0;
    k_copy_str(e->name, name, sizeof(e->name));
    e->base                    = base;
    e->size                    = size;
    e->depth                   = depth;
    e->type                    = type;
    e->mapping.mmu_flags       = mmu_flags;
    e->mapping.vmo_koid        = vmo_koid;
    e->mapping.vmo_offset      = vmo_offset;
    e->mapping.committed_pages = committed_pages;
    e->mapping.first_paddr     = first_paddr;
}

ab_status_t ab_job_get_process_maps(ab_object_t *job_obj, ab_koid_t target_proc_koid,
                                    ab_info_maps_t *out_maps, uint32_t max_maps,
                                    uint32_t *actual_out) {
    if (!job_obj || job_obj->type != AB_OBJ_TYPE_JOB || !out_maps || !actual_out) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_job_dispatcher_t *job = (ab_job_dispatcher_t *)job_obj;
    ab_process_t *target = NULL;

    if (target_proc_koid == 0) {
        target = get_current_process();
    } else {
        spin_lock_saved_state_t irqstate;
        spin_lock_irqsave(&job->lock, &irqstate);
        for (uint32_t i = 0; i < MAX_PROCS_PER_JOB; i++) {
            if (job->procs[i] && !job->procs[i]->exited &&
                job->procs[i]->base.koid == target_proc_koid) {
                target = job->procs[i];
                break;
            }
        }
        spin_lock_irqrestore(&job->lock, irqstate);
    }

    if (!target || target->exited) {
        return AB_ERR_NOT_FOUND;
    }

    uint32_t count = 0;
    size_t code_size = (size_t)((uintptr_t)_rodata_end - (uintptr_t)_text_start);

    /* 1. Depth 0: Process VmAspace ('/A' in Zircon vmaps) */
    add_map_entry(out_maps, max_maps, &count,
                  target->name, USER_ASPACE_BASE, USER_ASPACE_SIZE,
                  0, AB_INFO_MAPS_TYPE_ASPACE, 0, 0, 0, 0, 0);

    /* 2. Depth 1: Root VMAR ('/R' in Zircon vmaps) */
    add_map_entry(out_maps, max_maps, &count,
                  "root", USER_ASPACE_BASE, 0x000000003FFFF000ULL,
                  1, AB_INFO_MAPS_TYPE_VMAR, 0, 0, 0, 0, 0);

    /* 3. Depth 2: Shared EL0 .text + .rodata mapping ('M') */
    add_map_entry(out_maps, max_maps, &count,
                  "el0-code-rodata", USER_KVA_TO_UVA(_text_start), code_size,
                  2, AB_INFO_MAPS_TYPE_MAPPING,
                  AB_VM_PERM_READ | AB_VM_PERM_EXECUTE,
                  AB_KOID_KERNEL, 0, code_size / PAGE_SIZE,
                  physmap_to_paddr(_text_start));

    /* 4. Depth 2: Process User Stack mapping ('M') */
    if (target->stack_size > 0) {
        paddr_t stack_pa = 0;
        vmm_resolve_vaddr(target->aspace, target->stack_va, &stack_pa, NULL);
        add_map_entry(out_maps, max_maps, &count,
                      "user-stack", target->stack_va, target->stack_size,
                      2, AB_INFO_MAPS_TYPE_MAPPING,
                      AB_VM_PERM_READ | AB_VM_PERM_WRITE,
                      target->stack_vmo_koid, 0, target->stack_size / PAGE_SIZE,
                      stack_pa);
    }

    /* 5. Depth 2: Dynamic VMAR Region (0x10000000..0x40000000) & its VMO mappings */
    add_map_entry(out_maps, max_maps, &count,
                  "vmar-dynamic", 0x10000000ULL, 0x30000000ULL,
                  2, AB_INFO_MAPS_TYPE_VMAR, 0, 0, 0, 0, 0);

    for (uint32_t i = 0; i < MAX_VMAR_MAPPINGS; i++) {
        ab_vmar_mapping_t *m = &target->mappings[i];
        if (!m->active || !m->vmo) continue;

        size_t comm_pages = vmo_count_committed_pages(m->vmo, m->vmo_offset, m->size);
        uint32_t first_pg = (uint32_t)(m->vmo_offset / PAGE_SIZE);
        paddr_t first_pa  = (first_pg < m->vmo->num_pages) ? m->vmo->paddrs[first_pg] : 0;

        add_map_entry(out_maps, max_maps, &count,
                      m->vmo->name[0] ? m->vmo->name : "vmo",
                      m->base_va, m->size,
                      3, AB_INFO_MAPS_TYPE_MAPPING,
                      m->vm_perms, m->vmo->base.koid,
                      m->vmo_offset, comm_pages, first_pa);
    }

    *actual_out = count;
    return AB_OK;
}
