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


static ab_status_t sys_channel_create(uint32_t options,
                                      ab_handle_t *user_out0,
                                      ab_handle_t *user_out1) {
    if (options != 0 || !user_out0 || !user_out1) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();

    ab_object_t *ch0 = NULL, *ch1 = NULL;
    ab_status_t st = ab_channel_dispatcher_create(&ch0, &ch1);
    if (st != AB_OK) return st;

    ab_handle_t h0 = AB_HANDLE_INVALID, h1 = AB_HANDLE_INVALID;
    st = handle_alloc(proc, ch0, AB_RIGHTS_DEFAULT_CHANNEL, &h0);
    if (st == AB_OK) {
        st = handle_alloc(proc, ch1, AB_RIGHTS_DEFAULT_CHANNEL, &h1);
        if (st != AB_OK) handle_close(proc, h0);
    }
    ab_object_release(ch0);
    ab_object_release(ch1);

    if (st == AB_OK) {
        *user_out0 = h0;
        *user_out1 = h1;
    }
    return st;
}

static ab_status_t sys_channel_write(ab_handle_t handle, uint32_t options,
                                     const ab_channel_write_args_t *args) {
    if (options != 0 || !args) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_object_t *ch = NULL;
    ab_status_t st = handle_lookup(proc, handle, AB_OBJ_TYPE_CHANNEL,
                                   AB_RIGHT_WRITE, &ch, NULL);
    if (st != AB_OK) return st;

    st = ab_channel_write(proc, ch, args->bytes, args->num_bytes,
                          args->handles, args->num_handles);
    ab_object_release(ch);
    return st;
}

static ab_status_t sys_channel_read(ab_handle_t handle, uint32_t options,
                                    ab_channel_read_args_t *args) {
    if (options != 0 || !args) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_object_t *ch = NULL;
    ab_status_t st = handle_lookup(proc, handle, AB_OBJ_TYPE_CHANNEL,
                                   AB_RIGHT_READ, &ch, NULL);
    if (st != AB_OK) return st;

    st = ab_channel_read(proc, ch, args->bytes, args->handles,
                         args->max_bytes, args->max_handles,
                         args->actual_bytes, args->actual_handles);
    ab_object_release(ch);
    return st;
}


static ab_status_t sys_vmo_create(uint64_t size, uint32_t options,
                                  ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_object_t *vmo = NULL;
    ab_status_t st = ab_vmo_dispatcher_create((size_t)size, options, &vmo);
    if (st != AB_OK) return st;

    st = handle_alloc(get_current_process(), vmo, AB_RIGHTS_DEFAULT_VMO, user_out);
    ab_object_release(vmo);
    return st;
}

static ab_status_t sys_vmo_read(ab_handle_t handle, void *user_buf,
                                uint64_t offset, size_t len) {
    ab_object_t *vmo = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_VMO,
                                   AB_RIGHT_READ, &vmo, NULL);
    if (st != AB_OK) return st;
    st = ab_vmo_read(vmo, user_buf, offset, len);
    ab_object_release(vmo);
    return st;
}

static ab_status_t sys_vmo_write(ab_handle_t handle, const void *user_buf,
                                 uint64_t offset, size_t len) {
    ab_object_t *vmo = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_VMO,
                                   AB_RIGHT_WRITE, &vmo, NULL);
    if (st != AB_OK) return st;
    st = ab_vmo_write(vmo, user_buf, offset, len);
    ab_object_release(vmo);
    return st;
}

static ab_status_t sys_vmar_map(ab_handle_t vmar_handle, size_t vmar_offset,
                                const ab_vmar_map_args_t *args) {
    if (!args || !args->mapped_va_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();

    ab_object_t *vmar = NULL;
    ab_status_t st = handle_lookup(proc, vmar_handle, AB_OBJ_TYPE_VMAR,
                                   AB_RIGHT_MAP, &vmar, NULL);
    if (st != AB_OK) return st;

    /* Enforce capability rights on the VMO being mapped! */
    ab_rights_t req_vmo_rights = AB_RIGHT_MAP;
    if (args->map_flags & AB_VM_PERM_READ)    req_vmo_rights |= AB_RIGHT_READ;
    if (args->map_flags & AB_VM_PERM_WRITE)   req_vmo_rights |= AB_RIGHT_WRITE;
    if (args->map_flags & AB_VM_PERM_EXECUTE) req_vmo_rights |= AB_RIGHT_EXECUTE;

    ab_object_t *vmo = NULL;
    st = handle_lookup(proc, args->vmo_handle, AB_OBJ_TYPE_VMO,
                       req_vmo_rights, &vmo, NULL);
    if (st != AB_OK) {
        ab_object_release(vmar);
        return st;
    }

    st = ab_vmar_map(proc, vmar, vmar_offset, vmo,
                     args->vmo_offset, (size_t)args->len,
                     args->map_flags, args->mapped_va_out);
    ab_object_release(vmo);
    ab_object_release(vmar);
    return st;
}


extern uint16_t ab_alloc_asid(void);

static ab_status_t sys_process_create(ab_handle_t job_handle,
                                      const ab_process_create_args_t *args) {
    if (!args || !args->name || !args->proc_handle_out || !args->vmar_handle_out) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_process_t *caller = get_current_process();

    ab_object_t *job_obj = NULL;
    ab_status_t st = handle_lookup(caller, job_handle, AB_OBJ_TYPE_JOB,
                                   AB_RIGHT_MANAGE_PROCESS, &job_obj, NULL);
    if (st != AB_OK) return st;

    char kname[32];
    size_t n = (args->name_len < 31) ? args->name_len : 31;
    for (size_t i = 0; i < n; i++) kname[i] = args->name[i];
    kname[n] = '\0';

    ab_process_t *new_proc = ab_process_create_kernel(kname, ab_alloc_asid());
    if (!new_proc) {
        ab_object_release(job_obj);
        return AB_ERR_NO_MEMORY;
    }

    ab_object_t *vmar_obj = NULL;
    st = ab_vmar_dispatcher_create(new_proc, &vmar_obj);
    if (st != AB_OK) {
        ab_object_release(&new_proc->base);
        ab_object_release(job_obj);
        return st;
    }

    ab_handle_t proc_h = AB_HANDLE_INVALID, vmar_h = AB_HANDLE_INVALID;
    st = handle_alloc(caller, &new_proc->base, AB_RIGHTS_DEFAULT_PROCESS, &proc_h);
    if (st == AB_OK) {
        st = handle_alloc(caller, vmar_obj, AB_RIGHTS_DEFAULT_VMAR, &vmar_h);
        if (st != AB_OK) handle_close(caller, proc_h);
    }

    ab_object_release(vmar_obj);
    ab_object_release(&new_proc->base);
    ab_object_release(job_obj);

    if (st == AB_OK) {
        *args->proc_handle_out = proc_h;
        *args->vmar_handle_out = vmar_h;
    }
    return st;
}

static ab_status_t sys_thread_create(ab_handle_t proc_handle,
                                     const char *user_name, size_t name_len,
                                     ab_handle_t *user_out) {
    if (!user_name || !user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *caller = get_current_process();

    ab_object_t *proc_obj = NULL;
    ab_status_t st = handle_lookup(caller, proc_handle, AB_OBJ_TYPE_PROCESS,
                                   AB_RIGHT_MANAGE_THREAD, &proc_obj, NULL);
    if (st != AB_OK) return st;

    char kname[32];
    size_t n = (name_len < 31) ? name_len : 31;
    for (size_t i = 0; i < n; i++) kname[i] = user_name[i];
    kname[n] = '\0';

    ab_object_t *td_obj = NULL;
    st = ab_thread_dispatcher_create((ab_process_t *)proc_obj, kname, &td_obj);
    if (st == AB_OK) {
        st = handle_alloc(caller, td_obj, AB_RIGHTS_DEFAULT_THREAD, user_out);
        ab_object_release(td_obj);
    }
    ab_object_release(proc_obj);
    return st;
}

static ab_status_t sys_process_start(ab_handle_t proc_handle,
                                     const ab_process_start_args_t *args) {
    if (!args) return AB_ERR_INVALID_ARGS;
    ab_process_t *caller = get_current_process();

    ab_object_t *proc_obj = NULL;
    ab_status_t st = handle_lookup(caller, proc_handle, AB_OBJ_TYPE_PROCESS,
                                   AB_RIGHT_WRITE, &proc_obj, NULL);
    if (st != AB_OK) return st;

    ab_object_t *thr_obj = NULL;
    st = handle_lookup(caller, args->thread_handle, AB_OBJ_TYPE_THREAD,
                       AB_RIGHT_WRITE, &thr_obj, NULL);
    if (st != AB_OK) {
        ab_object_release(proc_obj);
        return st;
    }

    st = ab_process_start_user(caller, (ab_process_t *)proc_obj,
                               (ab_thread_dispatcher_t *)thr_obj,
                               args->entry_pc, args->user_sp,
                               args->arg_handle, args->arg2);
    ab_object_release(thr_obj);
    ab_object_release(proc_obj);
    return st;
}


static ab_status_t sys_vmo_create_physical(ab_handle_t resource_handle,
                                           const ab_vmo_create_phys_args_t *args) {
    if (!args || !args->vmo_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *caller = get_current_process();

    /* Require a valid ResourceDispatcher capability! */
    ab_object_t *res_obj = NULL;
    ab_status_t st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
                                   AB_RIGHT_WRITE, &res_obj, NULL);
    if (st != AB_OK) return st;
    ab_object_release(res_obj);

    ab_object_t *vmo_obj = NULL;
    st = ab_vmo_create_physical(args->paddr, args->size, &vmo_obj);
    if (st != AB_OK) return st;

    st = handle_alloc(caller, vmo_obj, AB_RIGHTS_DEFAULT_VMO, args->vmo_out);
    ab_object_release(vmo_obj);
    return st;
}

static ab_status_t sys_vmo_create_contiguous(ab_handle_t resource_handle,
                                             const ab_vmo_create_contig_args_t *args) {
    if (!args || !args->vmo_out || !args->paddr_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *caller = get_current_process();

    ab_object_t *res_obj = NULL;
    ab_status_t st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
                                   AB_RIGHT_WRITE, &res_obj, NULL);
    if (st != AB_OK) return st;
    ab_object_release(res_obj);

    ab_object_t *vmo_obj = NULL;
    st = ab_vmo_create_contiguous(args->size, &vmo_obj, args->paddr_out);
    if (st != AB_OK) return st;

    st = handle_alloc(caller, vmo_obj, AB_RIGHTS_DEFAULT_VMO, args->vmo_out);
    ab_object_release(vmo_obj);
    return st;
}

static ab_status_t sys_interrupt_create(ab_handle_t resource_handle,
                                        uint32_t irq_num,
                                        ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *caller = get_current_process();

    ab_object_t *res_obj = NULL;
    ab_status_t st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
                                   AB_RIGHT_READ, &res_obj, NULL);
    if (st != AB_OK) return st;
    ab_object_release(res_obj);

    ab_object_t *intr_obj = NULL;
    st = ab_interrupt_dispatcher_create(irq_num, &intr_obj);
    if (st != AB_OK) return st;

    st = handle_alloc(caller, intr_obj, AB_RIGHTS_DEFAULT_INTERRUPT, user_out);
    ab_object_release(intr_obj);
    return st;
}

static ab_status_t sys_interrupt_wait(ab_handle_t intr_handle) {
    ab_object_t *intr_obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), intr_handle,
                                   AB_OBJ_TYPE_INTERRUPT, AB_RIGHT_WAIT,
                                   &intr_obj, NULL);
    if (st != AB_OK) return st;
    st = ab_interrupt_wait(intr_obj);
    ab_object_release(intr_obj);
    return st;
}

static ab_status_t sys_interrupt_ack(ab_handle_t intr_handle) {
    ab_object_t *intr_obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), intr_handle,
                                   AB_OBJ_TYPE_INTERRUPT, AB_RIGHT_WRITE,
                                   &intr_obj, NULL);
    if (st != AB_OK) return st;
    st = ab_interrupt_ack(intr_obj);
    ab_object_release(intr_obj);
    return st;
}

static void sys_process_exit(int exit_code) {
    ab_process_t *proc = get_current_process();
    if (proc) {
        proc->exit_code = exit_code;
        state_tracker_update_signals(&proc->base, 0, AB_SIGNAL_TASK_TERMINATED);
        dprintf(INFO, "[process '%s' koid=%lu] exiting with code %d; closing remaining handles\n",
                proc->name, proc->base.koid, exit_code);
        for (uint32_t i = 0; i < MAX_VMAR_MAPPINGS; i++) {
            if (proc->mappings[i].active && proc->mappings[i].vmo) {
                ab_object_release(&proc->mappings[i].vmo->base);
                proc->mappings[i].vmo = NULL;
                proc->mappings[i].active = 0;
            }
        }
        for (uint32_t slot = 0; slot < MAX_HANDLES_PER_PROCESS; slot++) {
            if (proc->handles[slot].obj != NULL) {
                ab_handle_t h = (0x5U << 28) | ((proc->handles[slot].generation & 0xFFFFU) << 8) | slot;
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
        case SYS_CHANNEL_CREATE:
            ret = sys_channel_create((uint32_t)frame->r[0],
                                     (ab_handle_t *)frame->r[1],
                                     (ab_handle_t *)frame->r[2]);
            break;
        case SYS_CHANNEL_WRITE:
            ret = sys_channel_write((ab_handle_t)frame->r[0],
                                    (uint32_t)frame->r[1],
                                    (const ab_channel_write_args_t *)frame->r[2]);
            break;
        case SYS_CHANNEL_READ:
            ret = sys_channel_read((ab_handle_t)frame->r[0],
                                   (uint32_t)frame->r[1],
                                   (ab_channel_read_args_t *)frame->r[2]);
            break;
        case SYS_VMO_CREATE:
            ret = sys_vmo_create((uint64_t)frame->r[0],
                                 (uint32_t)frame->r[1],
                                 (ab_handle_t *)frame->r[2]);
            break;
        case SYS_VMO_READ:
            ret = sys_vmo_read((ab_handle_t)frame->r[0],
                               (void *)frame->r[1],
                               (uint64_t)frame->r[2],
                               (size_t)frame->r[3]);
            break;
        case SYS_VMO_WRITE:
            ret = sys_vmo_write((ab_handle_t)frame->r[0],
                                (const void *)frame->r[1],
                                (uint64_t)frame->r[2],
                                (size_t)frame->r[3]);
            break;
        case SYS_VMAR_MAP:
            ret = sys_vmar_map((ab_handle_t)frame->r[0],
                               (size_t)frame->r[1],
                               (const ab_vmar_map_args_t *)frame->r[2]);
            break;
        case SYS_PROCESS_CREATE:
            ret = sys_process_create((ab_handle_t)frame->r[0],
                                     (const ab_process_create_args_t *)frame->r[1]);
            break;
        case SYS_THREAD_CREATE:
            ret = sys_thread_create((ab_handle_t)frame->r[0],
                                    (const char *)frame->r[1],
                                    (size_t)frame->r[2],
                                    (ab_handle_t *)frame->r[3]);
            break;
        case SYS_PROCESS_START:
            ret = sys_process_start((ab_handle_t)frame->r[0],
                                    (const ab_process_start_args_t *)frame->r[1]);
            break;
        case SYS_VMO_CREATE_PHYSICAL:
            ret = sys_vmo_create_physical((ab_handle_t)frame->r[0],
                                          (const ab_vmo_create_phys_args_t *)frame->r[1]);
            break;
        case SYS_VMO_CREATE_CONTIG:
            ret = sys_vmo_create_contiguous((ab_handle_t)frame->r[0],
                                            (const ab_vmo_create_contig_args_t *)frame->r[1]);
            break;
        case SYS_INTERRUPT_CREATE:
            ret = sys_interrupt_create((ab_handle_t)frame->r[0],
                                       (uint32_t)frame->r[1],
                                       (ab_handle_t *)frame->r[2]);
            break;
        case SYS_INTERRUPT_WAIT:
            ret = sys_interrupt_wait((ab_handle_t)frame->r[0]);
            break;
        case SYS_INTERRUPT_ACK:
            ret = sys_interrupt_ack((ab_handle_t)frame->r[0]);
            break;
        default:
            dprintf(CRITICAL, "Unknown syscall %lu\n", sys_num);
            ret = AB_ERR_NOT_SUPPORTED;
            break;
    }

    frame->r[0] = (uint64_t)(int64_t)ret;
}
