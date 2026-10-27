/* =========================================================================
 * user/userboot/userboot.c - EL0 Root Bootstrap Service ('userboot')
 * ========================================================================= */
#include "ulibc.h"

static ab_handle_t spawn_service(ab_handle_t root_job_h, const char *name,
                                 uintptr_t entry_fn, ab_handle_t *handles,
                                 uint32_t num_handles, ab_handle_t *out_vmar_h) {
    ab_handle_t proc_h = 0, vmar_h = 0, thr_h = 0;
    sys_process_create(root_job_h, name, &proc_h, &vmar_h);
    sys_thread_create(proc_h, name, &thr_h);

    ab_handle_t p_ch = 0, c_ch = 0;
    sys_channel_create(0, &p_ch, &c_ch);

    handles[1] = vmar_h; /* Slot 1 is always the new process's Root VMAR handle */
    uint32_t tag = 0x53525621U;
    sys_channel_write(p_ch, &tag, sizeof(tag), handles, num_handles);
    sys_handle_close(p_ch);

    sys_process_start(proc_h, thr_h, entry_fn, 0, c_ch, 0);
    sys_handle_close(thr_h);
    if (out_vmar_h) *out_vmar_h = vmar_h;
    return proc_h;
}

void userboot_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t boot_h[3];
    sys_channel_read(boot_ch, &tag, sizeof(tag), boot_h, 3, &act_b, &act_h);

    ab_handle_t root_job_h = boot_h[0];
    ab_handle_t root_log_h = boot_h[1];
    ab_handle_t root_res_h = boot_h[2];

    ulog(root_log_h, "[userboot] Spawning EL0 microkernel servers: console-srv, fs-srv, abos-shell!");

    ab_handle_t con_srv_end = 0, con_cli_end = 0;
    ab_handle_t fs_srv_end  = 0, fs_cli_end  = 0;
    sys_channel_create(0, &con_srv_end, &con_cli_end);
    sys_channel_create(0, &fs_srv_end,  &fs_cli_end);

    /* 1. Spawn 'console-srv' with [log, vmar, resource, con_srv_end] */
    ab_handle_t log1 = 0, res1 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log1);
    sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res1);
    ab_handle_t con_h[4] = { log1, 0, res1, con_srv_end };
    ab_handle_t con_proc = spawn_service(root_job_h, "console-srv",
                                         (uintptr_t)console_server_entry, con_h, 4, NULL);

    /* 2. Spawn 'fs-srv' with [log, vmar, resource, fs_srv_end] */
    ab_handle_t log2 = 0, res2 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log2);
    sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res2);
    ab_handle_t fs_h[4] = { log2, 0, res2, fs_srv_end };
    ab_handle_t fs_proc = spawn_service(root_job_h, "fs-srv",
                                        (uintptr_t)fs_server_entry, fs_h, 4, NULL);

    /* 3. Spawn 'abos-shell' with [log, vmar, con_cli_end, fs_cli_end, job_h] */
    ab_handle_t log3 = 0, job3 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log3);
    sys_handle_duplicate(root_job_h, AB_RIGHT_SAME_RIGHTS, &job3);
    ab_handle_t sh_h[5] = { log3, 0, con_cli_end, fs_cli_end, job3 };
    ab_handle_t sh_proc = spawn_service(root_job_h, "abos-shell",
                                        (uintptr_t)shell_entry, sh_h, 5, NULL);

    ab_signals_t obs = 0;
    sys_object_wait_one(sh_proc,  AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
    sys_object_wait_one(fs_proc,  AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
    sys_object_wait_one(con_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);

    ulog(root_log_h, "[userboot] All user-space services exited cleanly!");
    sys_process_exit(0);
}
