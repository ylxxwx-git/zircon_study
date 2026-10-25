/* =========================================================================
 * syscalls.c - Capability-Checked System Call Implementations
 * References:
 *   - zircon_org/kernel/syscalls/handle_ops.cpp
 *   - zircon_org/kernel/syscalls/object.cpp
 *   - zircon_org/kernel/syscalls/ddk.cpp (sys_debuglog_create / write)
 * ========================================================================= */
#include "syscalls.h"
#include "debuglog.h"
#include "thread.h"

static ab_status_t sys_handle_close(ab_handle_t handle) {
    ab_process_t *proc = get_current_process();
    return handle_close(proc, handle);
}

static ab_status_t sys_handle_duplicate(ab_handle_t handle, ab_rights_t rights,
                                        ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    return handle_duplicate(proc, handle, rights, user_out);
}

static ab_status_t sys_handle_replace(ab_handle_t handle, ab_rights_t rights,
                                      ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    return handle_replace(proc, handle, rights, user_out);
}

static ab_status_t sys_object_get_info(ab_handle_t handle, ab_handle_basic_info_t *user_info) {
    if (!user_info) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_object_t *obj = NULL;
    ab_rights_t rights = 0;

    ab_status_t st = handle_lookup(proc, handle, AB_OBJ_TYPE_NONE, AB_RIGHT_INSPECT,
                                   &obj, &rights);
    if (st != AB_OK) return st;

    user_info->koid         = obj->koid;
    user_info->type         = obj->type;
    user_info->rights       = rights;
    user_info->handle_count = (uint32_t)obj->handle_count;

    ab_object_release(obj);
    return AB_OK;
}

static ab_status_t sys_debuglog_create(uint32_t options, ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();

    ab_object_t *log_obj = NULL;
    ab_status_t st = ab_log_dispatcher_create(options, &log_obj);
    if (st != AB_OK) return st;

    st = handle_alloc(proc, log_obj, AB_RIGHTS_DEFAULT_LOG, user_out);
    ab_object_release(log_obj); /* Drop creation ref; handle table holds its ref */
    return st;
}

static ab_status_t sys_debuglog_write(ab_handle_t log_handle, uint32_t options,
                                      const char *buf, size_t len) {
    (void)options;
    if (!buf || len > 256) return AB_ERR_INVALID_ARGS;

    ab_process_t *proc = get_current_process();
    ab_object_t *obj = NULL;

    /* Must be a valid AB_OBJ_TYPE_LOG handle with AB_RIGHT_WRITE! */
    ab_status_t st = handle_lookup(proc, log_handle, AB_OBJ_TYPE_LOG, AB_RIGHT_WRITE,
                                   &obj, NULL);
    if (st != AB_OK) return st;

    char msg[257];
    for (size_t i = 0; i < len; i++) {
        msg[i] = buf[i];
    }
    msg[len] = '\0';

    dprintf(INFO, "[debuglog proc='%s' koid=%lu] %s\n", proc->name, obj->koid, msg);
    ab_object_release(obj);
    return AB_OK;
}

static void sys_process_exit(int64_t retcode) {
    ab_process_t *proc = get_current_process();
    if (proc) {
        proc->exit_code = (int)retcode;
        dprintf(INFO, "[process '%s' koid=%lu] exiting with code %ld; closing remaining handles\n",
                proc->name, proc->base.koid, retcode);
        for (uint32_t slot = 0; slot < MAX_HANDLES_PER_PROCESS; slot++) {
            if (proc->handles[slot].obj != NULL) {
                ab_handle_t h = (0x5U << 28) | ((proc->handles[slot].generation & 0xFFFFU) << 8) | slot;
                handle_close(proc, h);
            }
        }
    }
    thread_exit((int)retcode);
}

void syscall_dispatch(iframe_t *iframe) {
    uint64_t sys_num = iframe->r[8];
    ab_status_t ret = AB_ERR_BAD_SYSCALL;

    switch (sys_num) {
        case SYS_HANDLE_CLOSE:
            ret = sys_handle_close((ab_handle_t)iframe->r[0]);
            break;
        case SYS_HANDLE_DUPLICATE:
            ret = sys_handle_duplicate((ab_handle_t)iframe->r[0],
                                       (ab_rights_t)iframe->r[1],
                                       (ab_handle_t *)(uintptr_t)iframe->r[2]);
            break;
        case SYS_HANDLE_REPLACE:
            ret = sys_handle_replace((ab_handle_t)iframe->r[0],
                                     (ab_rights_t)iframe->r[1],
                                     (ab_handle_t *)(uintptr_t)iframe->r[2]);
            break;
        case SYS_OBJECT_GET_INFO:
            ret = sys_object_get_info((ab_handle_t)iframe->r[0],
                                      (ab_handle_basic_info_t *)(uintptr_t)iframe->r[1]);
            break;
        case SYS_DEBUGLOG_CREATE:
            ret = sys_debuglog_create((uint32_t)iframe->r[0],
                                      (ab_handle_t *)(uintptr_t)iframe->r[1]);
            break;
        case SYS_DEBUGLOG_WRITE:
            ret = sys_debuglog_write((ab_handle_t)iframe->r[0],
                                     (uint32_t)iframe->r[1],
                                     (const char *)(uintptr_t)iframe->r[2],
                                     (size_t)iframe->r[3]);
            break;
        case SYS_PROCESS_EXIT:
            sys_process_exit((int64_t)iframe->r[0]);
            break;
        default:
            dprintf(CRITICAL, "[syscall] Unknown syscall #%lu\n", sys_num);
            ret = AB_ERR_BAD_SYSCALL;
            break;
    }

    iframe->r[0] = (uint64_t)(int64_t)ret;
}
