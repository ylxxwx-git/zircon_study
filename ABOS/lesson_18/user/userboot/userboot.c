/* =========================================================================
 * user/userboot/userboot.c - EL0 Root Bootstrap Service ('userboot')
 *
 * Lesson 18: Spawns console-srv, fs-srv, and 4 independent virtual terminal
 * shell processes (abos-shell-0 .. abos-shell-3), distributing 4 pairs of
 * console and filesystem capability channels.
 * ========================================================================= */
#include "ulibc.h"

static ab_handle_t spawn_service(ab_handle_t root_job_h, const char *name,
                                 uintptr_t entry_fn, ab_handle_t *handles,
                                 uint32_t num_handles, uint64_t arg2,
                                 ab_handle_t *out_vmar_h) {
    ab_handle_t proc_h = 0, vmar_h = 0, thr_h = 0;
    sys_process_create(root_job_h, name, &proc_h, &vmar_h);
    sys_thread_create(proc_h, name, &thr_h);

    ab_handle_t p_ch = 0, c_ch = 0;
    sys_channel_create(0, &p_ch, &c_ch);

    handles[1] = vmar_h; /* Slot 1 is always the new process's Root VMAR handle */
    uint32_t tag = 0x53525621U;
    sys_channel_write(p_ch, &tag, sizeof(tag), handles, num_handles);
    sys_handle_close(p_ch);

    sys_process_start(proc_h, thr_h, entry_fn, 0, c_ch, arg2);
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

    ulog(root_log_h, "[userboot] Spawning console-srv, fs-srv, and 4 virtual terminal shells (tty0..tty3)!");

    ab_handle_t con_srv_end[NUM_VTS], con_cli_end[NUM_VTS];
    ab_handle_t fs_srv_end[NUM_VTS],  fs_cli_end[NUM_VTS];
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        sys_channel_create(0, &con_srv_end[i], &con_cli_end[i]);
        sys_channel_create(0, &fs_srv_end[i],  &fs_cli_end[i]);
    }

    /* 1. Spawn 'console-srv' with [log, vmar, resource, con_srv_end[0..3]] (7 handles) */
    ab_handle_t log1 = 0, res1 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log1);
    sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res1);
    ab_handle_t con_h[3 + NUM_VTS] = {
        log1, 0, res1,
        con_srv_end[0], con_srv_end[1], con_srv_end[2], con_srv_end[3]
    };
    ab_handle_t con_proc = spawn_service(root_job_h, "console-srv",
                                         (uintptr_t)console_server_entry,
                                         con_h, 3 + NUM_VTS, 0, NULL);

    /* 2. Spawn 'fs-srv' with [log, vmar, resource, fs_srv_end[0..3]] (7 handles) */
    ab_handle_t log2 = 0, res2 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log2);
    sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res2);
    ab_handle_t fs_h[3 + NUM_VTS] = {
        log2, 0, res2,
        fs_srv_end[0], fs_srv_end[1], fs_srv_end[2], fs_srv_end[3]
    };
    ab_handle_t fs_proc = spawn_service(root_job_h, "fs-srv",
                                        (uintptr_t)fs_server_entry,
                                        fs_h, 3 + NUM_VTS, 0, NULL);

    /* 3. Spawn 4 virtual terminal shells: 'abos-shell-0' .. 'abos-shell-3' */
    ab_handle_t sh_procs[NUM_VTS];
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        char sh_name[24];
        u_snprintf(sh_name, sizeof(sh_name), "abos-shell-%u", i);

        ab_handle_t log_s = 0, job_s = 0;
        sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log_s);
        sys_handle_duplicate(root_job_h, AB_RIGHT_SAME_RIGHTS, &job_s);
        ab_handle_t sh_h[5] = { log_s, 0, con_cli_end[i], fs_cli_end[i], job_s };
        sh_procs[i] = spawn_service(root_job_h, sh_name,
                                    (uintptr_t)shell_entry,
                                    sh_h, 5, (uint64_t)i, NULL);
    }

    ab_signals_t obs = 0;
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        sys_object_wait_one(sh_procs[i], AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
    }
    sys_object_wait_one(fs_proc,  AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
    sys_object_wait_one(con_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);

    ulog(root_log_h, "[userboot] All 4 virtual terminals and user-space servers exited cleanly!");
    sys_process_exit(0);
}
