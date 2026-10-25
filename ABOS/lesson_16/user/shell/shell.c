/* =========================================================================
 * user/shell/shell.c - EL0 Interactive Shell ('abos-shell')
 * ========================================================================= */
#include "ulibc.h"

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

    ulog(log_h, "[abos-shell EL0] Started with VMAR + ConsoleChannel + FSChannel capabilities!");

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
                "  help       - Show this command list\n"
                "  caps       - Inspect this shell's capability handle table\n"
                "  ls         - List files in TAR disk.img (via EL0 fs-srv IPC)\n"
                "  cat <file> - Map read-only VMO of <file> from fs-srv & print\n"
                "  calc       - Load & execute 'calc.bin' from TAR disk at 0x20000000\n"
                "  exit       - Gracefully shut down console-srv, fs-srv, and shell\n");
        } else if (u_strcmp(line, "caps") == 0) {
            ab_handle_basic_info_t info;
            shell_print(con_ch, "Shell Capability Table (inspected via sys_object_get_info):\n");
            sys_object_get_info(log_h, &info);
            shell_print(con_ch, "  [Handle 0] Kernel DebugLog    (type=LOG,     rights=WRITE|DUPLICATE)\n");
            sys_object_get_info(vmar_h, &info);
            shell_print(con_ch, "  [Handle 1] Root Address Space (type=VMAR,    rights=MAP|READ|WRITE)\n");
            sys_object_get_info(con_ch, &info);
            shell_print(con_ch, "  [Handle 2] Console IPC Channel(type=CHANNEL, rights=READ|WRITE|WAIT)\n");
            sys_object_get_info(fs_ch, &info);
            shell_print(con_ch, "  [Handle 3] Filesystem IPC Chan(type=CHANNEL, rights=READ|WRITE|WAIT)\n");
            shell_print(con_ch, "  [Note] Shell holds ZERO Resource/MMIO/IRQ handles (pure capability isolation!)\n");
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

            /* 3. Map the 2-page 'calc.bin' VMO into calc_vmar at VA 0x20000000 (offset 0x10000000) */
            vaddr_t entry_va = 0;
            sys_vmar_map(calc_vmar, 0x10000000ULL, bin_vmo_h, 0, 2 * PAGE_SIZE,
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
