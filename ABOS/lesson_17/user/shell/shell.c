/* =========================================================================
 * user/shell/shell.c - EL0 Interactive Shell ('abos-shell')
 *
 * Lesson 17: Adds Zircon-style 'ps' and 'vmaps' commands for inspecting
 * the Job/Process/Thread hierarchy, per-process capability handle counts,
 * and Virtual Memory Address Space / VMAR / VMO mappings.
 *
 * References:
 *   - zircon_org/system/uapp/psutils/ps.c
 *   - zircon_org/system/uapp/psutils/vmaps.c
 *   - zircon_org/kernel/object/diagnostics.cpp
 * ========================================================================= */
#include "ulibc.h"

static void cmd_ps_help(ab_handle_t con_ch) {
    shell_print(con_ch,
        "Usage: ps [-T|-J|help]\n"
        "  ps      - Show Job/Process/Thread tree + Zircon handle stats table\n"
        "  ps -T   - Include kernel/user threads in task tree\n"
        "  ps -J   - Only show Jobs in task tree\n"
        "  ps help - Show column legend (from Zircon diagnostics.cpp)\n\n"
        "Handle Table Columns (Zircon DumpProcessListKeyMap):\n"
        "  id  : process koid\n"
        "  #h  : total number of open handles in process\n"
        "  #jb : job handles           #pr : process handles\n"
        "  #th : thread handles        #vo : vmo handles\n"
        "  #vm : vmar handles          #ch : channel handles\n"
        "  #ev : event/eventpair       #po : port handles\n"
        "  #ir : interrupt handles     #rs : resource handles\n"
        "  #lg : debuglog handles\n");
}

static void cmd_ps(ab_handle_t con_ch, ab_handle_t job_h, const char *args) {
    while (*args == ' ') args++;
    if (u_strcmp(args, "help") == 0 || u_strcmp(args, "--help") == 0) {
        cmd_ps_help(con_ch);
        return;
    }

    int only_jobs    = (u_strcmp(args, "-J") == 0);
    int show_threads = !only_jobs;

    ab_task_info_entry_t entries[AB_MAX_PS_ENTRIES];
    uint32_t count = 0;
    ab_status_t st = sys_job_get_ps_info(job_h, entries, AB_MAX_PS_ENTRIES, &count);
    if (st != AB_OK) {
        con_printf(con_ch, "ps: sys_job_get_ps_info failed (status=%d)\n", st);
        return;
    }

    con_printf(con_ch, "%-14s %7s %7s %7s %-8s %s\n",
               "TASK", "PSS", "PRIVATE", "SHARED", "STATE", "NAME");

    for (uint32_t i = 0; i < count; i++) {
        ab_task_info_entry_t *e = &entries[i];
        if (only_jobs && e->type != 'j') continue;
        if (!show_threads && e->type == 't') continue;

        char task_id[24];
        if (e->depth == 0) {
            u_snprintf(task_id, sizeof(task_id), "%c:%lu", e->type, e->koid);
        } else if (e->depth == 1) {
            u_snprintf(task_id, sizeof(task_id), "  %c:%lu", e->type, e->koid);
        } else {
            u_snprintf(task_id, sizeof(task_id), "    %c:%lu", e->type, e->koid);
        }

        char pss_str[16]  = "";
        char priv_str[16] = "";
        char sh_str[16]   = "";

        if (e->type == 'j' || e->type == 'p') {
            u_format_size(pss_str, sizeof(pss_str), e->pss_bytes);
            u_format_size(priv_str, sizeof(priv_str), e->private_bytes);
        }
        if (e->type == 'p') {
            u_format_size(sh_str, sizeof(sh_str), e->shared_bytes);
        } else if (e->type == 't' && e->private_bytes > 0) {
            u_format_size(priv_str, sizeof(priv_str), e->private_bytes);
        }

        con_printf(con_ch, "%-14s %7s %7s %7s %-8s %s\n",
                   task_id, pss_str, priv_str, sh_str, e->state, e->name);
    }

    if (!only_jobs) {
        con_printf(con_ch,
            "\nZircon Kernel Process Handle Table (DumpProcessList):\n"
            "%7s  #h:  #jb #pr #th #vo #vm #ch #ev #po #ir #rs #lg [name]\n", "id");
        for (uint32_t i = 0; i < count; i++) {
            ab_task_info_entry_t *e = &entries[i];
            if (e->type != 'p') continue;
            uint32_t *hc = e->handle_counts;
            con_printf(con_ch,
                "%7lu %3u:  %3u %3u %3u %3u %3u %3u %3u %3u %3u %3u %3u [%s]\n",
                e->koid,
                e->total_handles,
                hc[AB_OBJ_TYPE_JOB],
                hc[AB_OBJ_TYPE_PROCESS],
                hc[AB_OBJ_TYPE_THREAD],
                hc[AB_OBJ_TYPE_VMO],
                hc[AB_OBJ_TYPE_VMAR],
                hc[AB_OBJ_TYPE_CHANNEL],
                hc[AB_OBJ_TYPE_EVENT] + hc[AB_OBJ_TYPE_EVENTPAIR],
                hc[AB_OBJ_TYPE_PORT],
                hc[AB_OBJ_TYPE_INTERRUPT],
                hc[AB_OBJ_TYPE_RESOURCE],
                hc[AB_OBJ_TYPE_LOG],
                e->name);
        }
    }
}

static void cmd_vmaps_help(ab_handle_t con_ch) {
    shell_print(con_ch,
        "Usage: vmaps [<process-koid>|all|help]\n\n"
        "Dumps a process's virtual memory address space, VMARs, and VMO mappings\n"
        "(modeled after Zircon's system/uapp/psutils/vmaps.c & GetVmAspaceMaps).\n\n"
        "First column:\n"
        "  \"/A\" -- Process virtual address space (VmAspace)\n"
        "  \"/R\" -- Root VMAR (VmAddressRegion)\n"
        "  \"R\"  -- Child VMAR (R for Region)\n"
        "  \"M\"  -- VMO Mapping (VmMapping)\n\n"
        "Indentation indicates parent/child relationship in the VmAspace tree.\n");
}

static void print_one_process_vmaps(ab_handle_t con_ch, ab_handle_t job_h, ab_koid_t proc_koid) {
    ab_info_maps_t maps[AB_MAX_VMAPS];
    uint32_t count = 0;
    ab_status_t st = sys_process_get_maps(job_h, proc_koid, maps, AB_MAX_VMAPS, &count);
    if (st != AB_OK) {
        con_printf(con_ch, "vmaps: process with koid %lu not found (status=%d)\n",
                   proc_koid, st);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        ab_info_maps_t *e = &maps[i];
        const char *prefix = "   ";
        if (e->type == AB_INFO_MAPS_TYPE_ASPACE) {
            prefix = "/A ";
        } else if (e->type == AB_INFO_MAPS_TYPE_VMAR) {
            prefix = (e->depth <= 1) ? "/R " : " R ";
        } else if (e->type == AB_INFO_MAPS_TYPE_MAPPING) {
            prefix = (e->depth <= 2) ? " M " : "  M";
        }

        char sz_str[16];
        u_format_size(sz_str, sizeof(sz_str), e->size);
        vaddr_t end_va = e->base + e->size;

        if (e->type == AB_INFO_MAPS_TYPE_MAPPING) {
            char perms[4];
            perms[0] = (e->mapping.mmu_flags & AB_VM_PERM_READ)    ? 'r' : '-';
            perms[1] = (e->mapping.mmu_flags & AB_VM_PERM_WRITE)   ? 'w' : '-';
            perms[2] = (e->mapping.mmu_flags & AB_VM_PERM_EXECUTE) ? 'x' : '-';
            perms[3] = '\0';

            char res_str[16];
            u_format_size(res_str, sizeof(res_str), e->mapping.committed_pages * PAGE_SIZE);

            con_printf(con_ch,
                "%s %016lx-%016lx %s %6s:sz %5s:res %5lu:vmo '%s'\n",
                prefix, e->base, end_va, perms, sz_str, res_str,
                e->mapping.vmo_koid, e->name);
        } else {
            con_printf(con_ch,
                "%s %016lx-%016lx     %6s:sz                     '%s'\n",
                prefix, e->base, end_va, sz_str, e->name);
        }
    }
}

static void cmd_vmaps(ab_handle_t con_ch, ab_handle_t job_h, const char *args) {
    while (*args == ' ') args++;
    if (u_strcmp(args, "help") == 0 || u_strcmp(args, "--help") == 0 || u_strcmp(args, "-h") == 0) {
        cmd_vmaps_help(con_ch);
        return;
    }

    if (*args == '\0') {
        /* Default with no args: dump current process (0) and mention 'vmaps <koid>' / 'vmaps all' */
        print_one_process_vmaps(con_ch, job_h, 0);
        return;
    }

    if (u_strcmp(args, "all") == 0) {
        ab_task_info_entry_t entries[AB_MAX_PS_ENTRIES];
        uint32_t count = 0;
        if (sys_job_get_ps_info(job_h, entries, AB_MAX_PS_ENTRIES, &count) == AB_OK) {
            for (uint32_t i = 0; i < count; i++) {
                if (entries[i].type == 'p') {
                    con_printf(con_ch, "--- Process [koid=%lu, asid=%u, name='%s'] ---\n",
                               entries[i].koid, entries[i].asid, entries[i].name);
                    print_one_process_vmaps(con_ch, job_h, entries[i].koid);
                }
            }
        }
        return;
    }

    int ok = 0;
    int64_t koid_val = u_atoi(args, &ok);
    if (!ok || koid_val <= 0) {
        con_printf(con_ch, "vmaps: invalid koid '%s'. Usage: vmaps [<process-koid>|all|help]\n", args);
        return;
    }
    print_one_process_vmaps(con_ch, job_h, (ab_koid_t)koid_val);
}

void shell_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[5];
    sys_channel_read(boot_ch, &tag, sizeof(tag), h, 5, &act_b, &act_h);
    sys_handle_close(boot_ch);

    ab_handle_t log_h  = h[0];
    ab_handle_t vmar_h = h[1];
    ab_handle_t con_ch = h[2];
    ab_handle_t fs_ch  = h[3];
    ab_handle_t job_h  = h[4];

    ulog(log_h, "[abos-shell EL0] Started with VMAR + ConsoleChannel + FSChannel + Job capabilities!");

    /* Wait for fs-srv to finish VirtIO-Blk DMA initialization before printing banner */
    {
        fs_msg_t ping = { FS_OP_LIST, 0, 0, "" };
        ab_signals_t obs = 0;
        sys_channel_write(fs_ch, &ping, sizeof(ping), NULL, 0);
        sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
        sys_channel_read(fs_ch, &ping, sizeof(ping), NULL, 0, &act_b, &act_h);
    }

    shell_print(con_ch, "\n============================================================\n");
    shell_print(con_ch, "  ABOS Microkernel Shell (EL0) - Type 'help' for commands\n");
    shell_print(con_ch, "============================================================\n");

    char line[128];
    for (;;) {
        shell_print(con_ch, "abos$ ");
        shell_readline(con_ch, line, sizeof(line));

        if (line[0] == '\0') {
            continue;
        } else if (u_strcmp(line, "help") == 0) {
            shell_print(con_ch,
                "Available commands:\n"
                "  help              - Show this command list\n"
                "  ps [-T|-J|help]   - List Jobs, Processes, Threads & Handle stats (Zircon ps)\n"
                "  vmaps [koid|all]  - Dump process VmAspace/VMAR/VMO mappings (Zircon vmaps)\n"
                "  caps              - Inspect this shell's capability handle table\n"
                "  ls                - List files in TAR disk.img (via EL0 fs-srv IPC)\n"
                "  cat <file>        - Map read-only VMO of <file> from fs-srv & print\n"
                "  calc              - Load & execute 'calc.bin' from TAR disk at 0x20000000\n"
                "  exit              - Gracefully shut down console-srv, fs-srv, and shell\n");
        } else if (u_strcmp(line, "ps") == 0) {
            cmd_ps(con_ch, job_h, "");
        } else if (u_strncmp(line, "ps ", 3) == 0) {
            cmd_ps(con_ch, job_h, line + 3);
        } else if (u_strcmp(line, "vmaps") == 0) {
            cmd_vmaps(con_ch, job_h, "");
        } else if (u_strncmp(line, "vmaps ", 6) == 0) {
            cmd_vmaps(con_ch, job_h, line + 6);
        } else if (u_strcmp(line, "caps") == 0) {
            ab_handle_basic_info_t info;
            con_printf(con_ch, "Shell Capability Table (inspected via sys_object_get_info):\n");
            sys_object_get_info(log_h, &info);
            con_printf(con_ch, "  [Handle 0x%x] Kernel DebugLog     (koid=%lu, type=%u, rights=0x%x)\n",
                       log_h, info.koid, info.type, info.rights);
            sys_object_get_info(vmar_h, &info);
            con_printf(con_ch, "  [Handle 0x%x] Root Address Space  (koid=%lu, type=%u, rights=0x%x)\n",
                       vmar_h, info.koid, info.type, info.rights);
            sys_object_get_info(con_ch, &info);
            con_printf(con_ch, "  [Handle 0x%x] Console IPC Channel (koid=%lu, type=%u, rights=0x%x)\n",
                       con_ch, info.koid, info.type, info.rights);
            sys_object_get_info(fs_ch, &info);
            con_printf(con_ch, "  [Handle 0x%x] Filesystem IPC Chan (koid=%lu, type=%u, rights=0x%x)\n",
                       fs_ch, info.koid, info.type, info.rights);
            sys_object_get_info(job_h, &info);
            con_printf(con_ch, "  [Handle 0x%x] Parent Job Cap      (koid=%lu, type=%u, rights=0x%x)\n",
                       job_h, info.koid, info.type, info.rights);
            con_printf(con_ch, "  [Note] Shell holds ZERO Resource/MMIO/IRQ handles (pure capability isolation!)\n");
        } else if (u_strcmp(line, "ls") == 0) {
            fs_msg_t req = { FS_OP_LIST, 0, 0, "" };
            sys_channel_write(fs_ch, &req, sizeof(req), NULL, 0);

            ab_signals_t obs = 0;
            sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
            fs_msg_t rep;
            sys_channel_read(fs_ch, &rep, sizeof(rep), NULL, 0, &act_b, &act_h);
            shell_print(con_ch, "Files in TAR disk.img (served by EL0 fs-srv):\n");
            shell_print(con_ch, rep.data);
        } else if (u_strncmp(line, "cat ", 4) == 0) {
            const char *fname = line + 4;
            while (*fname == ' ') fname++;

            fs_msg_t req;
            req.op = FS_OP_OPEN_VMO;
            u_strcpy(req.data, fname);
            sys_channel_write(fs_ch, &req, sizeof(req), NULL, 0);

            ab_signals_t obs = 0;
            sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
            fs_msg_t rep;
            ab_handle_t file_vmo_h = AB_HANDLE_INVALID;
            sys_channel_read(fs_ch, &rep, sizeof(rep), &file_vmo_h, 1, &act_b, &act_h);

            if (rep.status == 0 && act_h == 1 && file_vmo_h != AB_HANDLE_INVALID) {
                vaddr_t file_va = 0;
                sys_vmar_map(vmar_h, 0, file_vmo_h, 0, PAGE_SIZE, AB_VM_PERM_READ, &file_va);
                shell_print(con_ch, (const char *)file_va);
                sys_handle_close(file_vmo_h);
            } else {
                shell_print(con_ch, "cat: file not found in TAR archive\n");
            }
        } else if (u_strcmp(line, "calc") == 0 || u_strcmp(line, "calc.bin") == 0) {
            /* 1. Request 'calc.bin' executable VMO from EL0 FS Server over fs_ch! */
            fs_msg_t req;
            req.op = FS_OP_OPEN_VMO;
            u_strcpy(req.data, "calc.bin");
            sys_channel_write(fs_ch, &req, sizeof(req), NULL, 0);

            ab_signals_t obs = 0;
            sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
            fs_msg_t rep;
            ab_handle_t bin_vmo_h = AB_HANDLE_INVALID;
            sys_channel_read(fs_ch, &rep, sizeof(rep), &bin_vmo_h, 1, &act_b, &act_h);

            if (rep.status != 0 || act_h != 1 || bin_vmo_h == AB_HANDLE_INVALID) {
                shell_print(con_ch, "Error: calc.bin not found in TAR filesystem!\n");
                continue;
            }

            /* 2. Create a new isolated EL0 Process & Root VMAR for 'calc.bin' */
            ab_handle_t calc_proc = 0, calc_vmar = 0, calc_thr = 0;
            const char *cname = "calc.bin";
            sys_process_create(job_h, cname, &calc_proc, &calc_vmar);

            /* 3. Map the 'calc.bin' VMO into calc_vmar at VA 0x20000000 (offset 0x10000000) */
            vaddr_t entry_va = 0;
            uint64_t map_sz = (rep.file_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (map_sz == 0) map_sz = PAGE_SIZE;
            sys_vmar_map(calc_vmar, 0x10000000ULL, bin_vmo_h, 0, map_sz,
                        AB_VM_PERM_READ | AB_VM_PERM_WRITE | AB_VM_PERM_EXECUTE,
                        &entry_va);
            sys_handle_close(bin_vmo_h);
            sys_handle_close(calc_vmar);

            /* 4. Create initial thread and pass [dup_log, dup_con] over bootstrap Channel */
            sys_thread_create(calc_proc, cname, &calc_thr);

            ab_handle_t p_ch = 0, c_ch = 0, dup_log = 0, dup_con = 0;
            sys_channel_create(0, &p_ch, &c_ch);
            sys_handle_duplicate(log_h, AB_RIGHT_SAME_RIGHTS, &dup_log);
            sys_handle_duplicate(con_ch, AB_RIGHT_SAME_RIGHTS, &dup_con);

            ab_handle_t ch_handles[2] = { dup_log, dup_con };
            uint32_t ctag = 0x43414C43U;
            sys_channel_write(p_ch, &ctag, sizeof(ctag), ch_handles, 2);
            sys_handle_close(p_ch);

            /* 5. Start calc.bin at _start (0x20000000 in crt0.o) and wait for exit! */
            sys_process_start(calc_proc, calc_thr, entry_va, 0, c_ch, 0);
            sys_handle_close(calc_thr);

            sys_object_wait_one(calc_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
            sys_handle_close(calc_proc);
        } else if (u_strcmp(line, "exit") == 0) {
            shell_print(con_ch, "Shutting down ABOS user-space servers. Goodbye!\n");
            fs_msg_t fstop = { FS_OP_SHUTDOWN, 0, 0, "" };
            sys_channel_write(fs_ch, &fstop, sizeof(uint32_t), NULL, 0);

            console_msg_t cstop = { CON_OP_SHUTDOWN, "" };
            sys_channel_write(con_ch, &cstop, sizeof(uint32_t), NULL, 0);
            break;
        } else {
            shell_print(con_ch, "Unknown command. Type 'help' for available commands.\n");
        }
    }

    sys_process_exit(0);
}
