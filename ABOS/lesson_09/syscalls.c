/* =========================================================================
 * syscalls.c - Capability & Signal System Call Implementation
 * ========================================================================= */
#include "syscalls.h"
#include "thread.h"
#include "debuglog.h"


static ab_status_t sys_handle_close(ab_handle_t handle) {
    return handle_close(get_current_process(), handle);
}

static ab_status_t sys_handle_duplicate(ab_handle_t handle, ab_rights_t rights,
                                        ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    return handle_duplicate(get_current_process(), handle, rights, user_out);
}

static ab_status_t sys_object_get_info(ab_handle_t handle,
                                       ab_handle_basic_info_t *user_info) {
    if (!user_info) return AB_ERR_INVALID_ARGS;
    ab_object_t *obj = NULL;
    ab_rights_t rights = 0;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                                    AB_RIGHT_INSPECT, &obj, &rights);
    if (st != AB_OK) return st;

    user_info->koid         = obj->koid;
    user_info->related_koid = obj->related_koid;
    user_info->type         = (uint32_t)obj->type;
    user_info->rights       = rights;
    user_info->handle_count = (uint32_t)obj->handle_count;

    ab_object_release(obj);
    return AB_OK;
}

static ab_status_t sys_debuglog_write(ab_handle_t log_handle, uint32_t flags,
                                      const char *user_buf, size_t len) {
    (void)flags;
    if (!user_buf || len > 256) return AB_ERR_INVALID_ARGS;

    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), log_handle,
                                    AB_OBJ_TYPE_LOG, AB_RIGHT_WRITE,
                                    &obj, NULL);
    if (st != AB_OK) return st;

    char kbuf[257];
    for (size_t i = 0; i < len; i++) {
        kbuf[i] = user_buf[i];
    }
    kbuf[len] = '\0';

    ab_process_t *proc = get_current_process();
    dprintf(INFO, "[debuglog proc='%s' koid=%lu] %s\n",
            proc ? proc->name : "unknown", obj->koid, kbuf);

    ab_object_release(obj);
    return AB_OK;
}

static ab_status_t sys_event_create(uint32_t options, ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_object_t *ev = NULL;
    ab_status_t st = ab_event_dispatcher_create(options, &ev);
    if (st != AB_OK) return st;

    st = handle_alloc(get_current_process(), ev, AB_RIGHTS_DEFAULT_EVENT, user_out);
    ab_object_release(ev);
    return st;
}

static ab_status_t sys_eventpair_create(uint32_t options,
                                        ab_handle_t *user_out0,
                                        ab_handle_t *user_out1) {
    if (options != 0 || !user_out0 || !user_out1) return AB_ERR_INVALID_ARGS;

    ab_object_t *ep0 = NULL, *ep1 = NULL;
    ab_status_t st = ab_eventpair_dispatcher_create(&ep0, &ep1);
    if (st != AB_OK) return st;

    ab_handle_t h0 = AB_HANDLE_INVALID, h1 = AB_HANDLE_INVALID;
    st = handle_alloc(get_current_process(), ep0, AB_RIGHTS_DEFAULT_EVENTPAIR, &h0);
    if (st == AB_OK) {
        st = handle_alloc(get_current_process(), ep1, AB_RIGHTS_DEFAULT_EVENTPAIR, &h1);
        if (st != AB_OK) {
            handle_close(get_current_process(), h0);
        }
    }
    ab_object_release(ep0);
    ab_object_release(ep1);

    if (st == AB_OK) {
        *user_out0 = h0;
        *user_out1 = h1;
    }
    return st;
}

#define USER_ALLOWED_SIGNALS (AB_SIGNAL_SIGNALED | AB_USER_SIGNAL_ALL)

static ab_status_t sys_object_signal(ab_handle_t handle,
                                     ab_signals_t clear_mask,
                                     ab_signals_t set_mask) {
    if ((clear_mask & ~USER_ALLOWED_SIGNALS) || (set_mask & ~USER_ALLOWED_SIGNALS)) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                                    AB_RIGHT_SIGNAL, &obj, NULL);
    if (st != AB_OK) return st;

    state_tracker_update_signals(obj, clear_mask, set_mask);
    ab_object_release(obj);
    return AB_OK;
}

static ab_status_t sys_object_signal_peer(ab_handle_t handle,
                                          ab_signals_t clear_mask,
                                          ab_signals_t set_mask) {
    if ((clear_mask & ~USER_ALLOWED_SIGNALS) || (set_mask & ~USER_ALLOWED_SIGNALS)) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_EVENTPAIR,
                                    AB_RIGHT_SIGNAL_PEER, &obj, NULL);
    if (st != AB_OK) return st;

    st = ab_eventpair_signal_peer(obj, clear_mask, set_mask);
    ab_object_release(obj);
    return st;
}

static ab_status_t sys_object_wait_one(ab_handle_t handle,
                                       ab_signals_t watched_signals,
                                       uint64_t deadline_ns,
                                       ab_signals_t *user_observed) {
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                                    AB_RIGHT_WAIT, &obj, NULL);
    if (st != AB_OK) return st;

    st = state_tracker_wait_one(obj, watched_signals, deadline_ns, user_observed);
    ab_object_release(obj);
    return st;
}


static ab_status_t sys_port_create(uint32_t options, ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_object_t *port = NULL;
    ab_status_t st = ab_port_dispatcher_create(options, &port);
    if (st != AB_OK) return st;

    st = handle_alloc(get_current_process(), port, AB_RIGHTS_DEFAULT_PORT, user_out);
    ab_object_release(port);
    return st;
}

static ab_status_t sys_port_queue(ab_handle_t port_handle,
                                  const ab_port_packet_t *user_pkt) {
    if (!user_pkt) return AB_ERR_INVALID_ARGS;
    ab_object_t *port = NULL;
    ab_status_t st = handle_lookup(get_current_process(), port_handle,
                                   AB_OBJ_TYPE_PORT, AB_RIGHT_WRITE, &port, NULL);
    if (st != AB_OK) return st;

    st = ab_port_queue(port, user_pkt);
    ab_object_release(port);
    return st;
}

static ab_status_t sys_port_wait(ab_handle_t port_handle,
                                 uint64_t deadline_ns,
                                 ab_port_packet_t *user_out_pkt) {
    if (!user_out_pkt) return AB_ERR_INVALID_ARGS;
    ab_object_t *port = NULL;
    ab_status_t st = handle_lookup(get_current_process(), port_handle,
                                   AB_OBJ_TYPE_PORT, AB_RIGHT_READ, &port, NULL);
    if (st != AB_OK) return st;

    st = ab_port_wait(port, deadline_ns, user_out_pkt);
    ab_object_release(port);
    return st;
}

static ab_status_t sys_object_wait_async(ab_handle_t handle,
                                         ab_handle_t port_handle,
                                         uint64_t key,
                                         ab_signals_t signals) {
    ab_process_t *proc = get_current_process();
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(proc, handle, AB_OBJ_TYPE_NONE,
                                   AB_RIGHT_WAIT, &obj, NULL);
    if (st != AB_OK) return st;

    ab_object_t *port = NULL;
    st = handle_lookup(proc, port_handle, AB_OBJ_TYPE_PORT,
                       AB_RIGHT_WRITE, &port, NULL);
    if (st != AB_OK) {
        ab_object_release(obj);
        return st;
    }

    st = state_tracker_wait_async(obj, port, key, signals);
    ab_object_release(port);
    ab_object_release(obj);
    return st;
}

static void sys_process_exit(int exit_code) {
    ab_process_t *proc = get_current_process();
    if (proc) {
        proc->exit_code = exit_code;
        dprintf(INFO, "[process '%s' koid=%lu] exiting with code %d; closing remaining handles\n",
                proc->name, proc->base.koid, exit_code);
        for (uint32_t i = 0; i < MAX_HANDLES_PER_PROCESS; i++) {
            if (proc->handles[i].obj != NULL) {
                ab_handle_t h = 0x50000000u | ((proc->handles[i].generation & 0xFFFFu) << 8) | i;
                handle_close(proc, h);
            }
        }
    }
    thread_exit(exit_code);
}

void syscall_dispatch(iframe_t *frame) {
    uint64_t sys_num = frame->r[8];
    ab_status_t ret = AB_ERR_NOT_SUPPORTED;

    switch (sys_num) {
        case SYS_HANDLE_CLOSE:
            ret = sys_handle_close((ab_handle_t)frame->r[0]);
            break;
        case SYS_HANDLE_DUPLICATE:
            ret = sys_handle_duplicate((ab_handle_t)frame->r[0],
                                       (ab_rights_t)frame->r[1],
                                       (ab_handle_t *)frame->r[2]);
            break;
        case SYS_OBJECT_GET_INFO:
            ret = sys_object_get_info((ab_handle_t)frame->r[0],
                                      (ab_handle_basic_info_t *)frame->r[1]);
            break;
        case SYS_DEBUGLOG_WRITE:
            ret = sys_debuglog_write((ab_handle_t)frame->r[0],
                                     (uint32_t)frame->r[1],
                                     (const char *)frame->r[2],
                                     (size_t)frame->r[3]);
            break;
        case SYS_PROCESS_EXIT:
            sys_process_exit((int)frame->r[0]);
            break;
        case SYS_NANOSLEEP:
            thread_sleep_ns(frame->r[0]);
            ret = AB_OK;
            break;
        case SYS_EVENT_CREATE:
            ret = sys_event_create((uint32_t)frame->r[0],
                                   (ab_handle_t *)frame->r[1]);
            break;
        case SYS_EVENTPAIR_CREATE:
            ret = sys_eventpair_create((uint32_t)frame->r[0],
                                       (ab_handle_t *)frame->r[1],
                                       (ab_handle_t *)frame->r[2]);
            break;
        case SYS_OBJECT_SIGNAL:
            ret = sys_object_signal((ab_handle_t)frame->r[0],
                                    (ab_signals_t)frame->r[1],
                                    (ab_signals_t)frame->r[2]);
            break;
        case SYS_OBJECT_SIGNAL_PEER:
            ret = sys_object_signal_peer((ab_handle_t)frame->r[0],
                                         (ab_signals_t)frame->r[1],
                                         (ab_signals_t)frame->r[2]);
            break;
        case SYS_OBJECT_WAIT_ONE:
            ret = sys_object_wait_one((ab_handle_t)frame->r[0],
                                      (ab_signals_t)frame->r[1],
                                      (uint64_t)frame->r[2],
                                      (ab_signals_t *)frame->r[3]);
            break;
        case SYS_PORT_CREATE:
            ret = sys_port_create((uint32_t)frame->r[0],
                                  (ab_handle_t *)frame->r[1]);
            break;
        case SYS_PORT_QUEUE:
            ret = sys_port_queue((ab_handle_t)frame->r[0],
                                 (const ab_port_packet_t *)frame->r[1]);
            break;
        case SYS_PORT_WAIT:
            ret = sys_port_wait((ab_handle_t)frame->r[0],
                                (uint64_t)frame->r[1],
                                (ab_port_packet_t *)frame->r[2]);
            break;
        case SYS_OBJECT_WAIT_ASYNC:
            ret = sys_object_wait_async((ab_handle_t)frame->r[0],
                                        (ab_handle_t)frame->r[1],
                                        (uint64_t)frame->r[2],
                                        (ab_signals_t)frame->r[3]);
            break;
        default:
            dprintf(CRITICAL, "Unknown syscall %lu\n", sys_num);
            ret = AB_ERR_NOT_SUPPORTED;
            break;
    }

    frame->r[0] = (uint64_t)(int64_t)ret;
}
