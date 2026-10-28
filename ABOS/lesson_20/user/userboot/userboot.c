/* =========================================================================
 * user/userboot/userboot.c - EL0 Root Bootstrap Service ('userboot')
 *
 * Lesson 20: True Zircon-style BOOTFS bootstrapping!
 *   - Only 'userboot' is embedded in the kernel image.
 *   - The kernel passes a read-only BOOTFS VMO handle containing the
 *     standalone user-space executables (console-srv.bin, fs-srv.bin,
 *     and shell.bin, all linked with user/link.ld at VA 0x20000000).
 *   - 'userboot' maps the BOOTFS VMO in EL0, extracts each executable into
 *     its own VMO, maps it at VA 0x20000000 in the target process's VMAR,
 *     and starts console-srv, fs-srv, and abos-shell-0..3.
 * ========================================================================= */
#include "ulibc.h"

static uint32_t parse_octal(const char *s, size_t n) {
    uint32_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
        v = (v << 3) + (uint32_t)(s[i] - '0');
    }
    return v;
}

static const uint8_t *bootfs_find_file(const uint8_t *bootfs, size_t bootfs_sz,
                                       const char *name, uint32_t *out_size) {
    size_t off = 0;
    while (off + 512 <= bootfs_sz) {
        const char *hdr = (const char *)(bootfs + off);
        if (hdr[0] == '\0') break;
        uint32_t fsz = parse_octal(hdr + 124, 12);
        if (u_strcmp(hdr, name) == 0 && off + 512 + fsz <= bootfs_sz) {
            if (out_size) *out_size = fsz;
            return bootfs + off + 512;
        }
        off += 512 + ((fsz + 511U) & ~511U);
    }
    return NULL;
}

static ab_handle_t spawn_from_bootfs(ab_handle_t root_job_h,
                                     const uint8_t *bootfs, size_t bootfs_sz,
                                     const char *bin_name, const char *proc_name,
                                     ab_handle_t *handles, uint32_t num_handles,
                                     uint64_t arg2) {
    uint32_t fsz = 0;
    const uint8_t *bin_data = bootfs_find_file(bootfs, bootfs_sz, bin_name, &fsz);
    if (!bin_data || fsz == 0) {
        return AB_HANDLE_INVALID;
    }

    uint64_t map_sz = ((uint64_t)fsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (map_sz == 0) map_sz = PAGE_SIZE;

    ab_handle_t bin_vmo = AB_HANDLE_INVALID;
    sys_vmo_create(map_sz, 0, &bin_vmo);
    sys_object_set_name(bin_vmo, bin_name);
    sys_vmo_write(bin_vmo, bin_data, 0, fsz);

    ab_handle_t proc_h = 0, vmar_h = 0, thr_h = 0;
    sys_process_create(root_job_h, proc_name, &proc_h, &vmar_h);
    sys_thread_create(proc_h, proc_name, &thr_h);

    /* Map standalone executable VMO at 0x20000000 (offset 0x10000000 in Root VMAR) */
    vaddr_t entry_va = 0;
    sys_vmar_map(vmar_h, 0x10000000ULL, bin_vmo, 0, map_sz,
                AB_VM_PERM_READ | AB_VM_PERM_WRITE | AB_VM_PERM_EXECUTE,
                &entry_va);
    sys_handle_close(bin_vmo);

    ab_handle_t p_ch = 0, c_ch = 0;
    sys_channel_create(0, &p_ch, &c_ch);

    handles[1] = vmar_h; /* Slot 1 is always the new process's Root VMAR handle */
    uint32_t tag = 0x53525621U;
    sys_channel_write(p_ch, &tag, sizeof(tag), handles, num_handles);
    sys_handle_close(p_ch);

    sys_process_start(proc_h, thr_h, entry_va, 0, c_ch, arg2);
    sys_handle_close(thr_h);
    return proc_h;
}

void userboot_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint64_t bootfs_sz = 0;
    uint32_t act_b = 0, act_h = 0;
    ab_handle_t boot_h[5];
    sys_channel_read(boot_ch, &bootfs_sz, sizeof(bootfs_sz), boot_h, 5, &act_b, &act_h);
    sys_handle_close(boot_ch);

    ab_handle_t root_job_h   = boot_h[0];
    ab_handle_t root_log_h   = boot_h[1];
    ab_handle_t root_res_h   = boot_h[2];
    ab_handle_t ub_vmar_h    = boot_h[3];
    ab_handle_t bootfs_vmo_h = boot_h[4];

    /* Map the BOOTFS VMO read-only into userboot's own VMAR */
    uint64_t bootfs_map_sz = (bootfs_sz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    vaddr_t bootfs_va = 0;
    sys_vmar_map(ub_vmar_h, 0, bootfs_vmo_h, 0, bootfs_map_sz,
                AB_VM_PERM_READ, &bootfs_va);
    const uint8_t *bootfs = (const uint8_t *)bootfs_va;

    ulog(root_log_h, "[userboot] Mapped BOOTFS VMO! Loading console-srv.bin, fs-srv.bin, and shell.bin @ 0x20000000...");

    ab_handle_t con_srv_end[NUM_VTS], con_cli_end[NUM_VTS];
    ab_handle_t fs_srv_end[NUM_VTS],  fs_cli_end[NUM_VTS];
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        sys_channel_create(0, &con_srv_end[i], &con_cli_end[i]);
        sys_channel_create(0, &fs_srv_end[i],  &fs_cli_end[i]);
    }

    /* 1. Spawn standalone 'console-srv.bin' from BOOTFS */
    ab_handle_t log1 = 0, res1 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log1);
    sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res1);
    ab_handle_t con_h[3 + NUM_VTS] = {
        log1, 0, res1,
        con_srv_end[0], con_srv_end[1], con_srv_end[2], con_srv_end[3]
    };
    ab_handle_t con_proc = spawn_from_bootfs(root_job_h, bootfs, (size_t)bootfs_sz,
                                             "console-srv.bin", "console-srv",
                                             con_h, 3 + NUM_VTS, 0);

    /* 2. Spawn standalone 'fs-srv.bin' from BOOTFS */
    ab_handle_t log2 = 0, res2 = 0;
    sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log2);
    sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res2);
    ab_handle_t fs_h[3 + NUM_VTS] = {
        log2, 0, res2,
        fs_srv_end[0], fs_srv_end[1], fs_srv_end[2], fs_srv_end[3]
    };
    ab_handle_t fs_proc = spawn_from_bootfs(root_job_h, bootfs, (size_t)bootfs_sz,
                                            "fs-srv.bin", "fs-srv",
                                            fs_h, 3 + NUM_VTS, 0);

    /* 3. Spawn 4 virtual terminal shells from standalone 'shell.bin' in BOOTFS */
    ab_handle_t sh_procs[NUM_VTS];
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        char sh_name[24];
        u_snprintf(sh_name, sizeof(sh_name), "abos-shell-%u", i);

        ab_handle_t log_s = 0, job_s = 0;
        sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log_s);
        sys_handle_duplicate(root_job_h, AB_RIGHT_SAME_RIGHTS, &job_s);
        ab_handle_t sh_h[5] = { log_s, 0, con_cli_end[i], fs_cli_end[i], job_s };
        sh_procs[i] = spawn_from_bootfs(root_job_h, bootfs, (size_t)bootfs_sz,
                                        "shell.bin", sh_name,
                                        sh_h, 5, (uint64_t)i);
    }

    /* Release BOOTFS VMO handle once all initial processes are spawned */
    sys_handle_close(bootfs_vmo_h);
    sys_handle_close(ub_vmar_h);

    ab_signals_t obs = 0;
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        sys_object_wait_one(sh_procs[i], AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
    }
    sys_object_wait_one(fs_proc,  AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
    sys_object_wait_one(con_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);

    ulog(root_log_h, "[userboot] All 4 virtual terminals and user-space servers exited cleanly!");
    sys_process_exit(0);
}
