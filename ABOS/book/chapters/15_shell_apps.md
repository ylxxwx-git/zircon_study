# 15 Interactive Shells and Standalone User Applications

## 15.1 Main Functions

At the top of ABOS's software stack sit the unprivileged interactive shells (**`user/shell/shell.c`**) and on-disk user applications (**`user/apps/calc.c`**). Every kernel mechanism built across Chapters 6–12 and every runtime/server abstraction built across Chapters 13–14 converges cleanly here:

1. **Four Concurrent Hardware-Isolated Shells with Zero Hardware Authority**:
   At boot, `userboot` spawns 4 independent instances of `bootfs.tar:/shell.bin` (`abos-shell-0` .. `abos-shell-3`), each bound to its own Virtual Terminal (`tty0..tty3`). All 4 shells link at `0x20000000` (`user/link.ld`) and execute simultaneously across the 4 Cortex-A53 SMP cores in separate ASID-tagged `VmAspace`s. None of the shells holds a `Resource`, `Interrupt`, or physical MMIO capability: all terminal I/O flows over `con_ch` to `console-srv`, and all disk I/O flows over `fs_ch` to `fs-srv`.
2. **Zircon-Style Task, Handle & Virtual Memory Introspection (`ps`, `vmaps`, `caps`)**:
   Modeled directly after Fuchsia/Zircon's `system/uapp/psutils/ps.c`, `vmaps.c`, and `kernel/object/diagnostics.cpp`, `shell.c` provides built-in `ps`, `vmaps`, and `caps` commands. Together they allow live inspection of the `Job -> Process -> Thread` hierarchy, `PSS`/`PRIVATE`/`SHARED` memory consumption, per-object-type capability handle counts (`#jb #pr #th #vo #vm #ch #ev #po #ir #rs #lg`), hardware MMU ASIDs, and every `/A` -> `/R` -> `R` -> `M` virtual memory mapping across the entire system.
3. **Zero-Copy VMO File Reading & Deterministic VMAR Reclamation (`cat`)**:
   When a user runs `cat motd.txt` or `cat zircon.txt`, the shell sends `FS_OP_OPEN_VMO` to `fs-srv`, receives a VMO capability handle over `fs_ch`, maps it read-only (`AB_VM_PERM_READ`) into its own dynamic VMAR, prints its contents, and immediately unmaps (`sys_vmar_unmap`) and closes (`sys_handle_close`) the VMO so zero virtual address space or physical memory leaks across repeated invocations.
4. **Dynamic Disk Application Loading with Progressive Capability Attenuation (`calc.bin`)**:
   When a user runs `calc` on any Virtual Terminal, the shell fetches `calc.bin` as a VMO from `fs-srv`, creates a child `ProcessDispatcher` and `VMAR`, maps `calc.bin` at `0x20000000`, injects a minimal 2-handle `ab_proc_args_t` bundle (`[AB_PA_LOG, AB_PA_CONSOLE]` — withholding `fs_ch`, `job_h`, and even `calc_vmar`!), starts `calc.bin`, and waits on `AB_SIGNAL_TASK_TERMINATED`. Meanwhile, `calc.c` itself reads like clean textbook C (`printf`, `u_gets`, `return 0`), completely unaware that its standard I/O is traversing capability IPC channels to an `EL0` UART multiplexer!

### End-to-End Execution Lifecycle of `calc` (`disk.img:/calc.bin`)

```text
  abos-shell-i (EL0)                fs-srv (EL0)                 calc.bin (EL0 @ 0x20000000)
  ------------------                ------------                 ---------------------------
  1. FS_OP_OPEN_VMO("calc.bin") -->
                                    2. Create VMO from tar_cache,
                                       attenuate rights, send handle
                                <-- [rep.file_size + bin_vmo_h]
  3. sys_process_create(job_h)
  4. sys_vmar_map(calc_vmar, 0x20000000, bin_vmo_h)
     sys_handle_close(bin_vmo_h & calc_vmar)
  5. Pack [dup_log, dup_con] into boot_ch ("ABPA")
  6. sys_process_start(calc_proc, 0x20000000, c_ch) -----------> 7. crt0.S: zero .bss
  8. sys_object_wait_one(calc_proc,                              9. __ulibc_init: unpack LOG+CON
       AB_SIGNAL_TASK_TERMINATED)                               10. main(): printf/u_gets <-> console-srv
       (sleeps while calc owns tty_i;                           11. Compute a (+|-|*|/) b
        tty_j shells keep running!)                             12. return 0 -> sys_process_exit(0)
  13. Woken by TASK_TERMINATED! <-------------------------------    [Kernel closes handles, frees
      sys_handle_close(calc_proc),                                   VmAspace, signals TERMINATED]
      print next shell prompt!
```

### Summary of Files in Chapter 15

| Source File | Lines | Packaged Location | Startup Capabilities Held | Role in ABOS User Space |
| :--- | :---: | :--- | :--- | :--- |
| `user/shell/shell.c` | 443 | `bootfs.tar:/shell.bin` | `LOG`, `VMAR_ROOT`, `CONSOLE`, `FS`, `JOB_DEFAULT` (5) | Multi-instance interactive REPL with `ps`, `vmaps`, `caps`, `vt`, `ls`, `cat`, `calc`, `exit` |
| `user/apps/calc.c` | 70 | `disk.img:/calc.bin` | `LOG`, `CONSOLE` (2 — strict leaf sandbox!) | Interactive 64-bit calculator loaded on demand from VirtIO-Blk TAR storage |

---

## 15.2 user/shell/shell.c

### 15.2.1 Functional Description

`user/shell/shell.c` implements the interactive user-space command interpreter (`abos-shell`) running on each of ABOS's 4 Virtual Terminals (`tty0..tty3`). In 443 lines of pure `EL0` C code, it combines Zircon's `ps` and `vmaps` kernel-introspection tools, capability handle self-auditing (`caps`), Virtual Terminal switching (`vt <0..3>`), zero-copy VMO file inspection (`ls`, `cat <file>`), dynamic capability-sandboxed process creation (`calc`), and coordinated multi-server shutdown (`exit`).

### 15.2.2 Heavily Commented Source Code

#### Program 15-1: `lesson_20/user/shell/shell.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/shell/shell.c" start="1" end="443" total="443" -->
```c
       //// File header block: `user/shell/shell.c` implements ABOS's standalone `EL0`
       //// interactive command shell (`abos-shell`), linked at `0x20000000` via
       //// `user/link.ld` and instantiated **4 times concurrently** (`abos-shell-0` ..
       //// `abos-shell-3`) on Virtual Terminals `tty0..tty3`. Its built-in `ps` and
       //// `vmaps` utilities are modeled directly after Zircon's `psutils`
       //// (`system/uapp/psutils/ps.c`, `vmaps.c`) and `kernel/object/diagnostics.cpp`.
   1 | /* =========================================================================
   2 |  * user/shell/shell.c - EL0 Interactive Shell ('abos-shell')
   3 |  *
   4 |  * Lesson 20: Standalone BOOTFS interactive shell linked at VA 0x20000000.
   5 |  * Supports Zircon-style 'ps' and 'vmaps' commands for inspecting the
   6 |  * Job/Process/Thread hierarchy, per-process capability handle counts,
   7 |  * and Virtual Memory Address Space / VMAR / VMO mappings.
   8 |  *
   9 |  * References:
  10 |  *   - zircon_org/system/uapp/psutils/ps.c
  11 |  *   - zircon_org/system/uapp/psutils/vmaps.c
  12 |  *   - zircon_org/kernel/object/diagnostics.cpp
  13 |  * ========================================================================= */
  14 | #include "ulibc.h"
  15 | 
       //// `cmd_ps_help`: prints usage flags (`-T`, `-J`, `help`) and the column legend
       //// matching Zircon's `DumpProcessListKeyMap` (`#jb`, `#pr`, `#th`, `#vo`, `#vm`,
       //// `#ch`, `#ev`, `#po`, `#ir`, `#rs`, `#lg`).
  16 | static void cmd_ps_help(ab_handle_t con_ch) {
  17 |     shell_print(con_ch,
  18 |         "Usage: ps [-T|-J|help]\n"
  19 |         "  ps      - Show Job/Process/Thread tree + Zircon handle stats table\n"
  20 |         "  ps -T   - Include kernel/user threads in task tree\n"
  21 |         "  ps -J   - Only show Jobs in task tree\n"
  22 |         "  ps help - Show column legend (from Zircon diagnostics.cpp)\n\n"
  23 |         "Handle Table Columns (Zircon DumpProcessListKeyMap):\n"
  24 |         "  id  : process koid\n"
  25 |         "  #h  : total number of open handles in process\n"
  26 |         "  #jb : job handles           #pr : process handles\n"
  27 |         "  #th : thread handles        #vo : vmo handles\n"
  28 |         "  #vm : vmar handles          #ch : channel handles\n"
  29 |         "  #ev : event/eventpair       #po : port handles\n"
  30 |         "  #ir : interrupt handles     #rs : resource handles\n"
  31 |         "  #lg : debuglog handles\n");
  32 | }
  33 | 
       //// `cmd_ps`: queries the kernel's task and handle-table snapshot via
       //// `sys_job_get_ps_info(job_h, entries, AB_MAX_PS_ENTRIES, &count)`. Because
       //// visibility is capability-scoped to `job_h`, a shell placed inside a child job
       //// would only ever see tasks inside its own job subtree.
  34 | static void cmd_ps(ab_handle_t con_ch, ab_handle_t job_h, const char *args) {
  35 |     while (*args == ' ') args++;
  36 |     if (u_strcmp(args, "help") == 0 || u_strcmp(args, "--help") == 0) {
  37 |         cmd_ps_help(con_ch);
  38 |         return;
  39 |     }
  40 | 
  41 |     int only_jobs    = (u_strcmp(args, "-J") == 0);
  42 |     int show_threads = !only_jobs;
  43 | 
       //// Invoke `SYS_JOB_GET_PS_INFO` to populate up to `AB_MAX_PS_ENTRIES` (24)
       //// `ab_task_info_entry_t` records in pre-order tree traversal (`j:<koid>` root
       //// job at depth 0 -> `p:<koid>` processes at depth 1 -> `t:<koid>` threads at
       //// depth 2), then emit the Zircon `ps` header row.
  44 |     ab_task_info_entry_t entries[AB_MAX_PS_ENTRIES];
  45 |     uint32_t count = 0;
  46 |     ab_status_t st = sys_job_get_ps_info(job_h, entries, AB_MAX_PS_ENTRIES, &count);
  47 |     if (st != AB_OK) {
  48 |         con_printf(con_ch, "ps: sys_job_get_ps_info failed (status=%d)\n", st);
  49 |         return;
  50 |     }
  51 | 
  52 |     con_printf(con_ch, "%-14s %7s %7s %7s %-8s %s\n",
  53 |                "TASK", "PSS", "PRIVATE", "SHARED", "STATE", "NAME");
  54 | 
       //// Render Part 1 of `ps` — the indented Job/Process/Thread hierarchy and
       //// VmCounter memory columns (`PSS`, `PRIVATE`, `SHARED`):
       ////   - Indents `task_id` by two spaces per `e->depth` (`j:2`, `  p:6`, `
       ////     t:8`).
       ////   - Formats byte counts into compact human-readable strings (`24k`, `152k`,
       ////     `516k`) via `u_format_size()`.
       ////   - Notice the runtime contrast between `userboot` and standalone `BOOTFS`
       ////     processes: `userboot` reports `SHARED > 0` (embedded kernel
       ////     code/rodata), whereas `console-srv`, `fs-srv`, and `abos-shell-0..3`
       ////     report `SHARED = 0B` and private committed VMO + stack pages under
       ////     `PRIVATE`!
  55 |     for (uint32_t i = 0; i < count; i++) {
  56 |         ab_task_info_entry_t *e = &entries[i];
  57 |         if (only_jobs && e->type != 'j') continue;
  58 |         if (!show_threads && e->type == 't') continue;
  59 | 
  60 |         char task_id[24];
  61 |         if (e->depth == 0) {
  62 |             u_snprintf(task_id, sizeof(task_id), "%c:%lu", e->type, e->koid);
  63 |         } else if (e->depth == 1) {
  64 |             u_snprintf(task_id, sizeof(task_id), "  %c:%lu", e->type, e->koid);
  65 |         } else {
  66 |             u_snprintf(task_id, sizeof(task_id), "    %c:%lu", e->type, e->koid);
  67 |         }
  68 | 
  69 |         char pss_str[16]  = "";
  70 |         char priv_str[16] = "";
  71 |         char sh_str[16]   = "";
  72 | 
  73 |         if (e->type == 'j' || e->type == 'p') {
  74 |             u_format_size(pss_str, sizeof(pss_str), e->pss_bytes);
  75 |             u_format_size(priv_str, sizeof(priv_str), e->private_bytes);
  76 |         }
  77 |         if (e->type == 'p') {
  78 |             u_format_size(sh_str, sizeof(sh_str), e->shared_bytes);
  79 |         } else if (e->type == 't' && e->private_bytes > 0) {
  80 |             u_format_size(priv_str, sizeof(priv_str), e->private_bytes);
  81 |         }
  82 | 
  83 |         con_printf(con_ch, "%-14s %7s %7s %7s %-8s %s\n",
  84 |                    task_id, pss_str, priv_str, sh_str, e->state, e->name);
  85 |     }
  86 | 
       //// Render Part 2 of `ps` — Zircon's `DumpProcessList` per-process capability
       //// handle table:
       ////   - Sums `AB_OBJ_TYPE_EVENT` and `AB_OBJ_TYPE_EVENTPAIR` under `#ev`.
       ////   - Visually proves capability-based least privilege at runtime:
       ////       * `console-srv` and `fs-srv` show `#ir = 1`, `#rs = 1`, `#po = 1`,
       ////         `#ch = 4`, `#jb = 0`.
       ////       * `abos-shell-0..3` show `#ir = 0`, `#rs = 0`, `#po = 0`, `#ch = 2`,
       ////         `#jb = 1`!
  87 |     if (!only_jobs) {
  88 |         con_printf(con_ch,
  89 |             "\nZircon Kernel Process Handle Table (DumpProcessList):\n"
  90 |             "%7s  #h:  #jb #pr #th #vo #vm #ch #ev #po #ir #rs #lg [name]\n", "id");
  91 |         for (uint32_t i = 0; i < count; i++) {
  92 |             ab_task_info_entry_t *e = &entries[i];
  93 |             if (e->type != 'p') continue;
  94 |             uint32_t *hc = e->handle_counts;
  95 |             con_printf(con_ch,
  96 |                 "%7lu %3u:  %3u %3u %3u %3u %3u %3u %3u %3u %3u %3u %3u [%s]\n",
  97 |                 e->koid,
  98 |                 e->total_handles,
  99 |                 hc[AB_OBJ_TYPE_JOB],
 100 |                 hc[AB_OBJ_TYPE_PROCESS],
 101 |                 hc[AB_OBJ_TYPE_THREAD],
 102 |                 hc[AB_OBJ_TYPE_VMO],
 103 |                 hc[AB_OBJ_TYPE_VMAR],
 104 |                 hc[AB_OBJ_TYPE_CHANNEL],
 105 |                 hc[AB_OBJ_TYPE_EVENT] + hc[AB_OBJ_TYPE_EVENTPAIR],
 106 |                 hc[AB_OBJ_TYPE_PORT],
 107 |                 hc[AB_OBJ_TYPE_INTERRUPT],
 108 |                 hc[AB_OBJ_TYPE_RESOURCE],
 109 |                 hc[AB_OBJ_TYPE_LOG],
 110 |                 e->name);
 111 |         }
 112 |     }
 113 | }
 114 | 
       //// `cmd_vmaps_help`: documents the 4 hierarchical node prefixes borrowed from
       //// Zircon's `vmaps.c` (`/A` = `VmAspace`, `/R` = Root `VmAddressRegion`, `R` =
       //// child `vmar-dynamic` region, `M` = `VmMapping`).
 115 | static void cmd_vmaps_help(ab_handle_t con_ch) {
 116 |     shell_print(con_ch,
 117 |         "Usage: vmaps [<process-koid>|all|help]\n\n"
 118 |         "Dumps a process's virtual memory address space, VMARs, and VMO mappings\n"
 119 |         "(modeled after Zircon's system/uapp/psutils/vmaps.c & GetVmAspaceMaps).\n\n"
 120 |         "First column:\n"
 121 |         "  \"/A\" -- Process virtual address space (VmAspace)\n"
 122 |         "  \"/R\" -- Root VMAR (VmAddressRegion)\n"
 123 |         "  \"R\"  -- Child VMAR (R for Region)\n"
 124 |         "  \"M\"  -- VMO Mapping (VmMapping)\n\n"
 125 |         "Indentation indicates parent/child relationship in the VmAspace tree.\n");
 126 | }
 127 | 
       //// `print_one_process_vmaps`: invokes `sys_process_get_maps(job_h, proc_koid,
       //// ...)` (`proc_koid == 0` inspects the calling shell itself; a nonzero KOID
       //// inspects any live process inside `job_h`).
 128 | static void print_one_process_vmaps(ab_handle_t con_ch, ab_handle_t job_h, ab_koid_t proc_koid) {
 129 |     ab_info_maps_t maps[AB_MAX_VMAPS];
 130 |     uint32_t count = 0;
 131 |     ab_status_t st = sys_process_get_maps(job_h, proc_koid, maps, AB_MAX_VMAPS, &count);
 132 |     if (st != AB_OK) {
 133 |         con_printf(con_ch, "vmaps: process with koid %lu not found (status=%d)\n",
 134 |                    proc_koid, st);
 135 |         return;
 136 |     }
 137 | 
       //// Format each `ab_info_maps_t` record in the process's `VmAspace` hierarchy:
       ////   - Assigns `/A `, `/R `, ` R `, ` M `, or `  M` based on `e->type` and tree
       ////     `e->depth` (`0..3`).
       ////   - For `AB_INFO_MAPS_TYPE_MAPPING` nodes, decodes
       ////     `AB_VM_PERM_{READ,WRITE,EXECUTE}` into `rwx` permission characters,
       ////     formats both virtual span (`sz`) and physically committed RAM/MMIO (`res
       ////     = committed_pages * 4096`) via `u_format_size()`, and prints the backing
       ////     VMO's `koid` and diagnostic name (`'pl011-uart-mmio'`,
       ////     `'virtio-blk-dma'`, `'shell.bin'`, `'user-stack'`).
 138 |     for (uint32_t i = 0; i < count; i++) {
 139 |         ab_info_maps_t *e = &maps[i];
 140 |         const char *prefix = "   ";
 141 |         if (e->type == AB_INFO_MAPS_TYPE_ASPACE) {
 142 |             prefix = "/A ";
 143 |         } else if (e->type == AB_INFO_MAPS_TYPE_VMAR) {
 144 |             prefix = (e->depth <= 1) ? "/R " : " R ";
 145 |         } else if (e->type == AB_INFO_MAPS_TYPE_MAPPING) {
 146 |             prefix = (e->depth <= 2) ? " M " : "  M";
 147 |         }
 148 | 
 149 |         char sz_str[16];
 150 |         u_format_size(sz_str, sizeof(sz_str), e->size);
 151 |         vaddr_t end_va = e->base + e->size;
 152 | 
 153 |         if (e->type == AB_INFO_MAPS_TYPE_MAPPING) {
 154 |             char perms[4];
 155 |             perms[0] = (e->mapping.mmu_flags & AB_VM_PERM_READ)    ? 'r' : '-';
 156 |             perms[1] = (e->mapping.mmu_flags & AB_VM_PERM_WRITE)   ? 'w' : '-';
 157 |             perms[2] = (e->mapping.mmu_flags & AB_VM_PERM_EXECUTE) ? 'x' : '-';
 158 |             perms[3] = '\0';
 159 | 
 160 |             char res_str[16];
 161 |             u_format_size(res_str, sizeof(res_str), e->mapping.committed_pages * PAGE_SIZE);
 162 | 
 163 |             con_printf(con_ch,
 164 |                 "%s %016lx-%016lx %s %6s:sz %5s:res %5lu:vmo '%s'\n",
 165 |                 prefix, e->base, end_va, perms, sz_str, res_str,
 166 |                 e->mapping.vmo_koid, e->name);
 167 |         } else {
 168 |             con_printf(con_ch,
 169 |                 "%s %016lx-%016lx     %6s:sz                     '%s'\n",
 170 |                 prefix, e->base, end_va, sz_str, e->name);
 171 |         }
 172 |     }
 173 | }
 174 | 
       //// `cmd_vmaps`: argument dispatcher supporting four invocation modes:
       ////   1. `vmaps help` / `--help` / `-h` -> column & symbol legend.
       ////   2. `vmaps` (no arguments) -> dumps the calling shell's own `VmAspace`
       ////      (`koid = 0`).
       ////   3. `vmaps all` -> enumerates every active process (`entries[i].type ==
       ////      'p'`) via `sys_job_get_ps_info`, printing its KOID, hardware MMU ASID,
       ////      name, and full VMAR/VMO tree!
       ////   4. `vmaps <koid>` -> parses `<koid>` strictly via `u_atoi` and dumps that
       ////      single target process.
 175 | static void cmd_vmaps(ab_handle_t con_ch, ab_handle_t job_h, const char *args) {
 176 |     while (*args == ' ') args++;
 177 |     if (u_strcmp(args, "help") == 0 || u_strcmp(args, "--help") == 0 || u_strcmp(args, "-h") == 0) {
 178 |         cmd_vmaps_help(con_ch);
 179 |         return;
 180 |     }
 181 | 
 182 |     if (*args == '\0') {
 183 |         /* Default with no args: dump current process (0) and mention 'vmaps <koid>' / 'vmaps all' */
 184 |         print_one_process_vmaps(con_ch, job_h, 0);
 185 |         return;
 186 |     }
 187 | 
 188 |     if (u_strcmp(args, "all") == 0) {
 189 |         ab_task_info_entry_t entries[AB_MAX_PS_ENTRIES];
 190 |         uint32_t count = 0;
 191 |         if (sys_job_get_ps_info(job_h, entries, AB_MAX_PS_ENTRIES, &count) == AB_OK) {
 192 |             for (uint32_t i = 0; i < count; i++) {
 193 |                 if (entries[i].type == 'p') {
 194 |                     con_printf(con_ch, "--- Process [koid=%lu, asid=%u, name='%s'] ---\n",
 195 |                                entries[i].koid, entries[i].asid, entries[i].name);
 196 |                     print_one_process_vmaps(con_ch, job_h, entries[i].koid);
 197 |                 }
 198 |             }
 199 |         }
 200 |         return;
 201 |     }
 202 | 
 203 |     int ok = 0;
 204 |     int64_t koid_val = u_atoi(args, &ok);
 205 |     if (!ok || koid_val <= 0) {
 206 |         con_printf(con_ch, "vmaps: invalid koid '%s'. Usage: vmaps [<process-koid>|all|help]\n", args);
 207 |         return;
 208 |     }
 209 |     print_one_process_vmaps(con_ch, job_h, (ab_koid_t)koid_val);
 210 | }
 211 | 
       //// `main()`: entry point for each of the 4 concurrent shell processes!
       ////   - Retrieves `vt_id` (`0..3`) passed in `x1` by `userboot` via
       ////     `ulibc_get_startup_arg1()`.
       ////   - Unpacks its 5 capabilities from `__ulibc_init`: `[0]=log_h`,
       ////     `[1]=vmar_h`, `[2]=con_ch`, `[3]=fs_ch`, and `[4]=job_h`.
       ////   - Logs its `EL0` virtual address `(void *)main` (`0x20000xxx`) and `vt_id`
       ////     to the kernel debuglog.
 212 | int main(void) {
 213 |     uint32_t vt_id = (uint32_t)ulibc_get_startup_arg1();
 214 |     uint32_t act_b = 0, act_h = 0;
 215 | 
 216 |     ab_handle_t log_h  = ulibc_get_startup_handle(0);
 217 |     ab_handle_t vmar_h = ulibc_get_startup_handle(1);
 218 |     ab_handle_t con_ch = ulibc_get_startup_handle(2);
 219 |     ab_handle_t fs_ch  = ulibc_get_startup_handle(3);
 220 |     ab_handle_t job_h  = ulibc_get_startup_handle(4);
 221 | 
 222 |     char start_msg[96];
 223 |     u_snprintf(start_msg, sizeof(start_msg),
 224 |                "[abos-shell-%u EL0 @ %p] Bound to Virtual Terminal [tty%u]!",
 225 |                vt_id, (void *)main, vt_id);
 226 |     ulog(log_h, start_msg);
 227 | 
       //// Startup readiness barrier with `fs-srv`: sends a lightweight `FS_OP_LIST` ping
       //// on `fs_ch` and waits for the reply before printing the interactive VT banner.
       //// Because all 6 BOOTFS processes start concurrently across the 4 SMP Cortex-A53
       //// cores, this barrier guarantees `fs-srv` has finished its VirtIO-Blk DMA cache
       //// load (and emitted its boot debuglog line) before `tty0`'s prompt appears
       //// cleanly on screen.
 228 |     /* Wait for fs-srv to finish VirtIO-Blk DMA initialization before printing banner */
 229 |     {
 230 |         fs_msg_t ping = { FS_OP_LIST, 0, 0, "" };
 231 |         ab_signals_t obs = 0;
 232 |         sys_channel_write(fs_ch, &ping, sizeof(ping), NULL, 0);
 233 |         sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
 234 |         sys_channel_read(fs_ch, &ping, sizeof(ping), NULL, 0, &act_b, &act_h);
 235 |     }
 236 | 
       //// Print the per-VT welcome banner over `con_ch`. Note: for `tty0` (the initially
       //// active VT), `console-srv` writes the banner both to `vts[0].screen_buf` AND
       //// directly to the physical PL011 UART; for `tty1..tty3`, `console-srv` buffers
       //// their banners and initial prompts silently inside `vts[1..3].screen_buf`,
       //// ready for instant replay the moment the user switches VTs!
 237 |     con_printf(con_ch,
 238 |                "\n============================================================\n"
 239 |                "  ABOS Virtual Terminal [tty%u] (PID koid, EL0)\n"
 240 |                "  Switch: Alt+0..3 | Ctrl+2..5 | Ctrl+N (next) | vt 0..3\n"
 241 |                "============================================================\n",
 242 |                vt_id);
 243 | 
       //// Interactive Read-Eval-Print Loop (REPL): prints the VT-specific prompt, blocks
       //// in `shell_readline(con_ch, ...)` until the user submits a line on this VT,
       //// ignores empty lines, and dispatches built-in commands (`help`).
 244 |     char line[128];
 245 |     for (;;) {
 246 |         con_printf(con_ch, "abos[tty%u]$ ", vt_id);
 247 |         shell_readline(con_ch, line, sizeof(line));
 248 | 
 249 |         if (line[0] == '\0') {
 250 |             continue;
 251 |         } else if (u_strcmp(line, "help") == 0) {
 252 |             shell_print(con_ch,
 253 |                 "Available commands:\n"
 254 |                 "  help              - Show this command list\n"
 255 |                 "  vt <0..3>         - Switch to Virtual Terminal tty0..tty3 (Alt+0..3 / Ctrl+2..5 / Ctrl+N)\n"
 256 |                 "  ps [-T|-J|help]   - List Jobs, Processes, Threads & Handle stats (Zircon ps)\n"
 257 |                 "  vmaps [koid|all]  - Dump process VmAspace/VMAR/VMO mappings (Zircon vmaps)\n"
 258 |                 "  caps              - Inspect this shell's capability handle table\n"
 259 |                 "  ls                - List files in TAR disk.img (via EL0 fs-srv IPC)\n"
 260 |                 "  cat <file>        - Map read-only VMO of <file> from fs-srv & print\n"
 261 |                 "  calc              - Load & execute 'calc.bin' from TAR disk at 0x20000000\n"
 262 |                 "  exit              - Gracefully shut down all VTs, console-srv, and fs-srv\n");
       //// `vt <0..3>` command: programmatic Virtual Terminal switch! Sends a
       //// `CON_OP_SWITCH_VT` IPC message carrying target digit `'0'..'3'` to
       //// `console-srv` and waits for the 4-byte acknowledgment. Essential over basic
       //// serial pipelines or test harnesses where raw terminal hotkeys aren't used.
 263 |         } else if (u_strncmp(line, "vt ", 3) == 0) {
 264 |             const char *arg = line + 3;
 265 |             while (*arg == ' ') arg++;
 266 |             if (arg[0] >= '0' && arg[0] <= '3' && arg[1] == '\0') {
 267 |                 console_msg_t sw = { CON_OP_SWITCH_VT, "" };
 268 |                 sw.text[0] = arg[0];
 269 |                 sw.text[1] = '\0';
 270 |                 sys_channel_write(con_ch, &sw, sizeof(sw), NULL, 0);
 271 |                 ab_signals_t obs = 0;
 272 |                 sys_object_wait_one(con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
 273 |                 uint32_t ack = 0;
 274 |                 sys_channel_read(con_ch, &ack, sizeof(ack), NULL, 0, &act_b, &act_h);
 275 |             } else {
 276 |                 shell_print(con_ch, "Usage: vt <0|1|2|3>\n");
 277 |             }
       //// Dispatch `ps` and `vmaps` (with or without trailing argument strings).
 278 |         } else if (u_strcmp(line, "ps") == 0) {
 279 |             cmd_ps(con_ch, job_h, "");
 280 |         } else if (u_strncmp(line, "ps ", 3) == 0) {
 281 |             cmd_ps(con_ch, job_h, line + 3);
 282 |         } else if (u_strcmp(line, "vmaps") == 0) {
 283 |             cmd_vmaps(con_ch, job_h, "");
 284 |         } else if (u_strncmp(line, "vmaps ", 6) == 0) {
 285 |             cmd_vmaps(con_ch, job_h, line + 6);
       //// `caps` command: live capability self-audit! Calls `sys_object_get_info`
       //// (`SYS_OBJECT_GET_INFO`) on each of the shell's 5 handles (`log_h`, `vmar_h`,
       //// `con_ch`, `fs_ch`, `job_h`) to display their kernel `koid`, `ab_obj_type_t`,
       //// and `ab_rights_t` bitmask — confirming firsthand that the shell holds zero
       //// `Resource`, MMIO, or IRQ handles.
 286 |         } else if (u_strcmp(line, "caps") == 0) {
 287 |             ab_handle_basic_info_t info;
 288 |             con_printf(con_ch, "Shell Capability Table (inspected via sys_object_get_info):\n");
 289 |             sys_object_get_info(log_h, &info);
 290 |             con_printf(con_ch, "  [Handle 0x%x] Kernel DebugLog     (koid=%lu, type=%u, rights=0x%x)\n",
 291 |                        log_h, info.koid, info.type, info.rights);
 292 |             sys_object_get_info(vmar_h, &info);
 293 |             con_printf(con_ch, "  [Handle 0x%x] Root Address Space  (koid=%lu, type=%u, rights=0x%x)\n",
 294 |                        vmar_h, info.koid, info.type, info.rights);
 295 |             sys_object_get_info(con_ch, &info);
 296 |             con_printf(con_ch, "  [Handle 0x%x] Console IPC Channel (koid=%lu, type=%u, rights=0x%x)\n",
 297 |                        con_ch, info.koid, info.type, info.rights);
 298 |             sys_object_get_info(fs_ch, &info);
 299 |             con_printf(con_ch, "  [Handle 0x%x] Filesystem IPC Chan (koid=%lu, type=%u, rights=0x%x)\n",
 300 |                        fs_ch, info.koid, info.type, info.rights);
 301 |             sys_object_get_info(job_h, &info);
 302 |             con_printf(con_ch, "  [Handle 0x%x] Parent Job Cap      (koid=%lu, type=%u, rights=0x%x)\n",
 303 |                        job_h, info.koid, info.type, info.rights);
 304 |             con_printf(con_ch, "  [Note] Shell holds ZERO Resource/MMIO/IRQ handles (pure capability isolation!)\n");
       //// `ls` command: sends `FS_OP_LIST` over `fs_ch`, blocks on `AB_SIGNAL_READABLE`,
       //// reads the formatted directory listing from `fs-srv`, and prints it via
       //// `con_ch`.
 305 |         } else if (u_strcmp(line, "ls") == 0) {
 306 |             fs_msg_t req = { FS_OP_LIST, 0, 0, "" };
 307 |             sys_channel_write(fs_ch, &req, sizeof(req), NULL, 0);
 308 | 
 309 |             ab_signals_t obs = 0;
 310 |             sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
 311 |             fs_msg_t rep;
 312 |             sys_channel_read(fs_ch, &rep, sizeof(rep), NULL, 0, &act_b, &act_h);
 313 |             shell_print(con_ch, "Files in TAR disk.img (served by EL0 fs-srv):\n");
 314 |             shell_print(con_ch, rep.data);
       //// `cat <file>` command — Zero-Copy VMO File Reading + Clean VMAR Unmapping:
       ////   1. Sends `FS_OP_OPEN_VMO` with `fname` (e.g., `motd.txt` or `zircon.txt`)
       ////      over `fs_ch` and receives back **both** `rep.file_size` **and** a
       ////      transferred `file_vmo_h` capability handle (`act_h == 1`).
       ////   2. Maps `file_vmo_h` read-only (`AB_VM_PERM_READ`, `vmar_offset = 0`) into
       ////      the shell's own `vmar_h`, reading file bytes directly from RAM
       ////      (`fdata`) in 128-byte chunks with zero further filesystem IPC!
       ////   3. Calls `sys_vmar_unmap(vmar_h, file_va, map_sz)` followed by
       ////      `sys_handle_close(file_vmo_h)`. Unmapping is crucial: each process has
       ////      `MAX_VMAR_MAPPINGS = 16` slots, so tearing down transient `cat`
       ////      mappings reclaims both the virtual address range `[0x10000000..)` and
       ////      the backing VMO's physical pages immediately!
 315 |         } else if (u_strncmp(line, "cat ", 4) == 0) {
 316 |             const char *fname = line + 4;
 317 |             while (*fname == ' ') fname++;
 318 | 
 319 |             fs_msg_t req;
 320 |             req.op = FS_OP_OPEN_VMO;
 321 |             u_strcpy(req.data, fname);
 322 |             sys_channel_write(fs_ch, &req, sizeof(req), NULL, 0);
 323 | 
 324 |             ab_signals_t obs = 0;
 325 |             sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
 326 |             fs_msg_t rep;
 327 |             ab_handle_t file_vmo_h = AB_HANDLE_INVALID;
 328 |             sys_channel_read(fs_ch, &rep, sizeof(rep), &file_vmo_h, 1, &act_b, &act_h);
 329 | 
 330 |             if (rep.status == 0 && act_h == 1 && file_vmo_h != AB_HANDLE_INVALID) {
 331 |                 uint64_t map_sz = ((uint64_t)rep.file_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 332 |                 if (map_sz == 0) map_sz = PAGE_SIZE;
 333 |                 vaddr_t file_va = 0;
 334 |                 if (sys_vmar_map(vmar_h, 0, file_vmo_h, 0, map_sz,
 335 |                                  AB_VM_PERM_READ, &file_va) == AB_OK) {
 336 |                     const char *fdata = (const char *)file_va;
 337 |                     uint32_t rem = rep.file_size;
 338 |                     uint32_t pos = 0;
 339 |                     char chunk[129];
 340 |                     while (rem > 0) {
 341 |                         uint32_t n = (rem > 128U) ? 128U : rem;
 342 |                         memcpy(chunk, fdata + pos, n);
 343 |                         chunk[n] = '\0';
 344 |                         shell_print(con_ch, chunk);
 345 |                         pos += n;
 346 |                         rem -= n;
 347 |                     }
 348 |                     sys_vmar_unmap(vmar_h, file_va, map_sz);
 349 |                 } else {
 350 |                     shell_print(con_ch, "cat: sys_vmar_map failed\n");
 351 |                 }
 352 |                 sys_handle_close(file_vmo_h);
 353 |             } else {
 354 |                 shell_print(con_ch, "cat: file not found in TAR archive\n");
 355 |             }
       //// `calc` / `calc.bin` command — Dynamic 5-Step Capability Process Launch from
       //// TAR Disk (`disk.img`):
       ////   - **Step 1**: Request `calc.bin` via `FS_OP_OPEN_VMO` from `fs-srv`,
       ////     receiving a VMO capability `bin_vmo_h` containing the flat binary linked
       ////     with `user/link.ld` at `0x20000000`.
 356 |         } else if (u_strcmp(line, "calc") == 0 || u_strcmp(line, "calc.bin") == 0) {
 357 |             /* 1. Request 'calc.bin' executable VMO from EL0 FS Server over fs_ch! */
 358 |             fs_msg_t req;
 359 |             req.op = FS_OP_OPEN_VMO;
 360 |             u_strcpy(req.data, "calc.bin");
 361 |             sys_channel_write(fs_ch, &req, sizeof(req), NULL, 0);
 362 | 
 363 |             ab_signals_t obs = 0;
 364 |             sys_object_wait_one(fs_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
 365 |             fs_msg_t rep;
 366 |             ab_handle_t bin_vmo_h = AB_HANDLE_INVALID;
 367 |             sys_channel_read(fs_ch, &rep, sizeof(rep), &bin_vmo_h, 1, &act_b, &act_h);
 368 | 
 369 |             if (rep.status != 0 || act_h != 1 || bin_vmo_h == AB_HANDLE_INVALID) {
 370 |                 shell_print(con_ch, "Error: calc.bin not found in TAR filesystem!\n");
 371 |                 continue;
 372 |             }
 373 | 
       ////   - **Step 2**: Create a brand-new child `ProcessDispatcher` (`calc_proc`,
       ////     allocated its own hardware ASID and `VmAspace`) and root dynamic VMAR
       ////     (`calc_vmar`) under `job_h` (`sys_process_create`).
 374 |             /* 2. Create a new isolated EL0 Process & Root VMAR for 'calc.bin' */
 375 |             ab_handle_t calc_proc = 0, calc_vmar = 0, calc_thr = 0;
 376 |             const char *cname = "calc.bin";
 377 |             if (sys_process_create(job_h, cname, &calc_proc, &calc_vmar) != AB_OK) {
 378 |                 sys_handle_close(bin_vmo_h);
 379 |                 shell_print(con_ch, "Error: failed to create calc.bin process!\n");
 380 |                 continue;
 381 |             }
 382 | 
       ////   - **Step 3**: Map `bin_vmo_h` into `calc_vmar` at `vmar_offset =
       ////     0x10000000` (`VA 0x20000000`) with `READ | WRITE | EXECUTE` permissions,
       ////     then immediately close **both** `bin_vmo_h` and `calc_vmar` in the
       ////     shell! Notice that `calc.bin` never even receives a handle to its own
       ////     `calc_vmar` — its address space layout is frozen by the parent shell
       ////     before its first instruction executes!
 383 |             /* 3. Map the 'calc.bin' VMO into calc_vmar at VA 0x20000000 (offset 0x10000000) */
 384 |             vaddr_t entry_va = 0;
 385 |             uint64_t map_sz = ((uint64_t)rep.file_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 386 |             if (map_sz == 0) map_sz = PAGE_SIZE;
 387 |             ab_status_t mst = sys_vmar_map(calc_vmar, 0x10000000ULL, bin_vmo_h, 0, map_sz,
 388 |                                            AB_VM_PERM_READ | AB_VM_PERM_WRITE | AB_VM_PERM_EXECUTE,
 389 |                                            &entry_va);
 390 |             sys_handle_close(bin_vmo_h);
 391 |             sys_handle_close(calc_vmar);
 392 |             if (mst != AB_OK) {
 393 |                 sys_handle_close(calc_proc);
 394 |                 shell_print(con_ch, "Error: failed to map calc.bin VMO!\n");
 395 |                 continue;
 396 |             }
 397 | 
       ////   - **Step 4**: Create the initial thread `calc_thr` (`sys_thread_create`),
       ////     create a one-shot bootstrap channel (`p_ch`, `c_ch`), duplicate **only**
       ////     `log_h` and `con_ch`, and write an `ab_proc_args_t` bundle granting just
       ////     **2 capabilities**: `[AB_PA_LOG, AB_PA_CONSOLE]`.
       ////
       ////     Observe Progressive Capability Attenuation in action:
       ////       `userboot` (5 root caps, incl. `Resource`) ->
       ////       `abos-shell` (5 caps, no `Resource`, has `fs_ch` + `job_h` + `vmar_h`) ->
       ////       `calc.bin` (2 caps ONLY: `log` + `console` — cannot access files, cannot
       ////       spawn processes, cannot even map new memory!).
 398 |             /* 4. Create initial thread and pass [dup_log, dup_con] with AB_PA_* tags */
 399 |             if (sys_thread_create(calc_proc, cname, &calc_thr) != AB_OK) {
 400 |                 sys_handle_close(calc_proc);
 401 |                 shell_print(con_ch, "Error: failed to create calc.bin thread!\n");
 402 |                 continue;
 403 |             }
 404 | 
 405 |             ab_handle_t p_ch = 0, c_ch = 0, dup_log = 0, dup_con = 0;
 406 |             sys_channel_create(0, &p_ch, &c_ch);
 407 |             sys_handle_duplicate(log_h, AB_RIGHT_SAME_RIGHTS, &dup_log);
 408 |             sys_handle_duplicate(con_ch, AB_RIGHT_SAME_RIGHTS, &dup_con);
 409 | 
 410 |             ab_handle_t ch_handles[2] = { dup_log, dup_con };
 411 |             ab_proc_args_t pargs;
 412 |             memset(&pargs, 0, sizeof(pargs));
 413 |             pargs.magic           = AB_PROC_ARGS_MAGIC;
 414 |             pargs.handle_types[0] = AB_PA_LOG;
 415 |             pargs.handle_types[1] = AB_PA_CONSOLE;
 416 |             sys_channel_write(p_ch, &pargs, sizeof(pargs), ch_handles, 2);
 417 |             sys_handle_close(p_ch);
 418 | 
       ////   - **Step 5**: Start `calc.bin` at `entry_va` (`0x20000000`, `_start` in
       ////     `crt0.S`) with `c_ch` in `x0` (`sys_process_start`), close `calc_thr`,
       ////     and block foreground execution in `sys_object_wait_one(calc_proc,
       ////     AB_SIGNAL_TASK_TERMINATED, ...)` until `calc.bin` exits! Meanwhile, both
       ////     the shell and `calc.bin` share the VT's `con_ch` sequentially: while the
       ////     shell sleeps on `calc_proc`, `calc.bin` owns `CON_OP_WRITE` and
       ////     `CON_OP_READ_LINE` on that Virtual Terminal, while the other 3 VTs
       ////     continue running completely undisturbed!
 419 |             /* 5. Start calc.bin at _start (0x20000000 in crt0.o) and wait for exit! */
 420 |             if (sys_process_start(calc_proc, calc_thr, entry_va, 0, c_ch, 0) == AB_OK) {
 421 |                 sys_handle_close(calc_thr);
 422 |                 sys_object_wait_one(calc_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
 423 |             } else {
 424 |                 sys_handle_close(c_ch);
 425 |                 sys_handle_close(calc_thr);
 426 |                 shell_print(con_ch, "Error: failed to start calc.bin!\n");
 427 |             }
 428 |             sys_handle_close(calc_proc);
       //// `exit` command — Coordinated Graceful System Shutdown: sends `FS_OP_SHUTDOWN`
       //// to `fs-srv` and `CON_OP_SHUTDOWN` to `console-srv`, then breaks out of the
       //// REPL to return `0` (`sys_process_exit(0)`). As `console-srv` exits, its 4
       //// server channel endpoints close, asserting `AB_SIGNAL_PEER_CLOSED` on the other
       //// 3 shells blocked in `shell_readline()`, causing them to exit cleanly as well
       //// and waking `userboot` to complete system teardown!
 429 |         } else if (u_strcmp(line, "exit") == 0) {
 430 |             shell_print(con_ch, "Shutting down ABOS user-space servers. Goodbye!\n");
 431 |             fs_msg_t fstop = { FS_OP_SHUTDOWN, 0, 0, "" };
 432 |             sys_channel_write(fs_ch, &fstop, sizeof(uint32_t), NULL, 0);
 433 | 
 434 |             console_msg_t cstop = { CON_OP_SHUTDOWN, "" };
 435 |             sys_channel_write(con_ch, &cstop, sizeof(uint32_t), NULL, 0);
 436 |             break;
 437 |         } else {
 438 |             shell_print(con_ch, "Unknown command. Type 'help' for available commands.\n");
 439 |         }
 440 |     }
 441 | 
 442 |     return 0;
 443 | }
```

### 15.2.3 Technical Notes

1. **Verifying Microkernel Isolation Live with `ps` and `vmaps all`**:
   Running `ps` at the `tty0` prompt produces two tables modeled directly after Zircon's `ps` and `k zx ps`:
   - The **Task Tree** shows `j:2 root` at the top, followed by `p:` entries for `userboot`, `console-srv`, `fs-srv`, and `abos-shell-0..3` (and `calc.bin` if you switch to another VT with `Alt+1` while `calc` is awaiting input on `tty0`!), along with each process's child `t:` thread state (`running` vs `blocked`) and `PSS`/`PRIVATE`/`SHARED` memory footprint.
   - The **Process Handle Table** (`#jb #pr #th #vo #vm #ch #ev #po #ir #rs #lg`) provides an immediate security audit: only `console-srv` and `fs-srv` hold `#ir = 1` and `#rs = 1`, only `userboot` holds `#pr = 6` (the 6 child process handles it supervises), and every `abos-shell` holds `#rs = 0` and `#ir = 0`.
   Running `vmaps all` similarly walks every process's `VmAspace`, showing `console-srv`, `fs-srv`, and all 4 `abos-shell` processes each mapped at `0x0000000020000000` inside distinct hardware ASIDs, alongside `pl011-uart-mmio` in `console-srv` and `virtio-blk-mmio` + `virtio-blk-dma` in `fs-srv`!

2. **Why `cat` Calls Both `sys_vmar_unmap` and `sys_handle_close`**:
   In a VMO/VMAR memory architecture (unlike POSIX `read()`), mapping a file VMO inserts an `ab_vmar_mapping_t` holding an independent kernel reference (`ab_object_add_ref(&vmo->base)`) to the underlying `VmObjectDispatcher`. Simply calling `sys_handle_close(file_vmo_h)` would drop the handle from the shell's handle table *without* removing the page-table entries or releasing the VMAR's reference to the VMO! Calling `sys_vmar_unmap(vmar_h, file_va, map_sz)` tears down the `TTBR0_EL1` PTEs (flushing ASID-tagged TLB entries), frees the VMAR mapping slot, and releases the VMAR's VMO reference so `sys_handle_close` reclaims every physical page cleanly.

3. **Concurrent Multi-VT Execution During `calc`**:
   Because each `abos-shell-i` runs in its own preemptively scheduled thread and communicates over dedicated per-VT `con_cli_end[i]` and `fs_cli_end[i]` channels, launching `calc` on `tty0` blocks *only* `abos-shell-0` (on `sys_object_wait_one(calc_proc, AB_SIGNAL_TASK_TERMINATED, ...)`). Pressing `Alt+1` (or `Ctrl+1` / `Ctrl+N`) switches `console-srv` immediately to `tty1`, where `abos-shell-1` can run `ps` and `vmaps all` to inspect the live `calc.bin` process paused at `u_gets()` on `tty0`!

---

## 15.3 user/apps/calc.c

### 15.3.1 Functional Description

`user/apps/calc.c` is ABOS's standalone user-space application stored inside the VirtIO block disk archive (`image/disk.img:/calc.bin`) rather than in `kernel.img` or `bootfs.tar`. Linked with `crt0.o` and `libulibc.a` at `0x20000000` (`user/link.ld`), it demonstrates that with `ulibc`'s `crt0` and `ab_proc_args_t` runtime in place, writing a third-party `EL0` application on a capability microkernel is just as concise and natural as writing standard hosted C.

### 15.3.2 Heavily Commented Source Code

#### Program 15-2: `lesson_20/user/apps/calc.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/apps/calc.c" start="1" end="70" total="70" -->
```c
       //// File header block: `user/apps/calc.c` is a standalone `EL0` user application
       //// compiled into `image/calc.bin` (linked with `crt0.o` + `libulibc.a` via
       //// `user/link.ld` at `0x20000000`) and stored inside `image/disk.img` (served at
       //// runtime over VirtIO-Blk DMA by `fs-srv`).
   1 | /* =========================================================================
   2 |  * user/apps/calc.c - Standalone EL0 User-Space Calculator Application
   3 |  *
   4 |  * Linked with user/ulibc/crt0.o and user/ulibc/libulibc.a at VA 0x20000000.
   5 |  * Uses standard printf() and u_gets() from ulibc!
   6 |  * ========================================================================= */
   7 | #include "ulibc.h"
   8 | 
       //// `main()`: by the time execution reaches line 9, `crt0.S` has zeroed `.bss` and
       //// `__ulibc_init()` has unpacked `[AB_PA_LOG, AB_PA_CONSOLE]` from the shell's
       //// bootstrap channel and closed `boot_ch`.
       ////
       //// Notice how `dlog_printf()` writes to the kernel `debuglog` while `printf()`
       //// transparently sends `CON_OP_WRITE` IPC messages to `console-srv` on whichever
       //// Virtual Terminal (`tty0..tty3`) launched `calc`! Printing `(void *)main`
       //// confirms at runtime that `calc.bin` is executing in its own isolated
       //// `VmAspace` at `0x20000xxx` — the exact same virtual address simultaneously
       //// occupied by `console-srv`, `fs-srv`, and all 4 `abos-shell` processes!
   9 | int main(void) {
  10 |     dlog_printf("[calc.bin EL0 @ %p] crt0 initialized! Running main()...", (void *)main);
  11 | 
  12 |     printf("\n--- ABOS Calculator (loaded from disk.img:/calc.bin @ VA %p) ---\n", (void *)main);
  13 | 
       //// Declare stack buffers (allocated on `calc.bin`'s private 24 KB `EL0` stack
       //// `[0x00800000..0x00806000)`) and read the first signed 64-bit integer operand:
       ////   - `printf(...)` prompts the user on the active Virtual Terminal.
       ////   - `u_gets(...)` sends `CON_OP_READ_LINE` to `console-srv`, blocking
       ////     `calc.bin` until the user edits a line (with full Backspace support and
       ////     VT-switching support!) and presses Enter.
       ////   - `u_atoi(...)` validates that the input is a well-formed signed integer;
       ////     on invalid input, returning `-1` passes `-1` to `sys_process_exit` in
       ////     `crt0.S`.
  14 |     char num1_str[64];
  15 |     char op_str[32];
  16 |     char num2_str[64];
  17 | 
  18 |     printf("Enter first number: ");
  19 |     u_gets(num1_str, sizeof(num1_str));
  20 |     int ok1 = 0;
  21 |     int64_t a = u_atoi(num1_str, &ok1);
  22 |     if (!ok1) {
  23 |         printf("Error: '%s' is not a valid integer!\n\n", num1_str);
  24 |         return -1;
  25 |     }
  26 | 
       //// Prompt for and validate the arithmetic operator (`+`, `-`, `*`, or `/`),
       //// skipping any leading spaces or tabs.
  27 |     printf("Enter operation (+, -, *, /): ");
  28 |     u_gets(op_str, sizeof(op_str));
  29 |     char *op_p = op_str;
  30 |     while (*op_p == ' ' || *op_p == '\t') op_p++;
  31 |     char op = *op_p;
  32 |     if (op != '+' && op != '-' && op != '*' && op != '/') {
  33 |         printf("Error: unknown operation '%c'! Use +, -, *, or /.\n\n", op ? op : '?');
  34 |         return -1;
  35 |     }
  36 | 
       //// Prompt for and validate the second signed 64-bit integer operand `b`.
  37 |     printf("Enter second number: ");
  38 |     u_gets(num2_str, sizeof(num2_str));
  39 |     int ok2 = 0;
  40 |     int64_t b = u_atoi(num2_str, &ok2);
  41 |     if (!ok2) {
  42 |         printf("Error: '%s' is not a valid integer!\n\n", num2_str);
  43 |         return -1;
  44 |     }
  45 | 
       //// Explicit division-by-zero guard! On ARMv8-A AArch64, hardware integer divide
       //// instructions (`SDIV` and `UDIV`) **do not raise a synchronous exception** when
       //// dividing by zero — the architecture specifies that `SDIV Rd, Rn, xzr` silently
       //// writes `0` into `Rd`! Checking `b == 0` explicitly in software prevents
       //// returning a bogus `0` result (and also guards against compiler UB).
  46 |     if (op == '/' && b == 0) {
  47 |         printf("Error: division by zero!\n\n");
  48 |         return -1;
  49 |     }
  50 | 
       //// Compute the 64-bit signed result `res` (and remainder `rem = a % b` for
       //// integer division).
  51 |     int64_t res = 0;
  52 |     int64_t rem = 0;
  53 |     if (op == '+') res = a + b;
  54 |     else if (op == '-') res = a - b;
  55 |     else if (op == '*') res = a * b;
  56 |     else if (op == '/') {
  57 |         res = a / b;
  58 |         rem = a % b;
  59 |     }
  60 | 
       //// Format and display the computed equation both on the user's Virtual Terminal
       //// (`printf`) and in the kernel diagnostic log (`dlog_printf`), displaying the
       //// exact remainder whenever `a` is not evenly divisible by `b`. Finally, `return
       //// 0` hands control back to `crt0.S`, which invokes `sys_process_exit(0)`,
       //// destroying `calc.bin`'s address space and asserting
       //// `AB_SIGNAL_TASK_TERMINATED` to resume the waiting `abos-shell` prompt!
  61 |     if (op == '/' && rem != 0) {
  62 |         printf("Result: %ld %c %ld = %ld (remainder %ld)\n\n", a, op, b, res, rem);
  63 |         dlog_printf("Result: %ld %c %ld = %ld (remainder %ld)", a, op, b, res, rem);
  64 |     } else {
  65 |         printf("Result: %ld %c %ld = %ld\n\n", a, op, b, res);
  66 |         dlog_printf("Result: %ld %c %ld = %ld", a, op, b, res);
  67 |     }
  68 | 
  69 |     return 0;
  70 | }
```

### 15.3.3 Technical Notes

1. **Extreme Sandboxing (`2` Capabilities Total)**:
   When `calc.bin` runs `main()`, its kernel handle table contains **only 2 entries**: `[AB_PA_LOG, AB_PA_CONSOLE]` (since `__ulibc_init` already closed `boot_ch` prior to `main()`). Even if `calc.c` contained an arbitrary code-execution vulnerability, the compromised process holds:
   - **No `Resource` handle** -> cannot touch MMIO or interrupts.
   - **No `FS` channel handle** -> cannot list or read files on `disk.img`.
   - **No `Job` handle** -> cannot spawn processes or inspect sibling tasks (`ps`/`vmaps`).
   - **No `VMAR` handle** -> cannot even map additional executable or writable pages into its own address space!
   This is the textbook demonstration of object-capability security: sandboxing requires zero complex `seccomp-bpf` syscall filters or `chroot` jails because authority is simply never granted in the first place.

2. **AArch64 Hardware Integer Division (`SDIV`) Semantics**:
   Notice lines 46–49 (`if (op == '/' && b == 0)`): unlike x86-64's `IDIV` instruction (which raises hardware exception `#DE` / `SIGFPE` on division by zero or `INT64_MIN / -1`), the ARMv8-A `SDIV` instruction **never traps** (`INT64_MIN / -1` wraps to `INT64_MIN`, and `x / 0` yields `0`). Validating `b == 0` explicitly in `calc.c` ensures clean error reporting and non-zero exit status (`-1`) back to the parent shell.

---

## 15.4 Summary

In this final source-code chapter we traced ABOS's interactive userland (`user/shell/shell.c` and `user/apps/calc.c`) and saw every layer of the microkernel work in concert:

1. **Multi-Session Capability-Isolated Shells (`shell.c`)**: Four instances of `shell.bin` run concurrently at `0x20000000` across `tty0..tty3` with zero hardware privileges, communicating exclusively via capability `Channel`s with `console-srv` and `fs-srv`.
2. **Zircon-Compatible System Introspection (`ps`, `vmaps`, `caps`)**: `shell.c` queries `SYS_JOB_GET_PS_INFO`, `SYS_PROCESS_GET_MAPS`, and `SYS_OBJECT_GET_INFO` to render live task hierarchies, `PSS`/`PRIVATE`/`SHARED` memory statistics, per-type capability handle matrices, and 4-level `VmAspace`/`VMAR`/`VMO` mappings.
3. **Zero-Copy VMO I/O & Sandboxed App Execution (`cat` & `calc.c`)**: Files and executables on `disk.img` are delivered from `fs-srv` as rights-attenuated VMO handles — mapped read-only and cleanly unmapped by `cat`, or mapped into a fresh ASID-isolated process at `0x20000000` with a minimal 2-handle `[LOG, CONSOLE]` capability bundle to execute `calc.bin`.

Across **Chapters 13, 14, and 15**, we have commented 100% of every line of ABOS's 10 user-space files (`link.ld`, `crt0.S`, `ulibc.h`, `crt_env.c`, `ulibc.c`, `userboot.c`, `console_srv.c`, `fs_srv.c`, `shell.c`, and `calc.c`), completing the full stack from `EL1` reset vector `_start` all the way to interactive multi-terminal `EL0` applications!
