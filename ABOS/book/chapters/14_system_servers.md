# 14 Root Bootstrap Process and EL0 Device Driver Servers

## 14.1 Main Functions

In a traditional monolithic operating system (such as Linux or BSD), device drivers for the UART console, Virtual Terminal (`tty`) subsystem, VirtIO block storage, and archive/disk filesystems execute inside `EL1` kernel space with unrestricted access to all physical RAM and CPU control registers. Furthermore, the initial user-space process (`/sbin/init`) runs as `UID 0` (`root`) with ambient authority over the entire `/dev` and `/` filesystem hierarchy.

ABOS Lesson 20 demonstrates the exact opposite architecture — **true Zircon-style microkernel isolation and Principle of Least Privilege in `EL0`**:

1. **Minimal Kernel-Embedded Bootstrap (`userboot`)**: Only the tiny 214-line `userboot` service (`user/userboot/userboot.c`) is linked into `kernel.img`. All system servers (`console-srv.bin`, `fs-srv.bin`) and interactive shells (`shell.bin`) are compiled as standalone `EL0` binaries (linked at `0x20000000` via `user/link.ld`) and packaged into `image/bootfs.tar` (passed to QEMU `-initrd`).
2. **Explicit Capability Wiring Without Ambient Authority**: `lk_main` hands `userboot` the 5 root bootstrap handles (`root_job`, `root_log`, `root_res`, `ub_vmar`, `bootfs_vmo`). `userboot` maps `bootfs.tar` read-only in `EL0`, pre-creates 8 point-to-point `Channel` pairs, spawns `console-srv`, `fs-srv`, and `abos-shell-0..3`, and distributes **only the exact capabilities each component needs**:
   - `console-srv` and `fs-srv` receive `AB_PA_RESOURCE` (so they can map their respective MMIO frames, allocate DMA memory, and bind hardware IRQs) plus the server endpoints of the 4 VT channels — **but receive no `Job` capability**, so even a compromised driver server cannot spawn new processes!
   - `abos-shell-0..3` receive their respective client `con_ch` and `fs_ch` channels plus `root_job` (for `ps`, `vmaps`, and spawning `calc.bin`) — **but receive zero `Resource` capability**, making it architecturally impossible for any shell or user app to touch MMIO registers, physical RAM, or GIC interrupts!
3. **Immediate `bootfs.tar` Reclamation**: As soon as the 6 initial processes are running, `userboot` unmaps `bootfs.tar` (`sys_vmar_unmap`) and closes `bootfs_vmo_h` (`sys_handle_close`), freeing 100% of the RAM disk's physical pages back to the kernel's Physical Memory Manager (`pmm`).
4. **Pure `EL0` Interrupt- & Port-Driven System Servers**:
   - **`console-srv` (`user/drivers/console_srv.c`, 401 lines)** maps the ARM PL011 UART (`0x09000000`), binds GIC SPI 33, enables the 16-byte hardware FIFO + receive-timeout interrupts, multiplexes **4 independent Virtual Terminals (`tty0..tty3`)** with 512-byte RX rings, canonical line editing, and 1024-byte screen scrollback buffers, and uses a 25 ms `sys_port_wait` deadline to disambiguate `ESC`, `Alt+0..3`, and `modifyOtherKeys`/kitty CSI hotkeys.
   - **`fs-srv` (`user/drivers/fs_srv.c`, 304 lines)** scans all 32 VirtIO-MMIO slots (`0x0a000000`), allocates a 32-page (128 KB) physically contiguous DMA VMO, reads **232 TAR sectors (116 KB)** from `disk.img` in a single 3-descriptor chained Virtqueue DMA transfer, and serves files to all 4 shells concurrently via **zero-copy VMO capability transfer** (`FS_OP_OPEN_VMO`).

### User-Space Capability Topology Constructed by `userboot`

```text
                       +---------------------------------------+
                       |   EL1 Microkernel (lk_main)           |
                       |   Creates: root_job, root_log,        |
                       |            root_res, bootfs_vmo       |
                       +-------------------+-------------------+
                                           | boot_ch (5 handles)
                                           v
                       +---------------------------------------+
                       |   userboot (EL0 Root Bootstrap)       |
                       |   1. Maps bootfs.tar RO, parses USTAR |
                       |   2. Creates 8 IPC Channel pairs      |
                       |   3. Spawns 6 EL0 procs @ 0x20000000  |
                       |   4. Unmaps & frees bootfs.tar!       |
                       +-----+-------------+-------------+-----+
                             |             |             |
        +--------------------+             |             +--------------------+
        | [LOG, VMAR, RESOURCE,            | [LOG, VMAR,                      | [LOG, VMAR, RESOURCE,
        |  con_srv[0..3]]                  |  con_cli[i], fs_cli[i], JOB]     |  fs_srv[0..3]]
        | (NO JOB!)                        | (ZERO RESOURCE!)                 | (NO JOB!)
        v                                  v                                  v
+--------------------+          +---------------------+          +--------------------+
| console-srv (EL0)  | con[0..3]| abos-shell-0 (tty0) | fs[0..3] |   fs-srv (EL0)     |
| - PL011 @ 0x09000000|<=======>| abos-shell-1 (tty1) |<========>| - VirtIO @0x0a000000|
| - GIC SPI IRQ 33   |  IPC     | abos-shell-2 (tty2) |  VMO IPC | - GIC SPI 48+slot  |
| - 4 VTs + Port     |          | abos-shell-3 (tty3) |          | - 128KB Contig DMA |
+--------------------+          +---------------------+          +--------------------+
```

### Summary of System Server Files in Chapter 14

| Source File | Lines | Linked Image | Hardware Capabilities Held | Primary Role |
| :--- | :---: | :--- | :--- | :--- |
| `user/userboot/userboot.c` | 214 | `kernel.img` (embedded) | `Job`, `Log`, `Resource`, `VMAR`, `bootfs.tar` VMO | Parses `bootfs.tar`, wires IPC channels, spawns servers & shells, reclaims `bootfs.tar`, reaps tasks |
| `user/drivers/console_srv.c` | 401 | `bootfs.tar:/console-srv.bin` | `Resource` -> PL011 MMIO (`0x09000000`) + IRQ 33 | Multiplexes `tty0..tty3` + PL011 FIFO RX/TX + hotkey/CSI parser over `PortDispatcher` |
| `user/drivers/fs_srv.c` | 304 | `bootfs.tar:/fs-srv.bin` | `Resource` -> VirtIO MMIO (`0x0a000000`) + SPI + 128 KB DMA VMO | VirtIO-Blk DMA driver + USTAR cache serving zero-copy file VMOs to `tty0..tty3` over `PortDispatcher` |

---

## 14.2 user/userboot/userboot.c

### 14.2.1 Functional Description

`user/userboot/userboot.c` is the root `EL0` bootstrap process modeled directly after Zircon's `userboot`. When `lk_main` finishes initializing the SMP kernel, it packs `bootfs.tar` (loaded by QEMU `-initrd`) into a read-only `bootfs_vmo`, writes `[root_job_h, root_log_h, root_res_h, ub_vmar_h, bootfs_vmo_h]` into a bootstrap channel, and drops to `EL0` at `userboot_entry`. Without relying on any kernel filesystem or kernel ELF loader, `userboot` maps `bootfs_vmo` into its own address space, extracts `console-srv.bin`, `fs-srv.bin`, and `shell.bin` into private executable VMOs mapped at `0x20000000`, injects attenuated `ab_proc_args_t` capability bundles into each child, releases `bootfs.tar`, and waits for clean system shutdown.

### 14.2.2 Heavily Commented Source Code

#### Program 14-1: `lesson_20/user/userboot/userboot.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/userboot/userboot.c" start="1" end="214" total="214" -->
```c
       //// File header block: `userboot` is the *only* `EL0` code linked into
       //// `kernel.img` (modeled directly after Zircon's
       //// `zircon/kernel/lib/userabi/userboot`). Its sole job is to unpack the read-only
       //// `bootfs.tar` RAM disk VMO supplied by `lk_main`, launch the standalone `EL0`
       //// device driver servers (`console-srv.bin` and `fs-srv.bin`) and 4 interactive
       //// shells (`shell.bin`), wire their IPC capability channels together under the
       //// Principle of Least Privilege, reclaim `bootfs.tar` memory, and supervise clean
       //// shutdown.
   1 | /* =========================================================================
   2 |  * user/userboot/userboot.c - EL0 Root Bootstrap Service ('userboot')
   3 |  *
   4 |  * Lesson 20: True Zircon-style BOOTFS bootstrapping!
   5 |  *   - Only 'userboot' is embedded in the kernel image.
   6 |  *   - The kernel passes a read-only BOOTFS VMO handle containing the
   7 |  *     standalone user-space executables (console-srv.bin, fs-srv.bin,
   8 |  *     and shell.bin, all linked with user/link.ld at VA 0x20000000).
   9 |  *   - 'userboot' maps the BOOTFS VMO in EL0, extracts each executable into
  10 |  *     its own VMO, maps it at VA 0x20000000 in the target process's VMAR,
  11 |  *     passes typed AB_PA_* capabilities via ab_proc_args_t, and starts
  12 |  *     console-srv, fs-srv, and abos-shell-0..3.
  13 |  * ========================================================================= */
  14 | #include "ulibc.h"
  15 | 
       //// `parse_octal`: decodes NUL- or space-padded ASCII octal fields (digits
       //// `'0'..'7'`) from POSIX USTAR 512-byte archive headers.
  16 | static uint32_t parse_octal(const char *s, size_t n) {
  17 |     uint32_t v = 0;
  18 |     for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
  19 |         v = (v << 3) + (uint32_t)(s[i] - '0');
  20 |     }
  21 |     return v;
  22 | }
  23 | 
       //// `bootfs_find_file`: linear zero-copy lookup over the memory-mapped
       //// `bootfs.tar` VMO in `EL0`:
       ////   - Each USTAR entry begins with a 512-byte header (`hdr[0..99]` =
       ////     NUL-terminated filename, `hdr[124..135]` = 12-byte ASCII octal payload
       ////     size `fsz`).
       ////   - An archive terminates with two consecutive 512-byte zero blocks (`hdr[0]
       ////     == '\0'`).
       ////   - File data starts immediately at `bootfs + off + 512`, and the next
       ////     header begins at `off + 512 + round_up_512(fsz)` (`(fsz + 511U) &
       ////     ~511U`).
       ////   - Notice the bounds check `off + 512 + fsz <= bootfs_sz`, protecting
       ////     `userboot` against truncated or malformed `-initrd` images.
  24 | static const uint8_t *bootfs_find_file(const uint8_t *bootfs, size_t bootfs_sz,
  25 |                                        const char *name, uint32_t *out_size) {
  26 |     size_t off = 0;
  27 |     while (off + 512 <= bootfs_sz) {
  28 |         const char *hdr = (const char *)(bootfs + off);
  29 |         if (hdr[0] == '\0') break;
  30 |         uint32_t fsz = parse_octal(hdr + 124, 12);
  31 |         if (u_strcmp(hdr, name) == 0 && off + 512 + fsz <= bootfs_sz) {
  32 |             if (out_size) *out_size = fsz;
  33 |             return bootfs + off + 512;
  34 |         }
  35 |         off += 512 + ((fsz + 511U) & ~511U);
  36 |     }
  37 |     return NULL;
  38 | }
  39 | 
       //// `spawn_from_bootfs`: ABOS's capability-based process loader (replacing
       //// monolithic `fork()` + `execve()`):
       ////   - Locates `bin_name` inside the mapped `bootfs` image.
       ////   - Logs a clear diagnostic error to `root_log_h` and returns
       ////     `AB_HANDLE_INVALID` if the binary is missing or empty.
  40 | static ab_handle_t spawn_from_bootfs(ab_handle_t root_job_h, ab_handle_t log_h,
  41 |                                      const uint8_t *bootfs, size_t bootfs_sz,
  42 |                                      const char *bin_name, const char *proc_name,
  43 |                                      ab_handle_t *handles, const uint8_t *handle_types,
  44 |                                      uint32_t num_handles, uint64_t arg2) {
  45 |     uint32_t fsz = 0;
  46 |     const uint8_t *bin_data = bootfs_find_file(bootfs, bootfs_sz, bin_name, &fsz);
  47 |     if (!bin_data || fsz == 0) {
  48 |         char err[96];
  49 |         u_snprintf(err, sizeof(err), "[userboot] ERROR: '%s' not found in BOOTFS!", bin_name);
  50 |         ulog(log_h, err);
  51 |         return AB_HANDLE_INVALID;
  52 |     }
  53 | 
       //// Round the flat binary's byte size `fsz` up to a 4 KB `PAGE_SIZE` multiple,
       //// allocate a fresh anonymous `VmObjectDispatcher` (`sys_vmo_create`), tag its
       //// kernel diagnostic name (`sys_object_set_name(bin_vmo, bin_name)`) so `vmaps`
       //// displays `'console-srv.bin'`, `'fs-srv.bin'`, or `'shell.bin'`, and copy the
       //// flat binary image into the VMO (`sys_vmo_write`).
       ////
       //// Why create a fresh VMO per child instead of mapping `bootfs_vmo_h` directly?
       //// Because each standalone binary contains writable `.data` and `.bss` sections
       //// inside its single flat image, and 4 concurrent `abos-shell-0..3` instances
       //// must each own private `.data`/`.bss` pages rather than corrupting one another!
  54 |     uint64_t map_sz = ((uint64_t)fsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  55 |     if (map_sz == 0) map_sz = PAGE_SIZE;
  56 | 
  57 |     ab_handle_t bin_vmo = AB_HANDLE_INVALID;
  58 |     if (sys_vmo_create(map_sz, 0, &bin_vmo) != AB_OK) {
  59 |         ulog(log_h, "[userboot] ERROR: sys_vmo_create failed!");
  60 |         return AB_HANDLE_INVALID;
  61 |     }
  62 |     sys_object_set_name(bin_vmo, bin_name);
  63 |     sys_vmo_write(bin_vmo, bin_data, 0, fsz);
  64 | 
       //// Create a new isolated `ProcessDispatcher` (`proc_h` with its own hardware ASID
       //// and `TTBR0_EL1` page table) and its root dynamic `VmAddressRegionDispatcher`
       //// (`vmar_h`, spanning `[0x10000000 .. 0x40000000)`) under `root_job_h`, plus its
       //// initial `ThreadDispatcher` (`thr_h`).
  65 |     ab_handle_t proc_h = 0, vmar_h = 0, thr_h = 0;
  66 |     if (sys_process_create(root_job_h, proc_name, &proc_h, &vmar_h) != AB_OK) {
  67 |         sys_handle_close(bin_vmo);
  68 |         ulog(log_h, "[userboot] ERROR: sys_process_create failed!");
  69 |         return AB_HANDLE_INVALID;
  70 |     }
  71 |     sys_thread_create(proc_h, proc_name, &thr_h);
  72 | 
       //// Map `bin_vmo` at `vmar_offset = 0x10000000ULL` inside `vmar_h`. Since `vmar_h`
       //// has `base_va = 0x10000000`, this places `_start` at exact virtual address
       //// `0x20000000` (`entry_va`), matching `USER_APP_BASE` in `user/link.ld`!
       ////
       //// Immediately close `userboot`'s own `bin_vmo` handle
       //// (`sys_handle_close(bin_vmo)`): the child's VMAR mapping now holds a kernel
       //// reference (`ab_object_add_ref`) to the VMO, so `userboot` does not leak VMO
       //// handles in its 64-slot handle table.
  73 |     /* Map standalone executable VMO at 0x20000000 (offset 0x10000000 in Root VMAR) */
  74 |     vaddr_t entry_va = 0;
  75 |     ab_status_t st = sys_vmar_map(vmar_h, 0x10000000ULL, bin_vmo, 0, map_sz,
  76 |                                   AB_VM_PERM_READ | AB_VM_PERM_WRITE | AB_VM_PERM_EXECUTE,
  77 |                                   &entry_va);
  78 |     sys_handle_close(bin_vmo);
  79 |     if (st != AB_OK) {
  80 |         sys_handle_close(thr_h);
  81 |         sys_handle_close(vmar_h);
  82 |         sys_handle_close(proc_h);
  83 |         ulog(log_h, "[userboot] ERROR: sys_vmar_map failed!");
  84 |         return AB_HANDLE_INVALID;
  85 |     }
  86 | 
       //// Construct the one-shot bootstrap `Channel` (`p_ch`, `c_ch`) and pack the
       //// `ab_proc_args_t` (`magic = AB_PROC_ARGS_MAGIC = "ABPA"`) capability message:
       ////   - Slot `1` (`handles[1]`) is populated with the child's own `vmar_h`
       ////     (`AB_PA_VMAR_ROOT`) so the child can map MMIO, DMA, or file VMOs.
       ////   - `sys_channel_write(p_ch, ...)` atomically *moves* (`AB_RIGHT_TRANSFER`)
       ////     all `num_handles` capabilities (including `vmar_h`, duplicated
       ////     `log`/`res`/`job` handles, and pre-wired IPC channel endpoints) out of
       ////     `userboot`'s handle table and into the in-flight `MessagePacket`!
       ////   - `sys_handle_close(p_ch)` closes the parent write endpoint immediately.
  87 |     ab_handle_t p_ch = 0, c_ch = 0;
  88 |     sys_channel_create(0, &p_ch, &c_ch);
  89 | 
  90 |     handles[1] = vmar_h; /* Slot 1 is always the new process's Root VMAR handle */
  91 |     ab_proc_args_t pargs;
  92 |     memset(&pargs, 0, sizeof(pargs));
  93 |     pargs.magic = AB_PROC_ARGS_MAGIC;
  94 |     for (uint32_t i = 0; i < num_handles && i < 8U; i++) {
  95 |         pargs.handle_types[i] = handle_types[i];
  96 |     }
  97 |     sys_channel_write(p_ch, &pargs, sizeof(pargs), handles, num_handles);
  98 |     sys_handle_close(p_ch);
  99 | 
       //// Start the child process at `entry_va` (`0x20000000`, `_start` in `crt0.S`),
       //// passing `sp = 0` (instructing the kernel to allocate a fresh 24 KB `EL0` stack
       //// at `[0x00800000 .. 0x00806000)`), transferring `c_ch` into the child's `x0`,
       //// and passing `arg2` into the child's `x1`. Close `thr_h` and return `proc_h` so
       //// `userboot` can wait for `AB_SIGNAL_TASK_TERMINATED` during shutdown.
 100 |     if (sys_process_start(proc_h, thr_h, entry_va, 0, c_ch, arg2) != AB_OK) {
 101 |         ulog(log_h, "[userboot] ERROR: sys_process_start failed!");
 102 |     }
 103 |     sys_handle_close(thr_h);
 104 |     return proc_h;
 105 | }
 106 | 
       //// `userboot_entry(uint64_t arg0)`: raw `EL0` entry point jumped to directly by
       //// `lk_main` via `ab_process_start_user(..., USER_KVA_TO_UVA(userboot_entry),
       //// ...)`. Unlike standalone binaries at `0x20000000`, `userboot` executes from
       //// read/execute pages shared with the kernel image below `0x20000000`
       //// (`0x00081000+`) and keeps all working variables on its 24 KB `EL0` stack.
       ////
       //// Reads the 8-byte `bootfs_sz` payload and the 5 root capability handles
       //// (`[root_job_h, root_log_h, root_res_h, ub_vmar_h, bootfs_vmo_h]`) written by
       //// `lk_main`, then closes `boot_ch` immediately.
 107 | void userboot_entry(uint64_t arg0) {
 108 |     ab_handle_t boot_ch = (ab_handle_t)arg0;
 109 | 
 110 |     uint64_t bootfs_sz = 0;
 111 |     uint32_t act_b = 0, act_h = 0;
 112 |     ab_handle_t boot_h[5];
 113 |     if (sys_channel_read(boot_ch, &bootfs_sz, sizeof(bootfs_sz),
 114 |                          boot_h, 5, &act_b, &act_h) != AB_OK || act_h < 5) {
 115 |         sys_process_exit(-1);
 116 |     }
 117 |     sys_handle_close(boot_ch);
 118 | 
 119 |     ab_handle_t root_job_h   = boot_h[0];
 120 |     ab_handle_t root_log_h   = boot_h[1];
 121 |     ab_handle_t root_res_h   = boot_h[2];
 122 |     ab_handle_t ub_vmar_h    = boot_h[3];
 123 |     ab_handle_t bootfs_vmo_h = boot_h[4];
 124 | 
       //// Map `bootfs_vmo_h` read-only (`AB_VM_PERM_READ`, `vmar_offset = 0` ->
       //// `0x10000000`) into `userboot`'s own VMAR so `bootfs_find_file()` can scan
       //// `bootfs.tar` directly with ordinary `EL0` memory loads.
 125 |     /* Map the BOOTFS VMO read-only into userboot's own VMAR */
 126 |     uint64_t bootfs_map_sz = (bootfs_sz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 127 |     vaddr_t bootfs_va = 0;
 128 |     if (sys_vmar_map(ub_vmar_h, 0, bootfs_vmo_h, 0, bootfs_map_sz,
 129 |                      AB_VM_PERM_READ, &bootfs_va) != AB_OK) {
 130 |         ulog(root_log_h, "[userboot] PANIC: Failed to map BOOTFS VMO!");
 131 |         sys_process_exit(-1);
 132 |     }
 133 |     const uint8_t *bootfs = (const uint8_t *)bootfs_va;
 134 | 
 135 |     ulog(root_log_h, "[userboot] Mapped BOOTFS VMO! Loading console-srv.bin, fs-srv.bin, and shell.bin @ 0x20000000...");
 136 | 
       //// Capability wiring topology: create 8 bidirectional `Channel` pairs up front —
       //// 4 console channels (`con_srv_end[0..3]` <-> `con_cli_end[0..3]`) and 4
       //// filesystem channels (`fs_srv_end[0..3]` <-> `fs_cli_end[0..3]`).
       ////
       //// Notice that `srv_types` and `sh_types` are declared `static const`, placing
       //// them in `.rodata` (read-only) rather than `.data`/`.bss`, respecting
       //// `userboot`'s read-only image mapping!
 137 |     ab_handle_t con_srv_end[NUM_VTS], con_cli_end[NUM_VTS];
 138 |     ab_handle_t fs_srv_end[NUM_VTS],  fs_cli_end[NUM_VTS];
 139 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 140 |         sys_channel_create(0, &con_srv_end[i], &con_cli_end[i]);
 141 |         sys_channel_create(0, &fs_srv_end[i],  &fs_cli_end[i]);
 142 |     }
 143 | 
 144 |     static const uint8_t srv_types[3 + NUM_VTS] = {
 145 |         AB_PA_LOG, AB_PA_VMAR_ROOT, AB_PA_RESOURCE,
 146 |         AB_PA_CHANNEL_SRV, AB_PA_CHANNEL_SRV, AB_PA_CHANNEL_SRV, AB_PA_CHANNEL_SRV
 147 |     };
 148 |     static const uint8_t sh_types[5] = {
 149 |         AB_PA_LOG, AB_PA_VMAR_ROOT, AB_PA_CONSOLE, AB_PA_FS, AB_PA_JOB_DEFAULT
 150 |     };
 151 | 
       //// 1. Spawn `console-srv` from `console-srv.bin`: duplicate `root_log_h` and
       //// `root_res_h`, attach all 4 server-side console channels `con_srv_end[0..3]`,
       //// and grant `AB_PA_RESOURCE` (`res1`) so `console-srv` can map PL011 MMIO
       //// (`0x09000000`) and bind GIC IRQ 33. Note that `console-srv` receives **no**
       //// `root_job_h` (cannot spawn processes) and **no** `fs` channel!
 152 |     /* 1. Spawn standalone 'console-srv.bin' from BOOTFS */
 153 |     ab_handle_t log1 = 0, res1 = 0;
 154 |     sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log1);
 155 |     sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res1);
 156 |     ab_handle_t con_h[3 + NUM_VTS] = {
 157 |         log1, 0, res1,
 158 |         con_srv_end[0], con_srv_end[1], con_srv_end[2], con_srv_end[3]
 159 |     };
 160 |     ab_handle_t con_proc = spawn_from_bootfs(root_job_h, root_log_h, bootfs, (size_t)bootfs_sz,
 161 |                                              "console-srv.bin", "console-srv",
 162 |                                              con_h, srv_types, 3 + NUM_VTS, 0);
 163 | 
       //// 2. Spawn `fs-srv` from `fs-srv.bin`: duplicate `root_log_h` and `root_res_h`,
       //// attach all 4 server-side filesystem channels `fs_srv_end[0..3]`, and grant
       //// `AB_PA_RESOURCE` (`res2`) so `fs-srv` can map VirtIO-MMIO (`0x0a000000`),
       //// allocate 128 KB contiguous DMA memory, and bind the VirtIO-Blk GIC SPI.
 164 |     /* 2. Spawn standalone 'fs-srv.bin' from BOOTFS */
 165 |     ab_handle_t log2 = 0, res2 = 0;
 166 |     sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log2);
 167 |     sys_handle_duplicate(root_res_h, AB_RIGHT_SAME_RIGHTS, &res2);
 168 |     ab_handle_t fs_h[3 + NUM_VTS] = {
 169 |         log2, 0, res2,
 170 |         fs_srv_end[0], fs_srv_end[1], fs_srv_end[2], fs_srv_end[3]
 171 |     };
 172 |     ab_handle_t fs_proc = spawn_from_bootfs(root_job_h, root_log_h, bootfs, (size_t)bootfs_sz,
 173 |                                             "fs-srv.bin", "fs-srv",
 174 |                                             fs_h, srv_types, 3 + NUM_VTS, 0);
 175 | 
       //// 3. Spawn 4 Virtual Terminal interactive shells (`abos-shell-0` ..
       //// `abos-shell-3`) from the same `shell.bin` archive entry, passing `(uint64_t)i`
       //// (`0..3`) as `arg2` (`x1`). Crucially, inspect `sh_h[5]`: each shell receives
       //// `[log_s, vmar_h, con_cli_end[i], fs_cli_end[i], job_s]` — **zero `Resource`
       //// capability**! An interactive user shell therefore has zero direct access to
       //// physical RAM, device MMIO registers, or hardware interrupts.
 176 |     /* 3. Spawn 4 virtual terminal shells from standalone 'shell.bin' in BOOTFS */
 177 |     ab_handle_t sh_procs[NUM_VTS];
 178 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 179 |         char sh_name[24];
 180 |         u_snprintf(sh_name, sizeof(sh_name), "abos-shell-%u", i);
 181 | 
 182 |         ab_handle_t log_s = 0, job_s = 0;
 183 |         sys_handle_duplicate(root_log_h, AB_RIGHT_SAME_RIGHTS, &log_s);
 184 |         sys_handle_duplicate(root_job_h, AB_RIGHT_SAME_RIGHTS, &job_s);
 185 |         ab_handle_t sh_h[5] = { log_s, 0, con_cli_end[i], fs_cli_end[i], job_s };
 186 |         sh_procs[i] = spawn_from_bootfs(root_job_h, root_log_h, bootfs, (size_t)bootfs_sz,
 187 |                                         "shell.bin", sh_name,
 188 |                                         sh_h, sh_types, 5, (uint64_t)i);
 189 |     }
 190 | 
       //// Post-boot memory reclamation! As soon as all 6 user-space processes have been
       //// spawned, `userboot` unmaps `bootfs.tar` from its VMAR (`sys_vmar_unmap`) and
       //// closes both `bootfs_vmo_h` and `ub_vmar_h`. Because `lk_main` already freed
       //// the bootloader's raw `-initrd` pages and dropped its kernel pointer to
       //// `bootfs_vmo`, closing `bootfs_vmo_h` drops the VMO's reference count to zero,
       //// immediately returning every physical page of `bootfs.tar` to the PMM free
       //// pool!
 191 |     /* Unmap and release BOOTFS VMO once all initial processes are spawned */
 192 |     sys_vmar_unmap(ub_vmar_h, bootfs_va, bootfs_map_sz);
 193 |     sys_handle_close(bootfs_vmo_h);
 194 |     sys_handle_close(ub_vmar_h);
 195 | 
       //// Supervisory shutdown barrier: `userboot` sleeps in `sys_object_wait_one`
       //// (consuming 0% CPU) waiting for all 4 shells (`sh_procs[0..3]`), `fs-srv`, and
       //// `console-srv` to assert `AB_SIGNAL_TASK_TERMINATED`, closes their process
       //// handles, logs a clean exit banner, and terminates via `sys_process_exit(0)`
       //// (waking `lk_main` in `EL1`).
 196 |     ab_signals_t obs = 0;
 197 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 198 |         if (sh_procs[i] != AB_HANDLE_INVALID) {
 199 |             sys_object_wait_one(sh_procs[i], AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
 200 |             sys_handle_close(sh_procs[i]);
 201 |         }
 202 |     }
 203 |     if (fs_proc != AB_HANDLE_INVALID) {
 204 |         sys_object_wait_one(fs_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
 205 |         sys_handle_close(fs_proc);
 206 |     }
 207 |     if (con_proc != AB_HANDLE_INVALID) {
 208 |         sys_object_wait_one(con_proc, AB_SIGNAL_TASK_TERMINATED, AB_TIME_INFINITE, &obs);
 209 |         sys_handle_close(con_proc);
 210 |     }
 211 | 
 212 |     ulog(root_log_h, "[userboot] All 4 virtual terminals and user-space servers exited cleanly!");
 213 |     sys_process_exit(0);
 214 | }
```

### 14.2.3 Technical Notes

1. **Why `userboot` Has No `main()` or `.bss` Globals**: `userboot.o` is linked directly into `kernel.elf` alongside `ulibc.o`. In `user_thread_trampoline` (`kernel/core/object.c`), when `td->user_entry < 0x20000000`, the kernel maps `[ _text_start .. _rodata_end )` into `userboot`'s user address space as **read-only + executable** (`MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_EXECUTE`) and purposefully **never maps kernel `.data` or `.bss`** into `EL0`! Because `userboot.c` uses only stack-local variables and `static const` lookup arrays (`srv_types`, `sh_types` in `.rodata`), it executes cleanly in `EL0` with zero risk of touching kernel writable memory.
2. **Handle Ownership Transfer vs Duplication**: Notice how carefully `userboot` manages handle lifetimes:
   - Shared capabilities (`root_log_h`, `root_res_h`, `root_job_h`) are **duplicated** via `sys_handle_duplicate(..., AB_RIGHT_SAME_RIGHTS, ...)` before being placed into a child's `handles[]` array, because `sys_channel_write` **consumes** (moves) every handle passed to it.
   - Exclusive capabilities (`vmar_h`, `con_srv_end[i]`, `con_cli_end[i]`, `fs_srv_end[i]`, `fs_cli_end[i]`) are placed *directly* into `handles[]` without duplication, so `userboot` retains zero residual handles to any child's VMAR or IPC channels! You can verify this live by running `ps` inside ABOS: `userboot`'s `#ch` and `#vm` counts are `0`!

---

## 14.3 user/drivers/console_srv.c

### 14.3.1 Functional Description

`user/drivers/console_srv.c` implements ABOS's multi-session Virtual Terminal and PL011 UART driver server (`console-srv`) in `EL0`. Using its `AB_PA_RESOURCE` capability, it maps the physical PL011 MMIO page (`0x09000000`), binds GIC SPI 33 (`sys_interrupt_create`), and multiplexes hardware UART RX interrupts alongside 4 client IPC channels (`tty0..tty3`) on a single `PortDispatcher`. Each Virtual Terminal maintains an independent 512-byte circular RX ring, a 128-byte canonical line editor, and a 1024-byte virtual screen buffer, allowing instant hotkey switching (`Alt+0..3`, `Ctrl+0..3` via `modifyOtherKeys`/kitty CSI, `Ctrl+N`, `Ctrl+T 0..3`, or `vt 0..3`) even while background shells are waiting for input or writing output.

### 14.3.2 Heavily Commented Source Code

#### Program 14-2: `lesson_20/user/drivers/console_srv.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/drivers/console_srv.c" start="1" end="401" total="401" -->
```c
       //// File header block: `console-srv` (`user/drivers/console_srv.c`) replaces both
       //// the monolithic kernel UART character driver (`drivers/tty/serial/amba-pl011.c`
       //// in Linux) AND the kernel TTY line discipline + Virtual Terminal multiplexer
       //// (`drivers/tty/vt/vt.c` + `n_tty.c`) with a single 401-line unprivileged `EL0`
       //// process multiplexed over a Zircon `PortDispatcher`!
   1 | /* =========================================================================
   2 |  * drivers/console_srv.c - EL0 Multi-Terminal Console Server ('console-srv')
   3 |  *
   4 |  * Lesson 20: Standalone BOOTFS system server linked at VA 0x20000000.
   5 |  * Multiplexes 4 Virtual Terminals (tty0..tty3) and PL011 UART RX IRQ 33
   6 |  * using a Zircon PortDispatcher (sys_port_create + sys_object_wait_async).
   7 |  *
   8 |  * Supports switching between terminals 0..3 at any time via:
   9 |  *   - Ctrl+0 / Ctrl+1 / Ctrl+2 / Ctrl+3 (CSI modifyOtherKeys / kitty / NUL)
  10 |  *   - Alt+0 / Alt+1 / Alt+2 / Alt+3 (ESC + 0..3)
  11 |  *   - Ctrl+T followed by 0..3, or ^0 / ^1 / ^2 / ^3
  12 |  *   - Shell command: 'vt 0' .. 'vt 3'
  13 |  * ========================================================================= */
  14 | #include "ulibc.h"
  15 | 
       //// ARM PrimeCell PL011 UART hardware constants on QEMU `virt`:
       ////   - `PL011_PHYS_BASE = 0x09000000`: 4 KB MMIO register frame.
       ////   - `PL011_IRQ_NUM   = 33`: GICv2 SPI #1 (ID `32 + 1 = 33`).
       ////   - `UART_DR (0x000)`: Data Register (8-bit RX/TX FIFO port).
       ////   - `UART_FR (0x018)`: Flag Register (`RXFE` bit 4 = RX FIFO Empty; `TXFF`
       ////     bit 5 = TX FIFO Full).
       ////   - `UART_LCR_H (0x02C)`: Line Control Register (`FEN` bit 4 = enable
       ////     16-byte hardware RX/TX FIFOs; `WLEN8` bits 6:5 = `0b11` = 8 data bits, 1
       ////     stop bit).
       ////   - `UART_IMSC (0x038)`: Interrupt Mask Set/Clear (`RXIM` bit 4 = RX FIFO
       ////     half-full watermark interrupt; `RTIM` bit 6 = 32-bit-period RX timeout
       ////     interrupt for draining tail bytes fewer than the FIFO watermark!).
       ////   - `UART_ICR (0x044)`: Interrupt Clear Register (write-1-to-clear).
  16 | #define PL011_PHYS_BASE   0x09000000ULL
  17 | #define PL011_IRQ_NUM     33U
  18 | #define UART_DR           0x000
  19 | #define UART_FR           0x018
  20 | #define UART_LCR_H        0x02C
  21 | #define UART_IMSC         0x038
  22 | #define UART_ICR          0x044
  23 | #define UART_FR_RXFE      (1U << 4)
  24 | #define UART_FR_TXFF      (1U << 5)
  25 | #define UART_LCR_H_FEN    (1U << 4)
  26 | #define UART_LCR_H_WLEN8  (3U << 5)
  27 | #define UART_IMSC_RXIM    (1U << 4)
  28 | #define UART_IMSC_RTIM    (1U << 6)
  29 | 
       //// Multiplexer tuning constants:
       ////   - `KEY_UART_IRQ = 100`: Port packet key distinguishing hardware IRQ 33
       ////     notifications from client VT channel notifications (keys `0..3`).
       ////   - `VT_SCREEN_CAP = 1024`: per-VT rolling scrollback buffer replayed onto
       ////     the physical UART whenever the user switches active Virtual Terminals.
       ////   - `VT_LINE_CAP = 128`: canonical line-editing buffer (`\b`, `DEL`,
       ////     `\r`/`\n`).
       ////   - `VT_RX_RING_CAP = 512`: per-VT circular typeahead ring decoupling fast
       ////     serial paste bursts from shell `CON_OP_READ_LINE` requests.
       ////   - `ESC_TIMEOUT_NS = 25,000,000` (25 ms): monotonic deadline used to
       ////     disambiguate a bare `ESC` (`0x1B`) press from multi-byte `Alt+0..3`
       ////     (`ESC 0..3`) or ANSI CSI (`ESC [ ...`) sequences!
  30 | #define KEY_UART_IRQ      100ULL
  31 | #define VT_SCREEN_CAP     1024U
  32 | #define VT_LINE_CAP       128U
  33 | #define VT_RX_RING_CAP    512U
  34 | #define ESC_TIMEOUT_NS    25000000ULL /* 25ms standalone ESC timeout */
  35 | 
       //// `vt_state_t`: complete isolated state of one Virtual Terminal (`tty0..tty3`),
       //// holding its server-side `Channel` handle (`ch`), whether its shell is
       //// currently blocked in `CON_OP_READ_LINE` (`waiting_read`), `CR`->`LF` state
       //// (`last_was_cr`), 512-byte RX typeahead ring, 128-byte active input line, and
       //// 1024-byte virtual screen buffer.
  36 | typedef struct {
  37 |     ab_handle_t ch;
  38 |     int         waiting_read;
  39 |     int         last_was_cr;
  40 |     char        rx_ring[VT_RX_RING_CAP];
  41 |     uint16_t    rx_head;
  42 |     uint16_t    rx_tail;
  43 |     uint16_t    rx_count;
  44 |     char        line_buf[VT_LINE_CAP];
  45 |     size_t      line_len;
  46 |     char        screen_buf[VT_SCREEN_CAP];
  47 |     size_t      screen_len;
  48 | } vt_state_t;
  49 | 
       //// `rx_parser_t`: 4-state finite-state machine for decoding hotkeys and escape
       //// sequences across arbitrary UART interrupt boundaries:
       ////   - `prefix_mode == 0`: normal character ground state.
       ////   - `prefix_mode == 1`: saw `Ctrl+T` (`0x14`) or `'^'`; awaiting `'0'..'3'`.
       ////   - `prefix_mode == 2`: saw `ESC` (`0x1B`); armed `esc_deadline` (`now + 25
       ////     ms`), awaiting `'['` (CSI), `'0'..'3'` (`Alt+0..3`), or 25 ms timeout
       ////     (`tty3`).
       ////   - `prefix_mode == 3`: inside ANSI CSI (`ESC [`), accumulating parameter
       ////     bytes into `csi_buf[16]` until a final byte in `0x40..0x7E` arrives.
  50 | typedef struct {
  51 |     int       prefix_mode; /* 0=normal, 1=after Ctrl+T/caret, 2=after ESC, 3=in CSI */
  52 |     ab_time_t esc_deadline;
  53 |     char      csi_buf[16];
  54 |     size_t    csi_len;
  55 | } rx_parser_t;
  56 | 
       //// `mmio_r32` / `mmio_w32`: `volatile` 32-bit MMIO register accessors executing
       //// directly at `EL0` against the mapped `pl011-uart-mmio` physical VMO.
  57 | static inline uint32_t mmio_r32(vaddr_t base, uint32_t off) {
  58 |     return *(volatile uint32_t *)(base + off);
  59 | }
  60 | 
  61 | static inline void mmio_w32(vaddr_t base, uint32_t off, uint32_t val) {
  62 |     *(volatile uint32_t *)(base + off) = val;
  63 | }
  64 | 
       //// `el0_uart_putc` / `el0_uart_puts`: polled TX helpers with automatic `\n` ->
       //// `\r\n` CRLF translation, spinning on `UART_FR_TXFF` whenever the 16-byte TX
       //// FIFO is momentarily full.
  65 | static void el0_uart_putc(vaddr_t uart_va, char c) {
  66 |     if (c == '\n') {
  67 |         while (mmio_r32(uart_va, UART_FR) & UART_FR_TXFF) { }
  68 |         mmio_w32(uart_va, UART_DR, '\r');
  69 |     }
  70 |     while (mmio_r32(uart_va, UART_FR) & UART_FR_TXFF) { }
  71 |     mmio_w32(uart_va, UART_DR, (uint32_t)(unsigned char)c);
  72 | }
  73 | 
  74 | static void el0_uart_puts(vaddr_t uart_va, const char *s) {
  75 |     while (*s) el0_uart_putc(uart_va, *s++);
  76 | }
  77 | 
       //// `vt_append_screen`: appends output text into a VT's 1024-byte `screen_buf`
       //// whether that VT is currently visible or running in the background! Whenever
       //// `screen_buf` fills up, the oldest 25% (`drop = VT_SCREEN_CAP / 4 = 256` bytes)
       //// is evicted in one contiguous shift, amortizing scrollback compaction.
  78 | static void vt_append_screen(vt_state_t *vt, const char *text) {
  79 |     while (*text) {
  80 |         char c = *text++;
  81 |         if (vt->screen_len + 1 >= VT_SCREEN_CAP) {
  82 |             size_t drop = VT_SCREEN_CAP / 4;
  83 |             for (size_t i = drop; i < vt->screen_len; i++) {
  84 |                 vt->screen_buf[i - drop] = vt->screen_buf[i];
  85 |             }
  86 |             vt->screen_len -= drop;
  87 |         }
  88 |         vt->screen_buf[vt->screen_len++] = c;
  89 |     }
  90 |     vt->screen_buf[vt->screen_len] = '\0';
  91 | }
  92 | 
       //// `vt_push_rx` and `vt_pop_rx`: bounded 512-byte FIFO ring buffer operations. If
       //// a user pastes a multi-line script while a shell is busy running a command
       //// (such as `ps`, `vmaps all`, or `calc`), `drain_uart_rx` safely buffers up to
       //// 512 keystrokes in `rx_ring` without dropping characters.
  93 | static void vt_push_rx(vt_state_t *vt, char c) {
  94 |     if (vt->rx_count < VT_RX_RING_CAP) {
  95 |         vt->rx_ring[vt->rx_tail] = c;
  96 |         vt->rx_tail = (uint16_t)((vt->rx_tail + 1U) % VT_RX_RING_CAP);
  97 |         vt->rx_count++;
  98 |     }
  99 | }
 100 | 
 101 | static int vt_pop_rx(vt_state_t *vt, char *out_c) {
 102 |     if (vt->rx_count == 0) return 0;
 103 |     *out_c = vt->rx_ring[vt->rx_head];
 104 |     vt->rx_head = (uint16_t)((vt->rx_head + 1U) % VT_RX_RING_CAP);
 105 |     vt->rx_count--;
 106 |     return 1;
 107 | }
 108 | 
       //// `deliver_line_to_vt`: completes a pending `CON_OP_READ_LINE` RPC by copying
       //// the NUL-terminated `line_buf` into `console_msg_t`, resetting `line_len = 0`
       //// and `waiting_read = 0`, and writing the reply over `vt->ch` (waking the
       //// blocked shell or `calc.bin` process).
 109 | static void deliver_line_to_vt(vt_state_t *vt) {
 110 |     console_msg_t rep;
 111 |     rep.op = CON_OP_READ_LINE;
 112 |     vt->line_buf[vt->line_len] = '\0';
 113 |     u_strcpy(rep.text, vt->line_buf);
 114 |     vt->line_len     = 0;
 115 |     vt->line_buf[0]  = '\0';
 116 |     vt->waiting_read = 0;
 117 |     sys_channel_write(vt->ch, &rep, sizeof(rep), NULL, 0);
 118 | }
 119 | 
       //// `process_vt_rx`: canonical TTY line discipline (`n_tty` equivalent) in `EL0`!
       //// Drains buffered bytes from `vt->rx_ring` only while `vt->waiting_read` is
       //// true:
       ////   - Collapses `\r\n` pairs (`last_was_cr`) so Windows/telnet serial clients
       ////     that transmit `CRLF` on Enter do not trigger two empty prompts.
       ////   - On `'\r'` or `'\n'`: echoes newline (if `is_active`), commits the typed
       ////     command + `'\n'` into `screen_buf`, calls `deliver_line_to_vt(vt)`, and
       ////     `break`s so subsequent queued lines wait for the next
       ////     `CON_OP_READ_LINE`!
       ////   - On Backspace (`'\b'` or ASCII `127` `DEL`): decrements `line_len` and
       ////     emits `"\b \b"` (cursor left, overwrite with space, cursor left) to
       ////     visibly erase the character on the active terminal.
       ////   - On printable ASCII (`32..126`): appends to `line_buf` (up to 127 chars)
       ////     and echoes immediately if `is_active`.
 120 | static void process_vt_rx(vaddr_t uart_va, vt_state_t *vt, int is_active) {
 121 |     while (vt->waiting_read && vt->rx_count > 0) {
 122 |         char c = 0;
 123 |         vt_pop_rx(vt, &c);
 124 | 
 125 |         if (c == '\n' && vt->last_was_cr) {
 126 |             vt->last_was_cr = 0;
 127 |             continue;
 128 |         }
 129 |         vt->last_was_cr = (c == '\r');
 130 | 
 131 |         if (c == '\r' || c == '\n') {
 132 |             if (is_active) el0_uart_putc(uart_va, '\n');
 133 |             vt->line_buf[vt->line_len] = '\0';
 134 |             vt_append_screen(vt, vt->line_buf);
 135 |             vt_append_screen(vt, "\n");
 136 |             deliver_line_to_vt(vt);
 137 |             break;
 138 |         } else if (c == '\b' || c == 127) {
 139 |             if (vt->line_len > 0) {
 140 |                 vt->line_len--;
 141 |                 if (is_active) el0_uart_puts(uart_va, "\b \b");
 142 |             }
 143 |         } else if (c >= 32 && c < 127) {
 144 |             if (vt->line_len + 1 < VT_LINE_CAP) {
 145 |                 vt->line_buf[vt->line_len++] = c;
 146 |                 if (is_active) el0_uart_putc(uart_va, c);
 147 |             }
 148 |         }
 149 |     }
 150 | }
 151 | 
       //// `switch_active_vt`: switches physical UART focus to `tty0..tty3`
       //// (`target_vt`), prints a clear status banner showing the newly active terminal,
       //// replays `vts[target_vt].screen_buf` followed by any partially typed `line_buf`
       //// so the user sees the exact historical screen and in-progress command line of
       //// that session, and drains any pending RX bytes for `target_vt`.
 152 | static void switch_active_vt(vaddr_t uart_va, vt_state_t *vts,
 153 |                              uint32_t *active_vt, uint32_t target_vt) {
 154 |     if (target_vt >= NUM_VTS) return;
 155 |     *active_vt = target_vt;
 156 | 
 157 |     char hdr[256];
 158 |     u_snprintf(hdr, sizeof(hdr),
 159 |                "\n============================================================\n"
 160 |                " >>> Switched to Virtual Terminal [tty%u] (abos-shell-%u) <<<\n"
 161 |                " [Switch: Alt+0..3 | Ctrl+2..5 | Ctrl+N (next) | vt 0..3]\n"
 162 |                "============================================================\n",
 163 |                target_vt, target_vt);
 164 |     el0_uart_puts(uart_va, hdr);
 165 | 
 166 |     if (vts[target_vt].screen_len > 0) {
 167 |         el0_uart_puts(uart_va, vts[target_vt].screen_buf);
 168 |     }
 169 |     if (vts[target_vt].line_len > 0) {
 170 |         vts[target_vt].line_buf[vts[target_vt].line_len] = '\0';
 171 |         el0_uart_puts(uart_va, vts[target_vt].line_buf);
 172 |     }
 173 |     process_vt_rx(uart_va, &vts[target_vt], 1);
 174 | }
 175 | 
       //// `parse_csi_vt_hotkey`: decodes modern terminal emulator CSI key-chord
       //// encodings for `Ctrl+0`..`Ctrl+4` (ASCII `'0'` = 48 .. `'4'` = 52 with modifier
       //// `5` = Ctrl):
       ////   1. **Kitty keyboard protocol / xterm `modifyOtherKeys` format 2**: `ESC [
       ////      <keycode> ; 5 u` (e.g., `"48;5u"` -> keycode 48 -> `tty0`, `"51;5u"` ->
       ////      keycode 51 -> `tty3`).
       ////   2. **xterm `modifyOtherKeys` format 0**: `ESC [ 27 ; 5 ; <keycode> ~`
       ////      (e.g., `"27;5;49~"` -> keycode 49 -> `tty1`).
       ////   Returns the target VT index `0..3` (mapping `Ctrl+4` = 52 back to `tty0` as a
       ////   convenient alias when `Ctrl+0` is intercepted by host zoom shortcuts), or `-1`
       ////   for unrelated CSI sequences (such as arrow keys `ESC [ A`).
 176 | /* Parse CSI sequences for Ctrl+0..Ctrl+4:
 177 |  *   - "48;5u" .. "52;5u" (xterm modifyOtherKeys / kitty)
 178 |  *   - "27;5;48~" .. "27;5;52~" (xterm modifyOtherKeys format 0)
 179 |  */
 180 | static int parse_csi_vt_hotkey(const char *csi, size_t len) {
 181 |     if (len < 4) return -1;
 182 |     char final_ch = csi[len - 1];
 183 |     int keycode = -1;
 184 | 
 185 |     if (final_ch == 'u') {
 186 |         int val = 0;
 187 |         size_t i = 0;
 188 |         while (i < len && csi[i] >= '0' && csi[i] <= '9') {
 189 |             val = val * 10 + (csi[i] - '0');
 190 |             i++;
 191 |         }
 192 |         if (i > 0 && csi[i] == ';') keycode = val;
 193 |     } else if (final_ch == '~' && len >= 7 &&
 194 |                csi[0] == '2' && csi[1] == '7' && csi[2] == ';') {
 195 |         size_t i = 3;
 196 |         while (i < len && csi[i] != ';') i++;
 197 |         if (i < len && csi[i] == ';') {
 198 |             i++;
 199 |             int val = 0;
 200 |             size_t digits = 0;
 201 |             while (i < len && csi[i] >= '0' && csi[i] <= '9') {
 202 |                 val = val * 10 + (csi[i] - '0');
 203 |                 i++;
 204 |                 digits++;
 205 |             }
 206 |             if (digits > 0 && csi[i] == '~') keycode = val;
 207 |         }
 208 |     }
 209 | 
 210 |     if (keycode >= 48 && keycode <= 51) return keycode - 48; /* Ctrl+0..3 -> tty0..3 */
 211 |     if (keycode == 52) return 0;                             /* Ctrl+4    -> tty0    */
 212 |     return -1;
 213 | }
 214 | 
       //// Race-free PL011 FIFO interrupt drain loop (`drain_uart_rx`):
       ////   Notice the nested loop structure on lines 218–220:
       ////     `while (!(FR & RXFE)) { write ICR(RXIM|RTIM); while (!(FR & RXFE)) { ... } }`
       ////   Why clear `UART_ICR` *before* draining `UART_DR`, wrapped in an outer `RXFE`
       ////   re-check? Consider the opposite (buggy) order — draining until `RXFE == 1`
       ////   and *then* writing `UART_ICR`:
       ////     1. Inner loop sees `RXFE == 1` and exits.
       ////     2. Microseconds later, a new UART byte arrives, asserting `RXIS`/`RTIS`.
       ////     3. Driver blindly writes `UART_ICR`, **wiping out** the newly arrived
       ////        byte's interrupt condition before calling `sys_interrupt_ack` —
       ////        stranding that byte in the FIFO until yet another keystroke arrives!
       ////   By clearing `UART_ICR` *first*, draining `UART_DR` until `RXFE == 1`, and
       ////   re-checking `!(FR & RXFE)` in the outer loop, any byte arriving at any instant
       ////   either gets drained immediately in this call OR leaves `RXIS`/`RTIS` asserted
       ////   so GIC IRQ 33 fires again the moment `sys_interrupt_ack` unmasks it!
 215 | static void drain_uart_rx(vaddr_t uart_va, vt_state_t *vts,
 216 |                           uint32_t *active_vt, rx_parser_t *rx) {
 217 |     /* Clear ICR before draining so any byte arriving after RXFE==1 re-asserts IRQ 33 */
 218 |     while (!(mmio_r32(uart_va, UART_FR) & UART_FR_RXFE)) {
 219 |         mmio_w32(uart_va, UART_ICR, UART_IMSC_RXIM | UART_IMSC_RTIM);
 220 |         while (!(mmio_r32(uart_va, UART_FR) & UART_FR_RXFE)) {
 221 |             char c = (char)(mmio_r32(uart_va, UART_DR) & 0xFFU);
 222 | 
       //// Escape & prefix state machine transitions inside `drain_uart_rx`:
       ////   - `prefix_mode == 1` (`Ctrl+T` or `'^'`): `'0'..'3'` switches VTs; any
       ////     other byte falls through as ordinary input.
       ////   - `prefix_mode == 2` (`ESC` seen): `'['` enters CSI collection
       ////     (`prefix_mode = 3`); `'0'..'3'` handles `Alt+0..3` (`ESC` + digit) VT
       ////     switching!
       ////   - `prefix_mode == 3` (CSI): buffers parameter characters until an ECMA-48
       ////     final byte (`0x40..0x7E`) terminates the sequence and triggers
       ////     `parse_csi_vt_hotkey()`.
 223 |         if (rx->prefix_mode == 1) {
 224 |             rx->prefix_mode = 0;
 225 |             if (c >= '0' && c <= '3') {
 226 |                 switch_active_vt(uart_va, vts, active_vt, (uint32_t)(c - '0'));
 227 |                 continue;
 228 |             }
 229 |         } else if (rx->prefix_mode == 2) {
 230 |             if (c == '[') {
 231 |                 rx->prefix_mode = 3;
 232 |                 rx->csi_len = 0;
 233 |                 continue;
 234 |             }
 235 |             rx->prefix_mode = 0;
 236 |             if (c >= '0' && c <= '3') {
 237 |                 /* Alt+0 .. Alt+3 (ESC followed by 0..3) */
 238 |                 switch_active_vt(uart_va, vts, active_vt, (uint32_t)(c - '0'));
 239 |                 continue;
 240 |             }
 241 |         } else if (rx->prefix_mode == 3) {
 242 |             if (rx->csi_len + 1 < sizeof(rx->csi_buf)) {
 243 |                 rx->csi_buf[rx->csi_len++] = c;
 244 |                 rx->csi_buf[rx->csi_len]   = '\0';
 245 |             }
 246 |             if (c >= 0x40 && c <= 0x7E) {
 247 |                 rx->prefix_mode = 0;
 248 |                 int target = parse_csi_vt_hotkey(rx->csi_buf, rx->csi_len);
 249 |                 if (target >= 0) {
 250 |                     switch_active_vt(uart_va, vts, active_vt, (uint32_t)target);
 251 |                 }
 252 |             }
 253 |             continue;
 254 |         }
 255 | 
       //// Raw single-byte serial/VTE control-code hotkeys in ground state (`prefix_mode
       //// == 0`):
       ////   - `0x00` (`NUL`, generated by `Ctrl+2` or `Ctrl+Space` in standard
       ////     VT100/xterm) -> `tty2`.
       ////   - `0x1C` (`FS`, generated by `Ctrl+4` or `Ctrl+\`) -> `tty0`.
       ////   - `0x1D` (`GS`, generated by `Ctrl+5` or `Ctrl+]`) -> `tty1`.
       ////   - `0x0E` (`SO`, `Ctrl+N`) or `0x1E` (`RS`, `Ctrl+6`) -> cycle to next VT
       ////     `(active + 1) % 4`.
       ////   - `0x14` (`DC4`, `Ctrl+T`) or `'^'` -> arm 2-key prefix mode (`prefix_mode
       ////     = 1`).
       ////   - `0x1B` (`ESC`, also generated by `Ctrl+3` or `Ctrl+[` in classic
       ////     terminals) -> enter `prefix_mode = 2` and record `rx->esc_deadline = now
       ////     + 25 ms` using trapless `ulibc_clock_get_monotonic()`!
 256 |         /* Hotkey triggers over serial/VTE */
 257 |         if (c == 0x00) {
 258 |             switch_active_vt(uart_va, vts, active_vt, 2U);
 259 |             continue;
 260 |         } else if (c == 0x1C) {
 261 |             switch_active_vt(uart_va, vts, active_vt, 0U);
 262 |             continue;
 263 |         } else if (c == 0x1D) {
 264 |             switch_active_vt(uart_va, vts, active_vt, 1U);
 265 |             continue;
 266 |         } else if (c == 0x0E || c == 0x1E) {
 267 |             switch_active_vt(uart_va, vts, active_vt, (*active_vt + 1U) % NUM_VTS);
 268 |             continue;
 269 |         } else if (c == 0x14 || c == '^') {
 270 |             rx->prefix_mode = 1;
 271 |             continue;
 272 |         } else if (c == 0x1B) {
 273 |             rx->prefix_mode  = 2;
 274 |             rx->esc_deadline = ulibc_clock_get_monotonic() + ESC_TIMEOUT_NS;
 275 |             continue;
 276 |         }
 277 | 
       //// Ordinary character path: enqueue `c` into the currently active VT's 512-byte
       //// `rx_ring` and immediately run `process_vt_rx(..., 1)` so echoed keystrokes
       //// appear with sub-millisecond latency when `active_vt` is waiting for a line.
 278 |         vt_push_rx(&vts[*active_vt], c);
 279 |         process_vt_rx(uart_va, &vts[*active_vt], 1);
 280 |         }
 281 |     }
 282 | }
 283 | 
       //// `main()`: retrieves `console-srv`'s 7 startup capabilities unpacked by
       //// `__ulibc_init`: `[0]=log_h`, `[1]=vmar_h`, `[2]=res_h`, and
       //// `[3..6]=vts[0..3].ch`.
 284 | int main(void) {
 285 |     uint32_t act_b = 0, act_h = 0;
 286 | 
 287 |     ab_handle_t log_h  = ulibc_get_startup_handle(0);
 288 |     ab_handle_t vmar_h = ulibc_get_startup_handle(1);
 289 |     ab_handle_t res_h  = ulibc_get_startup_handle(2);
 290 | 
 291 |     vt_state_t vts[NUM_VTS];
 292 |     memset(vts, 0, sizeof(vts));
 293 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 294 |         vts[i].ch = ulibc_get_startup_handle(3 + i);
 295 |     }
 296 | 
       //// Hardware initialization in `EL0` using `res_h` (`AB_PA_RESOURCE`):
       ////   1. `sys_vmo_create_physical(res_h, 0x09000000, 4096, &uart_vmo_h)` wraps
       ////      the PL011 register page in a `is_physical = 1` VMO (tagged
       ////      `'pl011-uart-mmio'`).
       ////   2. `sys_vmar_map` maps it into `console-srv`'s VMAR with
       ////      `MMU_FLAG_CACHE_DEVICE` (`MAIR AttrIdx 0 = Device-nGnRnE`,
       ////      non-executable `UXN | PXN`).
       ////   3. `sys_interrupt_create(res_h, 33, &uart_irq_h)` binds GIC SPI 33 to an
       ////      `InterruptDispatcher` and unmasks IRQ 33 at the GIC Distributor.
       ////   4. Configures 8N1 + 16-byte hardware FIFOs (`UART_LCR_H_WLEN8 |
       ////      UART_LCR_H_FEN`), clears all 11 PL011 interrupt sources (`0x7FF`), and
       ////      unmasks both RX FIFO watermark (`RXIM`) and 32-bit RX timeout (`RTIM`)
       ////      interrupts.
 297 |     /* Map PL011 MMIO physical page (0x09000000) into EL0 and bind IRQ 33 */
 298 |     ab_handle_t uart_vmo_h = 0, uart_irq_h = 0;
 299 |     sys_vmo_create_physical(res_h, PL011_PHYS_BASE, PAGE_SIZE, &uart_vmo_h);
 300 |     sys_object_set_name(uart_vmo_h, "pl011-uart-mmio");
 301 | 
 302 |     vaddr_t uart_va = 0;
 303 |     sys_vmar_map(vmar_h, 0, uart_vmo_h, 0, PAGE_SIZE,
 304 |                  AB_VM_PERM_READ | AB_VM_PERM_WRITE, &uart_va);
 305 | 
 306 |     sys_interrupt_create(res_h, PL011_IRQ_NUM, &uart_irq_h);
 307 |     mmio_w32(uart_va, UART_LCR_H, UART_LCR_H_WLEN8 | UART_LCR_H_FEN);
 308 |     mmio_w32(uart_va, UART_ICR, 0x7FFU);
 309 |     mmio_w32(uart_va, UART_IMSC, UART_IMSC_RXIM | UART_IMSC_RTIM);
 310 | 
       //// Emit `ESC [ > 4 ; 2 m` (enable xterm `modifyOtherKeys` mode 2) and `ESC [ > 1
       //// u` (push kitty progressive keyboard protocol flags = 1) so modern host
       //// terminals report unambiguous `Ctrl+0..3` CSI sequences over `-serial
       //// mon:stdio`.
 311 |     /* Enable xterm modifyOtherKeys (mode 2) + kitty keyboard protocol */
 312 |     el0_uart_puts(uart_va, "\x1b[>4;2m\x1b[>1u");
 313 | 
 314 |     ulog(log_h, "[console-srv EL0] Multiplexing 4 Virtual Terminals (tty0..tty3) + UART IRQ 33 on Port!");
 315 | 
       //// Create a single `PortDispatcher` (`port_h`) and register **5 asynchronous wait
       //// observers** on it:
       ////   - `uart_irq_h` with `key = KEY_UART_IRQ (100)` watching
       ////     `AB_SIGNAL_SIGNALED`.
       ////   - `vts[0..3].ch` with `key = 0..3` watching `AB_SIGNAL_READABLE |
       ////     AB_SIGNAL_PEER_CLOSED`.
 316 |     ab_handle_t port_h = AB_HANDLE_INVALID;
 317 |     sys_port_create(0, &port_h);
 318 | 
 319 |     sys_object_wait_async(uart_irq_h, port_h, KEY_UART_IRQ, AB_SIGNAL_SIGNALED);
 320 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 321 |         sys_object_wait_async(vts[i].ch, port_h, (uint64_t)i,
 322 |                               AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
 323 |     }
 324 | 
 325 |     uint32_t active_vt = 0;
 326 |     rx_parser_t rx;
 327 |     memset(&rx, 0, sizeof(rx));
 328 |     int running = 1;
 329 | 
       //// Unified event-driven server loop (`while (running)`):
       ////   - If `rx.prefix_mode == 2` (an `ESC` byte is awaiting follow-up bytes),
       ////     pass `rx.esc_deadline` to `sys_port_wait`; otherwise sleep indefinitely
       ////     (`AB_TIME_INFINITE`).
       ////   - If 25 ms elapse with neither a UART interrupt nor an IPC packet (`wst ==
       ////     AB_ERR_TIMED_OUT`), the `0x1B` byte was a standalone `ESC` / `Ctrl+3`
       ////     keypress — reset `prefix_mode = 0` and switch cleanly to `tty3`!
 330 |     while (running) {
 331 |         ab_port_packet_t pkt;
 332 |         ab_time_t deadline = (rx.prefix_mode == 2) ? rx.esc_deadline : AB_TIME_INFINITE;
 333 |         ab_status_t wst = sys_port_wait(port_h, deadline, &pkt);
 334 |         if (wst == AB_ERR_TIMED_OUT) {
 335 |             if (rx.prefix_mode == 2) {
 336 |                 /* Standalone 0x1B (Ctrl+3 / ESC) -> switch to tty3 */
 337 |                 rx.prefix_mode = 0;
 338 |                 switch_active_vt(uart_va, vts, &active_vt, 3U);
 339 |             }
 340 |             continue;
 341 |         }
 342 |         if (wst != AB_OK) {
 343 |             continue;
 344 |         }
 345 | 
       //// Hardware IRQ 33 packet handler (`pkt.key == KEY_UART_IRQ`):
       ////   1. Drain all bytes from the PL011 RX FIFO (`drain_uart_rx`).
       ////   2. Call `sys_interrupt_ack(uart_irq_h)` to clear `AB_SIGNAL_SIGNALED` and
       ////      re-enable IRQ 33 at the GIC Distributor (which the kernel masked upon
       ////      taking the hardware interrupt).
       ////   3. Re-register the one-shot `sys_object_wait_async` observer on
       ////      `uart_irq_h`.
 346 |         if (pkt.key == KEY_UART_IRQ) {
 347 |             drain_uart_rx(uart_va, vts, &active_vt, &rx);
 348 |             sys_interrupt_ack(uart_irq_h);
 349 |             sys_object_wait_async(uart_irq_h, port_h, KEY_UART_IRQ, AB_SIGNAL_SIGNALED);
 350 |             continue;
 351 |         }
 352 | 
       //// Client VT channel packet handler (`pkt.key` in `0..3`): drain *all* queued
       //// `console_msg_t` requests on `vt->ch` until `sys_channel_read` returns
       //// `AB_ERR_SHOULD_WAIT`:
       ////   - `CON_OP_WRITE`: append text to `vt`'s background/foreground
       ////     `screen_buf`, emit to physical UART *only* if `vt_idx == active_vt`, and
       ////     write 4-byte ACK.
       ////   - `CON_OP_READ_LINE`: mark `vt->waiting_read = 1`, opportunistically drain
       ////     UART RX if active, and run `process_vt_rx` (immediately satisfying the
       ////     read if a full line was already sitting in `rx_ring`!).
       ////   - `CON_OP_SWITCH_VT`: acknowledge the `vt <0..3>` shell command and switch
       ////     active terminal focus via `switch_active_vt()`.
       ////   - `CON_OP_SHUTDOWN`: set `running = 0` to exit the server loop cleanly.
 353 |         uint32_t vt_idx = (uint32_t)pkt.key;
 354 |         if (vt_idx >= NUM_VTS || vts[vt_idx].ch == AB_HANDLE_INVALID) {
 355 |             continue;
 356 |         }
 357 | 
 358 |         vt_state_t *vt = &vts[vt_idx];
 359 |         console_msg_t req;
 360 |         while (sys_channel_read(vt->ch, &req, sizeof(req), NULL, 0, &act_b, &act_h) == AB_OK) {
 361 |             if (req.op == CON_OP_WRITE) {
 362 |                 vt_append_screen(vt, req.text);
 363 |                 if (vt_idx == active_vt) {
 364 |                     el0_uart_puts(uart_va, req.text);
 365 |                 }
 366 |                 console_msg_t ack = { CON_OP_WRITE, "" };
 367 |                 sys_channel_write(vt->ch, &ack, sizeof(uint32_t), NULL, 0);
 368 |             } else if (req.op == CON_OP_READ_LINE) {
 369 |                 vt->waiting_read = 1;
 370 |                 if (vt_idx == active_vt) {
 371 |                     drain_uart_rx(uart_va, vts, &active_vt, &rx);
 372 |                 }
 373 |                 process_vt_rx(uart_va, vt, vt_idx == active_vt);
 374 |             } else if (req.op == CON_OP_SWITCH_VT) {
 375 |                 uint32_t target = (uint32_t)(req.text[0] - '0');
 376 |                 console_msg_t ack = { CON_OP_SWITCH_VT, "" };
 377 |                 sys_channel_write(vt->ch, &ack, sizeof(uint32_t), NULL, 0);
 378 |                 if (target < NUM_VTS) {
 379 |                     switch_active_vt(uart_va, vts, &active_vt, target);
 380 |                     drain_uart_rx(uart_va, vts, &active_vt, &rx);
 381 |                 }
 382 |             } else if (req.op == CON_OP_SHUTDOWN) {
 383 |                 running = 0;
 384 |                 break;
 385 |             }
 386 |         }
 387 | 
 388 |         if (!running) break;
 389 | 
       //// Channel lifecycle maintenance: if the shell closed its endpoint (`pkt.observed
       //// & AB_SIGNAL_PEER_CLOSED`), close `vt->ch` and mark it invalid; otherwise
       //// re-arm the one-shot `sys_object_wait_async` observer on `vt->ch`.
 390 |         if (pkt.observed & AB_SIGNAL_PEER_CLOSED) {
 391 |             sys_handle_close(vt->ch);
 392 |             vt->ch = AB_HANDLE_INVALID;
 393 |         } else {
 394 |             sys_object_wait_async(vt->ch, port_h, (uint64_t)vt_idx,
 395 |                                   AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
 396 |         }
 397 |     }
 398 | 
       //// Clean terminal teardown before `console-srv` exits (`return 0` -> `crt0.S` ->
       //// `sys_process_exit(0)`): emit `ESC [ < u` (pop kitty keyboard mode) and `ESC [
       //// > 4 ; 0 m` (disable xterm `modifyOtherKeys`) so the developer's host shell
       //// prompt is left in pristine standard keyboard state!
 399 |     el0_uart_puts(uart_va, "\x1b[<u\x1b[>4;0m");
 400 |     return 0;
 401 | }
```

### 14.3.3 Technical Notes

1. **Why PL011 Hardware FIFOs Require `UART_IMSC_RXIM | UART_IMSC_RTIM`**:
   When bit 4 (`UART_LCR_H_FEN`) of `UART_LCR_H` is cleared, the PL011 operates in 1-byte holding-register mode — any 2-byte burst (such as an `Alt+1` escape pair `0x1B 0x31` or a 6-byte CSI sequence `ESC [ 4 9 ; 5 u`) risks overrunning the single byte before the `EL0` driver schedules! Setting `UART_LCR_H_FEN` enables the 16-byte hardware RX and TX FIFOs so multi-byte escape sequences buffer safely in hardware. However, with `FEN = 1`, `UART_IMSC_RXIM` *alone* only asserts IRQ 33 when the RX FIFO reaches its watermark (default 1/2 full = 8 bytes), which would leave single interactive keystrokes sitting silently in the FIFO! Enabling **`UART_IMSC_RTIM`** (Receive Timeout Interrupt Mask) alongside `RXIM` instructs the PL011 to assert IRQ 33 whenever the RX FIFO is non-empty and 32 bit-periods have elapsed without a new character — delivering both burst resilience and instant single-keystroke responsiveness.

2. **Pre-Drain `UART_ICR` Race-Free Interrupt Loop (`drain_uart_rx`)**:
   In user-space interrupt drivers where GIC masking (`ab_interrupt_dispatch_hw_irq`) and unmasking (`sys_interrupt_ack`) occur across exception boundaries, clearing device interrupt status *after* draining the FIFO creates a classic lost-interrupt race:

```text
  BUGGY POST-CLEAR ORDER                    RACE-FREE PRE-CLEAR ORDER (lines 218-221)
  ----------------------------------------  -------------------------------------------
  1. Read DR until FR.RXFE == 1             1. Outer check: FR.RXFE == 0 (bytes present)
  2. <-- New byte arrives! RXIS=1 -->       2. Write UART_ICR = RXIM | RTIM (clear latch)
  3. Write UART_ICR (WIPES OUT step 2!)     3. Inner loop: read DR until FR.RXFE == 1
  4. sys_interrupt_ack() unmasks GIC 33,    4. <-- If byte arrives during/after step 3:
     but PL011 line is now LOW -> STUCK!       outer loop either drains it immediately
                                               OR PL011 re-asserts IRQ 33 as soon as
                                               sys_interrupt_ack() unmasks GIC 33!
```

3. **25 ms `sys_port_wait` Deadline for `ESC` Disambiguation**:
   Over a serial byte stream, pressing standalone `ESC` (or `Ctrl+3`/`Ctrl+[` in classic terminals) transmits `0x1B`, which is byte-for-byte identical to the leading byte of `Alt+0..3` (`0x1B '0'..'3'`) and ANSI CSI sequences (`0x1B '[' ...`). Rather than spinning or spawning a timer thread, `console-srv` sets `rx.prefix_mode = 2` and `rx.esc_deadline = ulibc_clock_get_monotonic() + 25,000,000` (25 ms) and passes `deadline` directly to `sys_port_wait(port_h, deadline, &pkt)`:
   - Machine-generated `Alt+N` or CSI escape bytes arrive within microseconds over the 16-byte FIFO (often in the *very same* `drain_uart_rx` iteration!), immediately advancing `prefix_mode` to `0` or `3`.
   - A human pressing standalone `ESC` / `Ctrl+3` generates no follow-up byte within 25 ms, causing `sys_port_wait` to return `AB_ERR_TIMED_OUT` and switching cleanly to `tty3`.

4. **Progressive Keyboard Enhancement (`modifyOtherKeys` + Kitty Protocol)**:
   Standard 1970s ASCII terminals lack distinct control codes for `Ctrl+0..3`. By writing `[>4;2m[>1u` at startup (and restoring `[<u[>4;0m` before exit), `console-srv` upgrades xterm, VTE/GNOME Terminal, Kitty, WezTerm, Ghostty, and iTerm2 to transmit unambiguous CSI keycodes (`ESC [ 48..51 ; 5 u` or `ESC [ 27 ; 5 ; 48..51 ~`), while simultaneously supporting `Alt+0..3`, `Ctrl+N`, `Ctrl+T 0..3`, `^0..3`, and the `vt 0..3` shell command on plain serial consoles.

---

## 14.4 user/drivers/fs_srv.c

### 14.4.1 Functional Description

`user/drivers/fs_srv.c` implements ABOS's user-space block storage and TAR archive filesystem server (`fs-srv`). Using its `AB_PA_RESOURCE` capability, `fs-srv` maps the 16 KB VirtIO-MMIO bus aperture (`0x0a000000`), discovers QEMU's `virtio-blk-device` slot and GIC SPI interrupt, allocates a 32-page (128 KB) physically contiguous DMA VMO (`sys_vmo_create_contiguous`), and executes a 3-descriptor scatter-gather Virtqueue DMA read that pulls 232 sectors (116 KB) of `disk.img` into user-space RAM while sleeping on `sys_interrupt_wait`. Once initialized, `fs-srv` multiplexes all 4 shell channels (`fs_ch[0..3]`) on a `PortDispatcher`, serving directory listings (`FS_OP_LIST`) and **zero-copy file VMOs** (`FS_OP_OPEN_VMO`).

### 14.4.2 Heavily Commented Source Code

#### Program 14-3: `lesson_20/user/drivers/fs_srv.c`

<!-- ECLK_CODE_LISTING file="lesson_20/user/drivers/fs_srv.c" start="1" end="304" total="304" -->
```c
       //// File header block: `fs-srv` (`user/drivers/fs_srv.c`) implements both a
       //// DMA-driven VirtIO-MMIO Block driver AND a read-only USTAR filesystem server
       //// entirely in unprivileged `EL0`, concurrently serving all 4 Virtual Terminal
       //// shells over a Zircon `PortDispatcher`.
   1 | /* =========================================================================
   2 |  * drivers/fs_srv.c - EL0 Multi-Client VirtIO-Blk & TAR Filesystem Server
   3 |  *
   4 |  * Lesson 20: Standalone BOOTFS system server linked at VA 0x20000000.
   5 |  * Multiplexes 4 virtual terminal shell channels (tty0..tty3) concurrently
   6 |  * using a Zircon PortDispatcher (sys_port_create + sys_object_wait_async).
   7 |  * ========================================================================= */
   8 | #include "ulibc.h"
   9 | 
       //// VirtIO-MMIO v1 (Legacy) transport register map at physical base `0x0a000000`
       //// (QEMU `virt` machine provides 32 VirtIO-MMIO slots spaced `0x200` = 512 bytes
       //// apart across `[0x0a000000 .. 0x0a004000)`, wired to GIC SPIs `48..79`):
       ////   - `0x000 MAGIC_VALUE` (`0x74726976` = `'virt'`), `0x008 DEVICE_ID` (`2` =
       ////     block).
       ////   - `0x020 GUEST_FEATURES`, `0x028 GUEST_PAGE_SIZE` (Legacy v1 page
       ////     granularity).
       ////   - `0x030 QUEUE_SEL`, `0x038 QUEUE_NUM` (`16`), `0x03C QUEUE_ALIGN`
       ////     (`4096`), `0x040 QUEUE_PFN` (physical page frame number of the
       ////     contiguous Virtqueue).
       ////   - `0x050 QUEUE_NOTIFY` (doorbell), `0x060 INTERRUPT_STAT`, `0x064
       ////     INTERRUPT_ACK`, `0x070 STATUS`, and `0x100 CONFIG` (`struct
       ////     virtio_blk_config.capacity`).
  10 | #define VIRTIO_MMIO_PHYS_BASE       0x0a000000ULL
  11 | #define VIRTIO_MMIO_MAGIC_VALUE     0x000
  12 | #define VIRTIO_MMIO_DEVICE_ID       0x008
  13 | #define VIRTIO_MMIO_GUEST_FEATURES  0x020
  14 | #define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028
  15 | #define VIRTIO_MMIO_QUEUE_SEL       0x030
  16 | #define VIRTIO_MMIO_QUEUE_NUM       0x038
  17 | #define VIRTIO_MMIO_QUEUE_ALIGN     0x03C
  18 | #define VIRTIO_MMIO_QUEUE_PFN       0x040
  19 | #define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
  20 | #define VIRTIO_MMIO_INTERRUPT_STAT  0x060
  21 | #define VIRTIO_MMIO_INTERRUPT_ACK   0x064
  22 | #define VIRTIO_MMIO_STATUS          0x070
  23 | #define VIRTIO_MMIO_CONFIG          0x100
  24 | #define VIRTIO_RING_SIZE            16
  25 | 
       //// Contiguous 128 KB (`32 * 4 KB`) DMA buffer partitioning:
       ////   - `FS_DMA_META_PAGES = 3` (12 KB): Pages 0..1 hold the Legacy Virtqueue
       ////     (Descriptor Table + Available Ring in Page 0; 4 KB-aligned Used Ring in
       ////     Page 1), and Page 2 holds the 16-byte `virtio_blk_req_t` header
       ////     (`+0x2000`) plus the 1-byte DMA completion status (`+0x2040`).
       ////   - `FS_TAR_CACHE_PAGES = 29` (116 KB = 118,784 bytes = **232 512-byte
       ////     sectors**): Pages 3..31 receive the entire USTAR archive from `disk.img`
       ////     in a single scatter-gather DMA read!
  26 | #define FS_DMA_TOTAL_PAGES          32U
  27 | #define FS_DMA_META_PAGES           3U
  28 | #define FS_TAR_CACHE_PAGES          (FS_DMA_TOTAL_PAGES - FS_DMA_META_PAGES)
  29 | #define FS_TAR_CACHE_MAX_BYTES      (FS_TAR_CACHE_PAGES * PAGE_SIZE)
  30 | 
       //// Packed Virtqueue split-ring structures defined by the OASIS VirtIO 1.0 (Legacy
       //// MMIO) specification:
       ////   - `virtq_desc_t` (16 bytes): 64-bit guest physical address `addr`, byte
       ////     length `len`, `flags` (`1 = VIRTQ_DESC_F_NEXT`, `2 =
       ////     VIRTQ_DESC_F_WRITE`), and chained descriptor index `next`.
       ////   - `virtq_avail_t` (36 bytes for `VIRTIO_RING_SIZE = 16`): driver-written
       ////     head-descriptor ring indexed by monotonically increasing `idx`.
  31 | typedef struct {
  32 |     uint64_t addr;
  33 |     uint32_t len;
  34 |     uint16_t flags;
  35 |     uint16_t next;
  36 | } __attribute__((packed)) virtq_desc_t;
  37 | 
  38 | typedef struct {
  39 |     uint16_t flags;
  40 |     uint16_t idx;
  41 |     uint16_t ring[VIRTIO_RING_SIZE];
  42 | } __attribute__((packed)) virtq_avail_t;
  43 | 
       //// `virtio_blk_req_t` (16 bytes): Descriptor 0 payload specifying `type` (`0 =
       //// VIRTIO_BLK_T_IN` read, `1 = VIRTIO_BLK_T_OUT` write), `reserved = 0`, and
       //// starting 512-byte `sector`.
  44 | typedef struct {
  45 |     uint32_t type;
  46 |     uint32_t reserved;
  47 |     uint64_t sector;
  48 | } __attribute__((packed)) virtio_blk_req_t;
  49 | 
       //// `tar_header_t`: standard POSIX.1-1988 USTAR 512-byte archive header
       //// (`name[100]` at offset 0, `size[12]` octal ASCII string at offset 124,
       //// `magic[6]` `"ustar"` at offset 257).
  50 | typedef struct {
  51 |     char name[100];
  52 |     char mode[8];
  53 |     char uid[8];
  54 |     char gid[8];
  55 |     char size[12];
  56 |     char mtime[12];
  57 |     char chksum[8];
  58 |     char typeflag;
  59 |     char linkname[100];
  60 |     char magic[6];
  61 | } __attribute__((packed)) tar_header_t;
  62 | 
       //// 32-bit `volatile` MMIO accessors (`mmio_r32`, `mmio_w32`) and ASCII octal
       //// converter (`oct2uint`).
  63 | static inline uint32_t mmio_r32(vaddr_t base, uint32_t off) {
  64 |     return *(volatile uint32_t *)(base + off);
  65 | }
  66 | 
  67 | static inline void mmio_w32(vaddr_t base, uint32_t off, uint32_t val) {
  68 |     *(volatile uint32_t *)(base + off) = val;
  69 | }
  70 | 
  71 | static uint32_t oct2uint(const char *oct, size_t len) {
  72 |     uint32_t val = 0;
  73 |     for (size_t i = 0; i < len && oct[i]; i++) {
  74 |         if (oct[i] >= '0' && oct[i] <= '7') {
  75 |             val = (val << 3) | (uint32_t)(oct[i] - '0');
  76 |         }
  77 |     }
  78 |     return val;
  79 | }
  80 | 
       //// `handle_fs_request` — `FS_OP_LIST` branch: scans the cached TAR archive in
       //// `tar_cache`, formats a bulleted directory listing (`"  - motd.txt\n"`, `"  -
       //// zircon.txt\n"`, `"  - calc.bin\n"`) into `rep.data[236]`, and sends the single
       //// `fs_msg_t` reply over `ch_h`.
  81 | static int handle_fs_request(ab_handle_t ch_h, const uint8_t *tar_cache,
  82 |                              size_t cache_bytes, const fs_msg_t *req_msg) {
  83 |     if (req_msg->op == FS_OP_LIST) {
  84 |         fs_msg_t rep;
  85 |         rep.op        = FS_OP_LIST;
  86 |         rep.status    = 0;
  87 |         rep.file_size = 0;
  88 |         size_t out_pos = 0;
  89 | 
  90 |         size_t off = 0;
  91 |         while (off + 512 <= cache_bytes) {
  92 |             const tar_header_t *th = (const tar_header_t *)(tar_cache + off);
  93 |             if (th->name[0] == '\0') break;
  94 |             uint32_t fsz = oct2uint(th->size, 12);
  95 |             const char *p = "  - ";
  96 |             while (*p && out_pos + 1 < sizeof(rep.data)) rep.data[out_pos++] = *p++;
  97 |             for (size_t i = 0; th->name[i] && out_pos + 1 < sizeof(rep.data); i++) {
  98 |                 rep.data[out_pos++] = th->name[i];
  99 |             }
 100 |             if (out_pos + 1 < sizeof(rep.data)) rep.data[out_pos++] = '\n';
 101 |             off += 512 + ((fsz + 511U) & ~511U);
 102 |         }
 103 |         rep.data[out_pos] = '\0';
 104 |         sys_channel_write(ch_h, &rep, sizeof(rep), NULL, 0);
 105 |         return 0;
       //// `handle_fs_request` — `FS_OP_OPEN_VMO` branch (Zero-Copy VMO File Serving!):
       //// Instead of streaming file data in tiny 236-byte IPC chunks over a Channel,
       //// `fs-srv` implements Zircon `fuchsia.io`'s `GetBackingMemory` pattern:
       ////   1. Locates the requested filename (`req_msg->data`) in `tar_cache`.
       ////   2. Creates a page-aligned `VmObjectDispatcher` (`rw_vmo`), labels it with
       ////      `th->name` (`sys_object_set_name` -> visible in `vmaps`!), and
       ////      populates it once via `sys_vmo_write`.
       ////   3. Calls `sys_handle_duplicate` to create an attenuated client handle
       ////      `ro_file_vmo` carrying `READ | WRITE | MAP | EXECUTE | TRANSFER |
       ////      INSPECT` (stripping `AB_RIGHT_DUPLICATE` and `AB_RIGHT_SET_PROPERTY`,
       ////      while permitting `shell` to either map text files `AB_VM_PERM_READ` for
       ////      `cat` or map flat binaries `RWX` at `0x20000000` for `calc.bin`), and
       ////      closes `fs-srv`'s own `rw_vmo` handle.
       ////   4. Transfers `ro_file_vmo` across process boundaries in
       ////      `sys_channel_write`'s handle array (`&ro_file_vmo, 1`) alongside
       ////      `rep.file_size`!
 106 |     } else if (req_msg->op == FS_OP_OPEN_VMO) {
 107 |         fs_msg_t rep;
 108 |         rep.op        = FS_OP_OPEN_VMO;
 109 |         rep.status    = (uint32_t)AB_ERR_NOT_FOUND;
 110 |         rep.file_size = 0;
 111 |         rep.data[0]   = '\0';
 112 | 
 113 |         ab_handle_t ro_file_vmo = AB_HANDLE_INVALID;
 114 |         size_t off = 0;
 115 |         while (off + 512 <= cache_bytes) {
 116 |             const tar_header_t *th = (const tar_header_t *)(tar_cache + off);
 117 |             if (th->name[0] == '\0') break;
 118 |             uint32_t fsz = oct2uint(th->size, 12);
 119 |             if (u_strcmp(th->name, req_msg->data) == 0 &&
 120 |                 off + 512 + fsz <= cache_bytes) {
 121 |                 ab_handle_t rw_vmo = AB_HANDLE_INVALID;
 122 |                 uint64_t vmo_sz = ((uint64_t)fsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
 123 |                 if (vmo_sz == 0) vmo_sz = PAGE_SIZE;
 124 |                 if (sys_vmo_create(vmo_sz, 0, &rw_vmo) == AB_OK) {
 125 |                     sys_object_set_name(rw_vmo, th->name);
 126 |                     sys_vmo_write(rw_vmo, tar_cache + off + 512, 0, fsz);
 127 |                     sys_handle_duplicate(rw_vmo,
 128 |                                          AB_RIGHT_READ | AB_RIGHT_WRITE | AB_RIGHT_MAP |
 129 |                                          AB_RIGHT_EXECUTE | AB_RIGHT_TRANSFER | AB_RIGHT_INSPECT,
 130 |                                          &ro_file_vmo);
 131 |                     sys_handle_close(rw_vmo);
 132 |                     rep.status    = 0;
 133 |                     rep.file_size = fsz;
 134 |                 }
 135 |                 break;
 136 |             }
 137 |             off += 512 + ((fsz + 511U) & ~511U);
 138 |         }
 139 | 
 140 |         if (ro_file_vmo != AB_HANDLE_INVALID) {
 141 |             sys_channel_write(ch_h, &rep, sizeof(rep), &ro_file_vmo, 1);
 142 |         } else {
 143 |             sys_channel_write(ch_h, &rep, sizeof(rep), NULL, 0);
 144 |         }
 145 |         return 0;
       //// `FS_OP_SHUTDOWN` returns `1` so the `Port` server loop exits cleanly when a
       //// user runs `exit` in any shell.
 146 |     } else if (req_msg->op == FS_OP_SHUTDOWN) {
 147 |         return 1;
 148 |     }
 149 |     return 0;
 150 | }
 151 | 
       //// `main()`: retrieves `fs-srv`'s 7 startup handles (`[0]=log_h`, `[1]=vmar_h`,
       //// `[2]=res_h`, and `[3..6]=fs_ch[0..3]`).
 152 | int main(void) {
 153 |     uint32_t act_b = 0, act_h = 0;
 154 | 
 155 |     ab_handle_t log_h  = ulibc_get_startup_handle(0);
 156 |     ab_handle_t vmar_h = ulibc_get_startup_handle(1);
 157 |     ab_handle_t res_h  = ulibc_get_startup_handle(2);
 158 |     ab_handle_t fs_ch[NUM_VTS];
 159 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 160 |         fs_ch[i] = ulibc_get_startup_handle(3 + i);
 161 |     }
 162 | 
       //// Map all 32 VirtIO-MMIO device slots (`32 * 0x200 = 16 KB = 4 * PAGE_SIZE`)
       //// starting at physical address `0x0a000000` into `EL0` (`mmio_uva`) via
       //// `sys_vmo_create_physical` + `sys_vmar_map`.
 163 |     /* Map VirtIO MMIO and allocate 32-page (128KB) Contiguous DMA VMO */
 164 |     ab_handle_t mmio_vmo = 0;
 165 |     sys_vmo_create_physical(res_h, VIRTIO_MMIO_PHYS_BASE, 4 * PAGE_SIZE, &mmio_vmo);
 166 |     sys_object_set_name(mmio_vmo, "virtio-blk-mmio");
 167 | 
 168 |     vaddr_t mmio_uva = 0;
 169 |     sys_vmar_map(vmar_h, 0, mmio_vmo, 0, 4 * PAGE_SIZE,
 170 |                  AB_VM_PERM_READ | AB_VM_PERM_WRITE, &mmio_uva);
 171 | 
       //// Dynamic VirtIO-MMIO bus enumeration in `EL0`: probe all 32 slots (`0..31`) for
       //// the little-endian magic `0x74726976` (`'v','i','r','t'`) and
       //// `VIRTIO_MMIO_DEVICE_ID == 2` (Block Device). Note that QEMU `virt` assigns
       //// slots from highest (`slot 31`) or lowest depending on `-device` flags; probing
       //// all 32 slots dynamically discovers both `dev_base` and its exact GICv2 SPI
       //// interrupt line `dev_irq = 48 + slot` without hardcoding a slot index!
 172 |     vaddr_t dev_base = 0;
 173 |     uint32_t dev_irq = 0;
 174 |     for (uint32_t slot = 0; slot < 32; slot++) {
 175 |         vaddr_t cand = mmio_uva + (uint64_t)slot * 0x200ULL;
 176 |         if (mmio_r32(cand, VIRTIO_MMIO_MAGIC_VALUE) == 0x74726976U &&
 177 |             mmio_r32(cand, VIRTIO_MMIO_DEVICE_ID) == 2U) {
 178 |             dev_base = cand;
 179 |             dev_irq  = 48U + slot;
 180 |             break;
 181 |         }
 182 |     }
 183 | 
 184 |     if (dev_base == 0) {
 185 |         ulog(log_h, "[fs-srv EL0] ERROR: No VirtIO-Blk device found!");
 186 |         return -1;
 187 |     }
 188 | 
       //// Read the 64-bit sector count from device-specific configuration space
       //// (`VIRTIO_MMIO_CONFIG` at `+0x100` low 32 bits, `+0x104` high 32 bits) and
       //// clamp `cache_bytes` to `min(disk_bytes, 116 KB)` (`232` sectors).
 189 |     uint64_t cap_lo = mmio_r32(dev_base, VIRTIO_MMIO_CONFIG);
 190 |     uint64_t cap_hi = mmio_r32(dev_base, VIRTIO_MMIO_CONFIG + 4);
 191 |     uint64_t disk_bytes = ((cap_hi << 32) | cap_lo) * 512ULL;
 192 |     size_t cache_bytes = FS_TAR_CACHE_MAX_BYTES;
 193 |     if (disk_bytes > 0 && disk_bytes < cache_bytes) {
 194 |         cache_bytes = (size_t)disk_bytes;
 195 |     }
 196 | 
       //// Bind `dev_irq` via `sys_interrupt_create` and allocate the 32-page (128 KB)
       //// physically contiguous DMA buffer via `sys_vmo_create_contiguous(res_h, ...)`.
       //// Note that `sys_vmo_create_contiguous` returns **both** the mappable `dma_vmo`
       //// handle (mapped at `dma_uva` for CPU access in `EL0`) **and** the physical base
       //// address `dma_pa` needed to program VirtIO's hardware DMA engine!
 197 |     ab_handle_t irq_h = 0, dma_vmo = 0;
 198 |     paddr_t dma_pa = 0;
 199 |     sys_interrupt_create(res_h, dev_irq, &irq_h);
 200 |     if (sys_vmo_create_contiguous(res_h, FS_DMA_TOTAL_PAGES * PAGE_SIZE,
 201 |                                   &dma_vmo, &dma_pa) != AB_OK) {
 202 |         ulog(log_h, "[fs-srv EL0] ERROR: Failed to allocate Contiguous DMA VMO!");
 203 |         return -1;
 204 |     }
 205 |     sys_object_set_name(dma_vmo, "virtio-blk-dma");
 206 | 
 207 |     vaddr_t dma_uva = 0;
 208 |     sys_vmar_map(vmar_h, 0, dma_vmo, 0, FS_DMA_TOTAL_PAGES * PAGE_SIZE,
 209 |                  AB_VM_PERM_READ | AB_VM_PERM_WRITE, &dma_uva);
 210 | 
       //// VirtIO-MMIO v1 (Legacy) initialization handshake:
       ////   1. Write `0` to `STATUS` to reset the device.
       ////   2. Set `ACKNOWLEDGE (1) | DRIVER (2)`.
       ////   3. Negotiate `GUEST_FEATURES = 0` and inform the host of `GUEST_PAGE_SIZE
       ////      = 4096`.
       ////   4. Select Virtqueue `0`, set `QUEUE_NUM = 16`, `QUEUE_ALIGN = 4096`, and
       ////      write `QUEUE_PFN = dma_pa >> 12`.
       ////   5. Set `DRIVER_OK (4)` (`1 | 2 | 4 = 7`), marking the queue live.
 211 |     mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 0);
 212 |     mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 1 | 2);
 213 |     mmio_w32(dev_base, VIRTIO_MMIO_GUEST_FEATURES, 0);
 214 |     mmio_w32(dev_base, VIRTIO_MMIO_GUEST_PAGE_SIZE, PAGE_SIZE);
 215 |     mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_SEL, 0);
 216 |     mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_RING_SIZE);
 217 |     mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_ALIGN, PAGE_SIZE);
 218 |     mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_PFN, (uint32_t)(dma_pa >> PAGE_SHIFT));
 219 |     mmio_w32(dev_base, VIRTIO_MMIO_STATUS, 1 | 2 | 4);
 220 | 
       //// Compute `EL0` virtual pointers inside `dma_uva` corresponding to `dma_pa`:
       ////   - `desc`       at `+0x0000` (Page 0: 16 * 16 B = 256 B)
       ////   - `avail`      at `+0x0100` (Page 0: 36 B; Page 1 `+0x1000` holds `used`
       ////     ring)
       ////   - `req`        at `+0x2000` (Page 2: 16 B `virtio_blk_req_t`)
       ////   - `req_status` at `+0x2040` (Page 2: 1 B status, poisoned to `0xFF`)
       ////   - `tar_cache`  at `+0x3000` (Pages 3..31: 116 KB TAR sector cache)
 221 |     virtq_desc_t  *desc  = (virtq_desc_t *)dma_uva;
 222 |     virtq_avail_t *avail = (virtq_avail_t *)(dma_uva + VIRTIO_RING_SIZE * sizeof(virtq_desc_t));
 223 |     virtio_blk_req_t *req = (virtio_blk_req_t *)(dma_uva + 2 * PAGE_SIZE);
 224 |     volatile uint8_t *req_status = (volatile uint8_t *)(dma_uva + 2 * PAGE_SIZE + 64);
 225 |     uint8_t *tar_cache    = (uint8_t *)(dma_uva + FS_DMA_META_PAGES * PAGE_SIZE);
 226 | 
 227 |     req->type     = 0;
 228 |     req->reserved = 0;
 229 |     req->sector   = 0;
 230 |     *req_status   = 0xFF;
 231 | 
       //// Build the 3-descriptor chained VirtIO-Blk DMA read request (`sector = 0`):
       ////   - `desc[0]`: reads 16-byte `virtio_blk_req_t` at `dma_pa + 0x2000` (`flags
       ////     = NEXT (1)`, `next = 1`).
       ////   - `desc[1]`: DMA-writes `cache_bytes` (116 KB = 232 sectors) into `dma_pa
       ////     + 0x3000` (`flags = NEXT | WRITE (1 | 2)`, `next = 2`).
       ////   - `desc[2]`: DMA-writes 1-byte `VIRTIO_BLK_S_OK (0)` completion byte into
       ////     `dma_pa + 0x2040` (`flags = WRITE (2)`, `next = 0` end-of-chain).
 232 |     desc[0].addr  = dma_pa + 2 * PAGE_SIZE;
 233 |     desc[0].len   = sizeof(virtio_blk_req_t);
 234 |     desc[0].flags = 1;
 235 |     desc[0].next  = 1;
 236 | 
 237 |     desc[1].addr  = dma_pa + FS_DMA_META_PAGES * PAGE_SIZE;
 238 |     desc[1].len   = (uint32_t)cache_bytes;
 239 |     desc[1].flags = 1 | 2;
 240 |     desc[1].next  = 2;
 241 | 
 242 |     desc[2].addr  = dma_pa + 2 * PAGE_SIZE + 64;
 243 |     desc[2].len   = 1;
 244 |     desc[2].flags = 2;
 245 |     desc[2].next  = 0;
 246 | 
       //// Publish head descriptor `0` into `avail->ring[0]`, execute `dsb sy` so all
       //// descriptor/request stores are globally visible before `avail->idx = 1` becomes
       //// visible, execute a second `dsb sy` before ringing the MMIO doorbell
       //// (`VIRTIO_MMIO_QUEUE_NOTIFY = 0`), and block in `EL0` on
       //// `sys_interrupt_wait(irq_h)`!
 247 |     avail->ring[0] = 0;
 248 |     __asm__ volatile("dsb sy" ::: "memory");
 249 |     avail->idx = 1;
 250 |     __asm__ volatile("dsb sy" ::: "memory");
 251 |     mmio_w32(dev_base, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
 252 | 
       //// DMA completion path: when QEMU finishes transferring all 232 sectors into RAM
       //// and asserts `dev_irq`, the ABOS kernel masks `dev_irq` at the GIC and wakes
       //// `fs-srv`. `fs-srv` acknowledges the device's `INTERRUPT_STAT` register,
       //// unmasks `dev_irq` via `sys_interrupt_ack(irq_h)`, verifies `*req_status == 0`
       //// (`VIRTIO_BLK_S_OK`), and logs the cached sector count via `dlog_printf`.
 253 |     sys_interrupt_wait(irq_h);
 254 |     mmio_w32(dev_base, VIRTIO_MMIO_INTERRUPT_ACK, mmio_r32(dev_base, VIRTIO_MMIO_INTERRUPT_STAT));
 255 |     sys_interrupt_ack(irq_h);
 256 | 
 257 |     if (*req_status != 0) {
 258 |         ulog(log_h, "[fs-srv EL0] ERROR: VirtIO-Blk DMA read returned non-zero status!");
 259 |         cache_bytes = 0;
 260 |     } else {
 261 |         dlog_printf("[fs-srv EL0] Cached %lu TAR sectors (%lu KB) via DMA! Multiplexing 4 VT channels on Port...",
 262 |                     (unsigned long)(cache_bytes / 512), (unsigned long)(cache_bytes / 1024));
 263 |     }
 264 | 
       //// Create a `PortDispatcher` (`port_h`) and register asynchronous wait observers
       //// on all 4 Virtual Terminal filesystem channels (`fs_ch[0..3]`, keys `0..3`).
 265 |     ab_handle_t port_h = AB_HANDLE_INVALID;
 266 |     sys_port_create(0, &port_h);
 267 |     for (uint32_t i = 0; i < NUM_VTS; i++) {
 268 |         sys_object_wait_async(fs_ch[i], port_h, (uint64_t)i,
 269 |                               AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
 270 |     }
 271 | 
       //// Event-driven multi-client IPC server loop: sleeps in `sys_port_wait` until any
       //// of the 4 shells sends an `FS_OP_LIST`, `FS_OP_OPEN_VMO`, or `FS_OP_SHUTDOWN`
       //// message, drains all queued requests on `fs_ch[idx]`, and either closes
       //// `fs_ch[idx]` on `AB_SIGNAL_PEER_CLOSED` or re-arms `sys_object_wait_async`.
 272 |     int running = 1;
 273 |     while (running) {
 274 |         ab_port_packet_t pkt;
 275 |         if (sys_port_wait(port_h, AB_TIME_INFINITE, &pkt) != AB_OK) {
 276 |             continue;
 277 |         }
 278 |         uint32_t idx = (uint32_t)pkt.key;
 279 |         if (idx >= NUM_VTS || fs_ch[idx] == AB_HANDLE_INVALID) {
 280 |             continue;
 281 |         }
 282 | 
 283 |         fs_msg_t req_msg;
 284 |         while (sys_channel_read(fs_ch[idx], &req_msg, sizeof(req_msg),
 285 |                                 NULL, 0, &act_b, &act_h) == AB_OK) {
 286 |             if (handle_fs_request(fs_ch[idx], tar_cache, cache_bytes, &req_msg)) {
 287 |                 running = 0;
 288 |                 break;
 289 |             }
 290 |         }
 291 | 
 292 |         if (!running) break;
 293 | 
 294 |         if (pkt.observed & AB_SIGNAL_PEER_CLOSED) {
 295 |             sys_handle_close(fs_ch[idx]);
 296 |             fs_ch[idx] = AB_HANDLE_INVALID;
 297 |         } else {
 298 |             sys_object_wait_async(fs_ch[idx], port_h, (uint64_t)idx,
 299 |                                   AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
 300 |         }
 301 |     }
 302 | 
 303 |     return 0;
 304 | }
```

### 14.4.3 Technical Notes

1. **32-Page (128 KB) Contiguous DMA Memory Layout**:
   Because VirtIO-MMIO v1 (`QUEUE_PFN`) requires the Virtqueue's Descriptor Table, Available Ring, and 4 KB-aligned Used Ring to occupy physically contiguous pages — and DMA scatter-gather descriptors (`virtq_desc_t.addr`) require physical addresses directly visible to the VirtIO host device — `fs-srv` carves a single 32-page (`131,072`-byte) contiguous DMA VMO (`dma_pa` / `dma_uva`) into four regions:

```text
  Offset     Page(s)   Size     DMA Role & Direction
  ---------  --------  -------  -------------------------------------------------------
  +0x00000   Page 0    4 KB     virtq_desc_t[16] (256 B) + virtq_avail_t (36 B) [CPU->Dev]
  +0x01000   Page 1    4 KB     virtq_used_t ring (aligned to QUEUE_ALIGN=4096) [Dev->CPU]
  +0x02000   Page 2    4 KB     +0x2000: virtio_blk_req_t (16 B: T_IN, sector=0)[CPU->Dev]
                                +0x2040: req_status (1 B: 0xFF -> 0x00 S_OK)    [Dev->CPU]
  +0x03000   Pages     116 KB   tar_cache (29 * 4 KB = 118,784 B = 232 sectors) [Dev->CPU]
  ..0x1FFFF  3..31              Receives motd.txt, zircon.txt, calc.bin USTAR!
```

2. **3-Descriptor Chained Virtqueue Transaction & ARMv8 `dsb sy` Ordering**:
   VirtIO block requests always consist of three logically separate buffers chained via `VIRTQ_DESC_F_NEXT (1)`:
   - **`desc[0]`** (`flags = 1`, `next = 1`): 16-byte device-readable `virtio_blk_req_t` command header (`type = 0` read, `sector = 0`).
   - **`desc[1]`** (`flags = 1 | 2`, `next = 2`): 118,784-byte device-writable (`VIRTQ_DESC_F_WRITE = 2`) payload buffer (`tar_cache`).
   - **`desc[2]`** (`flags = 2`, `next = 0`): 1-byte device-writable status trailer (`req_status`).
   On weakly ordered ARMv8-A multiprocessors, `dma_uva` is mapped as `Normal Write-Back` memory whereas `dev_base` (`0x0a000000`) is mapped as `Device-nGnRnE`. Executing `dsb sy` before writing `avail->idx = 1` ensures the device never observes an updated `idx` with stale descriptor entries, and executing a second `dsb sy` before writing `VIRTIO_MMIO_QUEUE_NOTIFY` guarantees all RAM writes have drained prior to the MMIO doorbell!

3. **Zero-Copy VMO Serving vs Byte-Stream IPC**:
   Look at how `FS_OP_OPEN_VMO` serves `calc.bin` (or any file opened via `cat`): regardless of whether a file is 200 bytes (`motd.txt`) or 16 KB (`calc.bin`), the IPC message over `fs_ch` transfers **one `fs_msg_t` header and one `ab_handle_t` VMO capability**! The client (`abos-shell`) maps those physical VMO pages directly into its own `VmAspace` (for `cat`) or into a newly created child process's `VmAspace` at `0x20000000` (for `calc.bin`), completely eliminating multi-kilobyte `Channel` copy loops.

---

## 14.5 Summary

In this chapter we studied the three pillars of ABOS's user-space system layer:

1. **Root Bootstrap & Least-Privilege Capability Wiring (`userboot.c`)**: Embedded as the sole `EL0` component in `kernel.img`, `userboot` parses `bootfs.tar` in `EL0`, spawns `console-srv`, `fs-srv`, and 4 `abos-shell` instances at `0x20000000`, restricts `AB_PA_RESOURCE` strictly to the two driver servers while withholding `Job` from drivers and withholding `Resource` from shells, and immediately reclaims all `bootfs.tar` physical pages.
2. **Multi-VT UART Console Server (`console_srv.c`)**: Maps PL011 MMIO and GIC IRQ 33 in `EL0`, pairs hardware 16-byte FIFOs with `RXIM | RTIM` and a race-free pre-drain `UART_ICR` loop, maintains 4 independent Virtual Terminals (`tty0..tty3`) with 512-byte RX rings and 1024-byte screen scrollback buffers, and multiplexes `Port` IPC packets alongside a 25 ms `ESC`/CSI hotkey disambiguation timer.
3. **VirtIO-Blk DMA & VMO Filesystem Server (`fs_srv.c`)**: Discovers QEMU's VirtIO-Blk device across 32 MMIO slots, caches 232 TAR sectors (116 KB) via a 3-descriptor chained DMA read into a 32-page contiguous VMO, and serves files to all 4 shells concurrently via rights-attenuated VMO capability handles.

In **Chapter 15**, we complete our journey through the ABOS source tree by examining the interactive client programs that sit atop these `EL0` servers: `user/shell/shell.c` and `user/apps/calc.c`.
