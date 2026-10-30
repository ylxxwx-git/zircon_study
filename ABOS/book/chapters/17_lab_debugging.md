# 17 Experimental Environment, Running in QEMU and GDB Debugging

In Chapter 17 of Dr. Zhao Jiong's *A Heavily Commented Linux Kernel Source Code*, the theoretical source-code analysis culminates in a live laboratory environment: booting Linux 0.12 inside the Bochs IA-32 emulator, mounting root floppy/hard-disk images, inspecting binary symbol tables, attaching a symbolic debugger across privilege rings, and modifying the kernel with hands-on experiments.

Operating systems are living machines—reading source code tells you *what* an instruction sequence specifies, whereas watching 4 SMP Cortex-A53 cores come online, inspecting capability handles and ASID-tagged page tables live, and catching an `EL0` data abort inside `arm64_sync_exception` under `gdb-multiarch` reveals *how* hardware and software truly interact.

This chapter serves as the complete laboratory manual for **ABOS Lesson 20**:
* **Section 17.1** dissects the QEMU `virt` AArch64 virtual hardware board, every CLI flag in `make run`, and QEMU Monitor (`Ctrl+A c`) hardware inspection commands.
* **Section 17.2** walks line-by-line through a clean build and the live 4-core SMP boot transcript from `_start` and `lk_main` all the way to `abos[tty0]$ `.
* **Section 17.3** conducts four interactive capability and VM introspection labs (`caps`, `ps -T`, `ps -J`, `vmaps all`, virtual terminal switching, zero-copy VMO `cat`, and `calc`).
* **Section 17.4** uses `tar`, `nm`, and `objdump` on the host to inspect USTAR images, physical-vs-higher-half symbol addresses, and compiler-generated jump tables.
* **Section 17.5** provides a complete `gdb-multiarch` recipe (`.gdbinit`) for stepping across the MMU turn-on boundary (`0x40080000` -> `0xFFFFFFFF80084000`) and debugging overlapping `0x20000000` `EL0` servers and shells.
* **Section 17.6** presents three guided, self-contained kernel and user-space hacking projects: adding syscall `35` (`SYS_CLOCK_GET_MONOTONIC`), building and packing a new `hello.bin` application onto `disk.img`, and writing a `crash.bin` fault-injection binary to verify microkernel fault isolation.

---

## 17.1 QEMU virt Simulation Environment and Hardware Flags

### 17.1.1 The QEMU `virt` AArch64 Hardware Platform

Unlike legacy x86 PCs (which hardcode ISA ports `0x60`, `0x3F8`, `0x1F0`, and BIOS `INT 0x13`), 64-bit ARM servers and embedded SoCs have no fixed PC/AT I/O port space. Instead, QEMU provides the **`virt`** machine model: a clean, synthetic ARMv8-A platform that exposes standard ARM IP blocks (GICv2, PL011 UART, ARM Generic Timer, PSCI firmware, and VirtIO-MMIO transports) and describes its exact RAM and initrd geometry via a **Flattened Device Tree (FDT / DTB)** passed in register `x0` at reset.

Table 17-1 lists the physical memory map and interrupt topology of QEMU `virt` consumed by ABOS Lesson 20:

| Hardware Block | Physical Address Range | Kernel Mapping (`TTBR1_EL1`) / `EL0` Owner | GICv2 INTID | Consumer in ABOS |
| :--- | :--- | :--- | :---: | :--- |
| **GICv2 Distributor (`GICD`)** | `0x08000000 – 0x0800FFFF` | Periphmap `0xFFFFFFFFC8000000` (`EL1`) | — | `kernel/dev/gic.c` |
| **GICv2 CPU Interface (`GICC`)** | `0x08010000 – 0x0801FFFF` | Periphmap `0xFFFFFFFFC8010000` (`EL1`) | — | `kernel/dev/gic.c` |
| **ARM Generic Phys Timer** | System regs `cntpct_el0` / `cntp_*` | Per-CPU system registers (`EL1` & `EL0` RO) | **PPI 30** | `kernel/dev/timer.c` (100 Hz tick) |
| **PL011 UART0** | `0x09000000 – 0x09000FFF` | `EL1` Periphmap (`dprintf`) + `EL0` Physical VMO | **SPI 33** | `debuglog.c` (`EL1`) & `console-srv` (`EL0`) |
| **VirtIO-MMIO Transports (x32)** | `0x0A000000 – 0x0A003FFF` (`0x200`/slot) | `EL0` Physical VMO (`virtio-blk-mmio`, 16 KB) | **SPI 48..79** | `fs-srv` (`EL0`, discovers slot 31 -> IRQ 79) |
| **PSCI Firmware Conduit** | `HVC #0` (`0xC4000003` = `CPU_ON`) | Emulated in QEMU EL2/EL3 firmware | — | `main.c:psci_cpu_on()` (boots CPUs 1..3) |
| **System DRAM (128 MB default)** | `0x40000000 – 0x47FFFFFF` | Physmap `0xFFFFFFFF80000000 – 0xFFFFFFFF87FFFFFF` | — | `pmm.c` (`32,768` 4 KB frames) |
| **Kernel `.idmap` Load Base** | `0x40080000` (512 KB into DRAM) | Identity-mapped at boot, then Physmap | — | `start.S:_start` |
| **BOOTFS `-initrd` Image** | `0x44000000 – 0x4400F000` (60 KB) | Copied to `bootfs.tar` VMO, then freed to PMM | — | `dtb.c` -> `main.c` -> `userboot` |
| **Flattened Device Tree (FDT)** | `0x44200000 – 0x44210000` (64 KB) | Read via Physmap `0xFFFFFFFF84200000` (wired) | — | `dtb.c:dtb_parse()` |

### 17.1.2 Dissecting Every Flag of the `make run` Command

In `lesson_20/Makefile` (lines 59–61), `make run` launches QEMU with the following exact invocation:

```bash
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -smp 4 \
    -kernel image/kernel.img \
    -initrd image/bootfs.tar \
    -nographic -serial mon:stdio \
    -drive file=image/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0
```

Each flag plays a specific architectural role:

1. **`-M virt`**: Selects QEMU's generic ARM virtual platform with 128 MB default DRAM starting at `0x40000000`, a GICv2 interrupt controller, a PL011 serial port at `0x09000000`, 32 VirtIO-MMIO device slots at `0x0a000000`, and built-in PSCI (`Power State Coordination Interface`) support via `HVC #0`. (Note: because `virtualization=on` is *not* set, QEMU resets CPU 0 directly into `EL1h`, though `start.S` includes `.Ldrop_el2` so ABOS boots identically even if `-M virt,virtualization=on` is passed!)
2. **`-cpu cortex-a53`**: Instantiates ARMv8.0-A Cortex-A53 cores matching `-mcpu=cortex-a53` in `config.mk`, supporting 4 KB translation granules and 16-bit ASIDs (`ID_AA64MMFR0_EL1.ASIDBits == 0b0010`).
3. **`-smp 4`**: Creates 4 symmetric Cortex-A53 vCPUs (`MPIDR_EL1 & 0xFF` = `0..3`). At power-on, QEMU holds CPUs `1..3` in PSCI powered-off state while CPU `0` enters `_start`; `lk_main()` brings CPUs `1..3` online via `PSCI_CPU_ON`.
4. **`-kernel image/kernel.img`**: Crucial QEMU boot-protocol detail! When `-kernel` is given a **raw flat binary** (`kernel.img`) rather than an ELF executable (`kernel.elf`), QEMU activates the **ARM64 Linux/bare-metal boot protocol**:
   * It loads `kernel.img` at `RAM_BASE + 0x80000` = `0x40080000`.
   * It dynamically generates the machine's Flattened Device Tree (`.dtb`), places it in RAM (`0x44200000`), and passes its physical address in **`x0`** (`x1 = x2 = x3 = 0`).
   *(Caution: if you pass `-kernel image/kernel.elf` by mistake, QEMU uses its generic ELF loader instead, which skips generating/passing the FDT address in `x0` and fails to populate `-initrd` in `/chosen`!)*
5. **`-initrd image/bootfs.tar`**: Loads `bootfs.tar` (`61,440` bytes) into guest DRAM (`0x44000000..0x4400f000`) and injects `/chosen/linux,initrd-start` and `/chosen/linux,initrd-end` properties into the FDT so `dtb_parse()` and `lk_main()` can wrap it into the `BOOTFS` VMO.
6. **`-nographic -serial mon:stdio`**: Disables graphical SDL/GTK windows and multiplexes **both** the guest's PL011 UART0 serial stream and the **QEMU Human Monitor (`mon:`)** onto your host terminal's standard input/output.
7. **`-drive file=image/disk.img,format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0`**: Two-part block attachment:
   * `-drive ...,if=none,id=hd0` opens the 1 MB `image/disk.img` as a raw sector backend named `hd0` without auto-attaching a default PCI controller.
   * `-device virtio-blk-device,drive=hd0` plugs `hd0` specifically into a **VirtIO-MMIO** transport slot (`virtio-blk-device`, as opposed to PCI `virtio-blk-pci`), allowing `fs-srv` in `EL0` to discover magic `0x74726976` (`"virt"`) and `DEVICE_ID == 2` at MMIO offset `0x0a003e00` without needing a PCIe root-complex driver!

### 17.1.3 QEMU Monitor Shortcuts and Live Hardware Inspection

Because `-serial mon:stdio` multiplexes QEMU's monitor on `stdio` using the escape prefix **`Ctrl+A`**, you can inspect CPU system registers, page tables, and physical memory at any moment without even attaching GDB:

| Key Sequence / Monitor Command | Effect |
| :--- | :--- |
| **`Ctrl+A x`** | **Immediately terminate QEMU** and return to your host Linux/macOS shell (essential if you do not run `exit` inside ABOS). |
| **`Ctrl+A c`** | **Toggle** between the ABOS PL011 serial console (`abos[tty0]$ `) and the interactive **`(qemu)`** monitor prompt. Press `Ctrl+A c` again to return to ABOS! |
| **`Ctrl+A h`** | Print a concise summary of all `Ctrl+A` multiplexer hotkeys. |
| **`Ctrl+A Ctrl+A`** | Send a literal `0x01` (`Ctrl+A`) byte through to the guest UART. |
| **`(qemu) info cpus`** | List all 4 Cortex-A53 vCPUs (`#0..#3`), their thread IDs, and halted/running states. |
| **`(qemu) cpu 2`** | Select vCPU 2 as the active monitor target for subsequent register/memory queries. |
| **`(qemu) info registers`** | Dump `X0..X30`, `PC`, `SP`, `PSTATE`, `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `ESR_EL1`, `FAR_EL1`, and `VBAR_EL1` on the selected vCPU. |
| **`(qemu) info mem`** / **`info mtree`** | Inspect active virtual memory mappings or QEMU's physical MemoryRegion tree (`0x08000000` GIC, `0x09000000` PL011, `0x0a000000` VirtIO, `0x40000000` RAM). |
| **`(qemu) xp /8gx 0x40083000`** | Examine 8 64-bit giant words (`gx`) at **physical** address `0x40083000` (`kernel_table`, showing the two 1 GB block descriptors for Physmap and Periphmap). |
| **`(qemu) x /8i 0xffffffff80084bc8`** | Disassemble 8 AArch64 instructions at **virtual** address `0xffffffff80084bc8` (`lk_main`). |

---

## 17.2 Building and Running ABOS (Annotated Boot Transcript)

### 17.2.1 Clean Compilation Output (`make clean && make`)

Running a clean build inside `lesson_20/` exercises every stage of the build DAG analyzed in Chapter 16:

```text
$ cd lesson_20
$ make clean && make
Cleaning up all subdirectory build artifacts...
  [CC]      kernel/core/dtb.c
  [CC]      kernel/core/exceptions_c.c
  [CC]      kernel/core/main.c
  [CC]      kernel/core/object.c
  [CC]      kernel/core/syscalls.c
  [CC]      kernel/core/thread.c
  [CC]      kernel/core/wait.c
  [AS]      kernel/core/exceptions.S
  [AS]      kernel/core/start.S
  [AS]      kernel/core/switch.S
  [AS]      kernel/core/uspace_entry.S
  [CC]      kernel/mm/heap.c
  [CC]      kernel/mm/mmu.c
  [CC]      kernel/mm/pmm.c
  [CC]      kernel/dev/debuglog.c
  [CC]      kernel/dev/gic.c
  [CC]      kernel/dev/timer.c
  [AS]      user/ulibc/crt0.S
  [CC]      user/ulibc/crt_env.c
  [CC]      user/ulibc/ulibc.c
  [AR]      user/ulibc/libulibc.a
  [CC]      user/drivers/console_srv.c
  [LD]      image/console-srv.elf (using common user/link.ld)
  [OBJCOPY] image/console-srv.bin
  [CC]      user/drivers/fs_srv.c
  [LD]      image/fs-srv.elf (using common user/link.ld)
  [OBJCOPY] image/fs-srv.bin
  [CC]      user/userboot/userboot.c
  [CC]      user/shell/shell.c
  [LD]      image/shell.elf (using common user/link.ld)
  [OBJCOPY] image/shell.bin
  [CC]      user/apps/calc.c
  [LD]      image/calc.elf (using common user/link.ld)
  [OBJCOPY] image/calc.bin
  [LD]      image/kernel.elf (EL1 kernel + userboot only)
  [OBJCOPY] image/kernel.img
  [BOOTFS]  image/bootfs.tar (console-srv.bin fs-srv.bin shell.bin)
  [TAR]     image/disk.img (motd.txt, zircon.txt, calc.bin)
```

### 17.2.2 Line-by-Line Walkthrough of the 4-Core SMP Boot Transcript

Launching `make run` boots ABOS from physical address `0x40080000` into the interactive capability shell in under 50 milliseconds. Below is the **exact verbatim serial transcript** produced by QEMU, followed by a stage-by-stage architectural dissection:

```text
$ make run
Running ABOS Lesson 20 in QEMU...

==================================================================
Welcome to ABOS (Zircon-Style Microkernel)
Lesson 20: Standalone EL0 System Servers via Zircon-Style BOOTFS
==================================================================
[mmu] Higher-half MMU + I/D-caches active: SCTLR_EL1=0xc5183d TCR_EL1=0x10b5213521 TTBR1_EL1=0x0000000040083000
[dtb] FDT at PA 0x0000000044200000 (VA 0xffffffff84200000, 65536 B): RAM 0x0000000040000000 .. 0x0000000048000000 (128 MB)
[dtb] Initrd (BOOTFS) at PA 0x0000000044000000 .. 0x000000004400f000 (61440 B)
[pmm] Arena initialized: base=0x0000000040000000 pages=32768 (32445 free, 323 wired)
[heap] Initialized 512 KB kernel heap at VA 0xffffffff80124000 (PA 0x0000000040124000)
[smp] Booting secondary CPUs (1..3) via PSCI_CPU_ON...
[cpu 1] secondary scheduler active (EL1)
[cpu 2] secondary scheduler active (EL1)
[cpu 3] secondary scheduler active (EL1)
[smp] All 4 CPUs online and scheduling in EL1!
[bootfs] Created BOOTFS VMO (koid=1027, 61440 bytes from initrd PA 0x0000000044000000) for userboot
[debuglog cpu=0 proc='userboot' koid=1025] [userboot] Mapped BOOTFS VMO! Loading console-srv.bin, fs-srv.bin, and shell.bin @ 0x20000000...
[debuglog cpu=0 proc='console-srv' koid=1025] [console-srv EL0] Multiplexing 4 Virtual Terminals (tty0..tty3) + UART IRQ 33 on Port!
[debuglog cpu=1 proc='abos-shell-1' koid=1025] [abos-shell-1 EL0 @ 0x0000000020000a68] Bound to Virtual Terminal [tty1]!
[debuglog cpu=2 proc='abos-shell-0' koid=1025] [abos-shell-0 EL0 @ 0x0000000020000a68] Bound to Virtual Terminal [tty0]!
[debuglog cpu=3 proc='fs-srv' koid=1025] [fs-srv EL0] Cached 232 TAR sectors (116 KB) via DMA! Multiplexing 4 VT channels on Port...
[debuglog cpu=2 proc='abos-shell-3' koid=1025] [abos-shell-3 EL0 @ 0x0000000020000a68] Bound to Virtual Terminal [tty3]!
[debuglog cpu=1 proc='abos-shell-2' koid=1025] [abos-shell-2 EL0 @ 0x0000000020000a68] Bound to Virtual Terminal [tty2]!

============================================================
  ABOS Virtual Terminal [tty0] (PID koid, EL0)
  Switch: Alt+0..3 | Ctrl+2..5 | Ctrl+N (next) | vt 0..3
============================================================
abos[tty0]$ 
```

Let us trace every single phase of this boot log:

1. **Silent Early Boot (`start.S`: `_start` -> `high_kernel_entry` -> `lk_main`)**:
   Before a single character is printed to the UART, CPU 0 enters `_start` at PA `0x40080000`, stashes the FDT physical address (`0x44200000`) in `x20`, populates two 1 GB block page tables (`idmap_table` at PA `0x40082000` and `kernel_table` at PA `0x40083000`), turns on `SCTLR_EL1.{M,C,I}`, branches to `high_kernel_entry` at higher-half VA `0xffffffff80096320`, installs `VBAR_EL1`, sets `SP` to `_stack_top` (`0xffffffff80124000`), zeroes `.bss`, and calls `lk_main(0x44200000)`. `lk_main()` initializes `dlog_lock` and the bootstrap thread descriptor, then prints the `Welcome to ABOS` banner over Periphmap UART0 (`0xffffffffc9000000`).
2. **`[mmu]` Verification**:
   * `SCTLR_EL1=0xc5183d`: Bit `0` (`M=1`, MMU enabled), Bit `2` (`C=1`, D-cache enabled), Bit `12` (`I=1`, I-cache enabled), alongside ARMv8 architectural `RES1` bits.
   * `TCR_EL1=0x10b5213521`: Notice bit `36` (`0x10_0000_0000` = `TCR_EL1.AS`) was dynamically set by `start.S` lines 95–99 after probing `ID_AA64MMFR0_EL1`, activating **16-bit hardware ASIDs**! Lower 32 bits `0xb5213521` configure `T0SZ=33` (2 GB `TTBR0_EL1` user window `0x0..0x80000000`) and `T1SZ=33` (2 GB `TTBR1_EL1` kernel window `0xffffffff80000000..0xffffffffffffffff`) with 4 KB granules and Inner-Shareable Write-Back Write-Allocate table walks.
   * `TTBR1_EL1=0x0000000040083000`: Physical base address of `kernel_table` in `.idmap.data`.
3. **`[dtb]` Hardware Discovery**:
   `dtb_parse()` translates PA `0x44200000` via `paddr_to_physmap()` to `0xffffffff84200000`, validates big-endian magic `0xd00dfeed`, discovers 128 MB RAM (`0x40000000..0x48000000`), and locates the `-initrd` BOOTFS image at PA `0x44000000..0x4400f000` (`61,440` bytes = `15` pages).
4. **`[pmm]` Physical Frame Accounting**:
   `128 MB / 4 KB = 32,768` total pages. Why are exactly **`323` pages wired**? Let us verify the arithmetic:
   * PA `0x40000000..0x40124000` (512 KB bootloader gap `128` pages + kernel `.idmap`/`.text`/`.rodata`/`.data`/`.bss`/stacks `164` pages up to `_end` = `0x40124000`) = **`292` pages**.
   * FDT at `0x44200000..0x44210000` (`65,536` bytes) = **`16` pages**.
   * Initrd at `0x44000000..0x4400f000` (`61,440` bytes) = **`15` pages**.
   * `292 + 16 + 15 = 323` wired pages, leaving `32,768 - 323 = 32,445` free pages!
5. **`[heap]` Initialization**:
   `heap_init()` allocates `128` contiguous pages (`512` KB) immediately at PA `0x40124000` (VA `0xffffffff80124000`, right at `_end`!) for `kmalloc`/`kzalloc`.
6. **`[smp]` Secondary Core Bring-Up**:
   Follows GICv2 and 100 Hz Generic Timer setup on CPU 0. `lk_main()` issues `HVC #0` (`PSCI_CPU_ON`, `0xC4000003`) for CPUs `1`, `2`, and `3` with entry PA `0x40080000` (`_start`). Seeing `mmu_tables_ready == 1`, CPUs `1..3` enable their MMUs immediately, set up their private 8 KB kernel stacks (`_stack_top - cpu * 8192`), print `[cpu N] secondary scheduler active (EL1)`, increment `secondary_cpu_ready_count`, execute `sev`, and enter their idle threads.
7. **`[bootfs]` Root Object & BOOTFS VMO Creation**:
   `lk_main()` creates the first four kernel objects from `global_koid_counter` (which starts at `1024` in `object.c` line 9!):
   * `koid=1024`: `root_job` (`JOB`)
   * `koid=1025`: `root_log` (`LOG`)
   * `koid=1026`: `root_res` (`RESOURCE`)
   * `koid=1027`: `bootfs_vmo` (`VMO`, named `"bootfs.tar"`, `61,440` bytes copied from `0x44000000`, after which `pmm_free_wired_range()` returns those 15 bootloader initrd pages back to the PMM free list!).
8. **`EL0` Bootstrap Across All 4 SMP Cores (`userboot` -> `console-srv`, `fs-srv`, `abos-shell-0..3`)**:
   Look at the `cpu=` tags in the `[debuglog]` lines! As soon as `userboot` (`koid=1028`, on `cpu=0`) spawns `console-srv`, `fs-srv`, and the four shells, ABOS's preemptive SMP scheduler distributes them across **all 4 physical cores simultaneously**:
   * `cpu=0`: `userboot` and `console-srv` (maps PL011 MMIO, enables IRQ 33, emits xterm/Kitty hotkey sequences, blocks on `sys_port_wait`).
   * `cpu=3`: `fs-srv` (maps VirtIO-Blk MMIO, allocates 128 KB contiguous DMA VMO, reads `232` sectors = `116` KB via Virtqueue DMA, and services the startup barrier pings from the shells).
   * `cpu=1` and `cpu=2`: `abos-shell-0..3` (notice **all four shells** report `main` at the exact same virtual address `0x0000000020000a68`!). Once `fs-srv` answers `tty0`'s readiness ping, `abos-shell-0` prints the `tty0` banner and blocks reading a line at `abos[tty0]$ `.

---

## 17.3 Interactive Introspection Labs (caps, ps, vmaps, VT Switching, cat, calc)

Once at the `abos[tty0]$ ` prompt, you have live interactive access to ABOS's Zircon-inspired diagnostic suite. Let us run four structured labs and learn how to decode every field.

### 17.3.1 Lab 1: Verifying Pure Capability Isolation with `caps`

Type `caps` at `abos[tty0]$ `:

```text
abos[tty0]$ caps
Shell Capability Table (inspected via sys_object_get_info):
  [Handle 0x50000101] Kernel DebugLog     (koid=1025, type=10, rights=0xc0b)
  [Handle 0x50000102] Root Address Space  (koid=1066, type=12, rights=0xc3f)
  [Handle 0x50000103] Console IPC Channel (koid=1035, type=4, rights=0xf0f)
  [Handle 0x50000104] Filesystem IPC Chan (koid=1037, type=4, rights=0xf0f)
  [Handle 0x50000105] Parent Job Cap      (koid=1024, type=11, rights=0x3c0f)
  [Note] Shell holds ZERO Resource/MMIO/IRQ handles (pure capability isolation!)
```

**How to read every hex number in `caps`:**
* **Why do handle values start at `0x50000101` (slot 1) instead of `0x50000100` (slot 0)?**
  Recall `make_handle_value(slot, gen)` in `object.c`: `(0x5 << 28) | (gen << 8) | slot`. When `userboot` called `sys_process_start(..., c_ch, 0)`, the kernel installed the bootstrap channel `boot_ch` (`c_ch`) into **slot `0`** (`0x50000100`). Inside `_start -> __ulibc_init(boot_ch, arg1)`, `sys_channel_read` unpacked the 5 payload handles into **slots `1..5`** (`0x50000101` .. `0x50000105`, all at `generation = 1`) and immediately called `sys_handle_close(boot_ch)`, freeing slot `0` (and incrementing slot `0`'s generation to `2` for future reuse)!
* **Decoding the `rights` bitmasks** (cross-check with Appendix A2):
  * `0xc0b` (`LOG`, type `10`): `TRANSFER (0x2) | DUPLICATE (0x1) | WRITE (0x8) | WAIT (0x400) | INSPECT (0x800)` = `AB_RIGHTS_DEFAULT_LOG`.
  * `0xc3f` (`VMAR`, type `12`): `AB_RIGHTS_BASIC (0xc03) | READ (0x4) | WRITE (0x8) | EXECUTE (0x10) | MAP (0x20)` = `AB_RIGHTS_DEFAULT_VMAR`.
  * `0xf0f` (`CHANNEL`, type `4`): `AB_RIGHTS_BASIC (0xc03) | READ (0x4) | WRITE (0x8) | SIGNAL (0x100) | SIGNAL_PEER (0x200)` = `AB_RIGHTS_DEFAULT_CHANNEL`.
  * `0x3c0f` (`JOB`, type `11`): `AB_RIGHTS_BASIC (0xc03) | READ (0x4) | WRITE (0x8) | MANAGE_PROCESS (0x1000) | MANAGE_THREAD (0x2000)` = `AB_RIGHTS_DEFAULT_JOB` (allowing the shell to run `ps`, `vmaps`, and spawn child apps like `calc.bin`).
* **Security invariant**: Because the shell holds **no** `AB_OBJ_TYPE_RESOURCE` (`type=9`) handle, any attempt by shell code (or a child app) to call `sys_vmo_create_physical`, `sys_vmo_create_contiguous`, or `sys_interrupt_create` fails immediately in `handle_lookup()` with `AB_ERR_BAD_HANDLE` (`-11`) or `AB_ERR_WRONG_TYPE` (`-12`)!

### 17.3.2 Lab 2: Reading Task Trees and Handle Matrices with `ps -T` and `ps -J`

Run `ps -T` (full Job -> Process -> Thread tree plus handle matrix) and `ps -J` (Jobs summary):

```text
abos[tty0]$ ps -T
TASK               PSS PRIVATE  SHARED STATE    NAME
j:1024            524k    444k         active   root
  p:1028          104k     24k     80k active   userboot
    t:1030                  8k         blocked  userboot-main
  p:1051           44k     44k      0B active   console-srv
    t:1053                  8k         blocked  console-srv
  p:1057          184k    184k      0B active   fs-srv
    t:1059                  8k         blocked  fs-srv
  p:1065           48k     48k      0B active   abos-shell-0
    t:1067                  8k         running  abos-shell-0
  p:1074           48k     48k      0B active   abos-shell-1
    t:1076                  8k         blocked  abos-shell-1
  p:1083           48k     48k      0B active   abos-shell-2
    t:1086                  8k         blocked  abos-shell-2
  p:1091           48k     48k      0B active   abos-shell-3
    t:1093                  8k         blocked  abos-shell-3

Zircon Kernel Process Handle Table (DumpProcessList):
     id  #h:  #jb #pr #th #vo #vm #ch #ev #po #ir #rs #lg [name]
   1028   9:    1   6   0   0   0   0   0   0   0   1   1 [userboot]
   1051  10:    0   0   0   1   1   4   0   1   1   1   1 [console-srv]
   1057  11:    0   0   0   2   1   4   0   1   1   1   1 [fs-srv]
   1065   5:    1   0   0   0   1   2   0   0   0   0   1 [abos-shell-0]
   1074   5:    1   0   0   0   1   2   0   0   0   0   1 [abos-shell-1]
   1083   5:    1   0   0   0   1   2   0   0   0   0   1 [abos-shell-2]
   1091   5:    1   0   0   0   1   2   0   0   0   0   1 [abos-shell-3]

abos[tty0]$ ps -J
TASK               PSS PRIVATE  SHARED STATE    NAME
j:1024            524k    444k         active   root
```

**Column-by-column guide to `ps -T`:**
1. **`TASK` (`j:<koid>`, `p:<koid>`, `t:<koid>`)**: Displays the 3-tier Zircon task hierarchy rooted at Job `j:1024` (`root`), containing 7 active `EL0` processes (`p:1028`..`p:1091`), each with its user thread (`t:1030`..`t:1093`).
2. **`PSS` / `PRIVATE` / `SHARED` Memory Columns**:
   * Every thread (`t:...`) reports `8k` (`PRIVATE`) for its dedicated 8 KB `EL1` kernel stack (`THREAD_STACK_SIZE`).
   * `userboot` (`p:1028`) shows `24k` `PRIVATE` (its 6-page `EL0` user stack) and `80k` `SHARED` (the 20-page read-only kernel code/rodata projection `'userboot-rodata'` with `vmo_koid=1`). Notice `bootfs.tar` (`60k`) is *not* listed here because `userboot` cleanly called `sys_vmar_unmap` and `sys_handle_close(bootfs_vmo_h)` on lines 192–193 of `userboot.c` right after spawning the shells!
   * `console-srv` (`p:1051`) uses `44k` (`24k` stack + `16k` `console-srv.bin` + `4k` PL011 MMIO).
   * `fs-srv` (`p:1057`) uses `184k` (`24k` stack + `16k` `fs-srv.bin` + `16k` VirtIO MMIO + `128k` contiguous DMA cache).
   * Each `abos-shell-N` uses `48k` (`24k` stack + `24k` `shell.bin`).
   * Sum of all 7 processes: `104 + 44 + 184 + 4 * 48 = 524k` (`j:1024` `PSS`)!
3. **`STATE`**: Only `t:1067` (`abos-shell-0`, executing `sys_job_get_ps_info` right now!) is `running`; all other 6 user threads are `blocked` inside `sys_object_wait_one` or `sys_port_wait` consuming **0% CPU**.
4. **`DumpProcessList` Handle Matrix**: Look how cleanly the handle table audits system privilege:
   * `userboot` (`#h=9`): holds `1` Job, `6` Process handles (`console-srv`, `fs-srv`, and 4 shells so it can wait for `AB_SIGNAL_TASK_TERMINATED`), `1` Resource, and `1` Log (`#vo=0` and `#vm=0` because it closed its BOOTFS VMO and own VMAR after startup!).
   * `console-srv` (`#h=10`) & `fs-srv` (`#h=11`): hold `4` Channels (`tty0..tty3`), `1` Port (`#po=1`), `1` Interrupt (`#ir=1`), `1` Resource (`#rs=1`), `1` VMAR, `1` Log, plus `1` or `2` driver MMIO/DMA VMOs (`#vo`). Notice `#jb=0`: **even if a hardware driver is compromised, it holds no Job capability and cannot spawn new processes or inspect other tasks!**
   * `abos-shell-0..3` (`#h=5`): hold `1` Job, `1` VMAR, `2` Channels (`con_ch`, `fs_ch`), `1` Log, and **`0` Interrupts / `0` Resources**.

### 17.3.3 Lab 3: Inspecting ASID-Isolated Address Spaces with `vmaps all`

Run `vmaps all` to dump the `VmAspace` (`/A`), `VmAddressRegion` (`/R`, ` R`), and `VmMapping` (` M`) hierarchy of every running process:

```text
abos[tty0]$ vmaps all
--- Process [koid=1028, asid=1, name='userboot'] ---
/A  0000000000001000-0000000080000000         2G:sz                     'userboot'
/R  0000000000001000-0000000040000000         1G:sz                     'root'
 M  0000000000084000-0000000000098000 r-x    80k:sz   80k:res     1:vmo 'userboot-rodata'
 M  0000000000800000-0000000000806000 rw-    24k:sz   24k:res  1033:vmo 'user-stack'
 R  0000000010000000-0000000040000000       768M:sz                     'vmar-dynamic'
--- Process [koid=1051, asid=2, name='console-srv'] ---
/A  0000000000001000-0000000080000000         2G:sz                     'console-srv'
/R  0000000000001000-0000000040000000         1G:sz                     'root'
 M  0000000000800000-0000000000806000 rw-    24k:sz   24k:res  1063:vmo 'user-stack'
 R  0000000010000000-0000000040000000       768M:sz                     'vmar-dynamic'
  M 0000000020000000-0000000020004000 rwx    16k:sz   16k:res  1050:vmo 'console-srv.bin'
  M 0000000010000000-0000000010001000 rw-     4k:sz    4k:res  1070:vmo 'pl011-uart-mmio'
--- Process [koid=1057, asid=3, name='fs-srv'] ---
/A  0000000000001000-0000000080000000         2G:sz                     'fs-srv'
/R  0000000000001000-0000000040000000         1G:sz                     'root'
 M  0000000000800000-0000000000806000 rw-    24k:sz   24k:res  1064:vmo 'user-stack'
 R  0000000010000000-0000000040000000       768M:sz                     'vmar-dynamic'
  M 0000000020000000-0000000020004000 rwx    16k:sz   16k:res  1056:vmo 'fs-srv.bin'
  M 0000000010000000-0000000010004000 rw-    16k:sz   16k:res  1071:vmo 'virtio-blk-mmio'
  M 0000000010005000-0000000010025000 rw-   128k:sz  128k:res  1089:vmo 'virtio-blk-dma'
--- Process [koid=1065, asid=4, name='abos-shell-0'] ---
/A  0000000000001000-0000000080000000         2G:sz                     'abos-shell-0'
/R  0000000000001000-0000000040000000         1G:sz                     'root'
 M  0000000000800000-0000000000806000 rw-    24k:sz   24k:res  1082:vmo 'user-stack'
 R  0000000010000000-0000000040000000       768M:sz                     'vmar-dynamic'
  M 0000000020000000-0000000020006000 rwx    24k:sz   24k:res  1062:vmo 'shell.bin'
```
*(Processes `abos-shell-1..3` with `asid=5..7` exhibit identical layouts backed by distinct VMO KOIDs `1072`, `1081`, and `1090`.)*

**Key observations in `vmaps all`:**
* **Guard page at `0x00000000`**: Every `/A` (`VmAspace`) starts at `0x0000000000001000`, leaving virtual page `0` (`0x0..0xFFF`) permanently unmapped so `NULL` pointer dereferences trap deterministically!
* **Same VA `0x20000000`, six different binaries/instances**: `console-srv` (`asid=2`, `1050:vmo`), `fs-srv` (`asid=3`, `1056:vmo`), and `abos-shell-0..3` (`asid=4..7`, `1062/1072/1081/1090:vmo`) all map their executable image at `0x20000000`. Because every user PTE sets `PTE_NG` (`nG = 1`, Non-Global) and `vmm_switch_aspace()` writes `((uint64_t)asid << 48) | ttbr0_phys` to `TTBR0_EL1`, the Cortex-A53 TLB caches translations for all of them simultaneously without flushing the TLB on context switch!
* **Dynamic MMIO & DMA regions**: When `console-srv` and `fs-srv` pass `vmar_offset == 0` to `sys_vmar_map`, the kernel's bump allocator (`proc->next_map_va`, starting at `0x10000000`) places `'pl011-uart-mmio'`, `'virtio-blk-mmio'`, and `'virtio-blk-dma'` cleanly inside `'vmar-dynamic'` with `rw-` (`PTE_PXN | PTE_UXN` non-executable) permissions.

### 17.3.4 Lab 4: Virtual Terminal Switching, Zero-Copy VMO `cat`, `calc`, and Clean Shutdown

Switch to Virtual Terminal 1 (either by pressing **`Alt+1`**, **`Ctrl+N`**, **`Ctrl+T` then `1`**, or typing **`vt 1`**), list the VirtIO-Blk TAR disk, read `zircon.txt` via VMO mapping, run `calc`, and shut down ABOS:

```text
abos[tty0]$ vt 1

============================================================
 >>> Switched to Virtual Terminal [tty1] (abos-shell-1) <<<
 [Switch: Alt+0..3 | Ctrl+2..5 | Ctrl+N (next) | vt 0..3]
============================================================

============================================================
  ABOS Virtual Terminal [tty1] (PID koid, EL0)
  Switch: Alt+0..3 | Ctrl+2..5 | Ctrl+N (next) | vt 0..3
============================================================
abos[tty1]$ ls
Files in TAR disk.img (served by EL0 fs-srv):
  - motd.txt
  - zircon.txt
  - calc.bin
abos[tty1]$ cat zircon.txt
Zircon Microkernel Principles Demonstrated in ABOS Lesson 20:
  1. Only 'userboot' is embedded in the kernel; servers live in BOOTFS VMO.
  2. console-srv.bin, fs-srv.bin, shell.bin, and calc.bin all share user/link.ld @ 0x20000000.
  3. Run 'vmaps all' to inspect each process's standalone VMO mapping!
abos[tty1]$ calc
[debuglog cpu=0 proc='calc.bin' koid=1025] [calc.bin EL0 @ 0x0000000020000050] crt0 initialized! Running main()...

--- ABOS Calculator (loaded from disk.img:/calc.bin @ VA 0x0000000020000050) ---
Enter first number: 42
Enter operation (+, -, *, /): *
Enter second number: 6
Result: 42 * 6 = 252

[debuglog cpu=0 proc='calc.bin' koid=1025] Result: 42 * 6 = 252
[process 'calc.bin' koid=1101] exiting with code 0; closing remaining handles
abos[tty1]$ exit
Shutting down ABOS user-space servers. Goodbye!
[process 'abos-shell-1' koid=1074] exiting with code 0; closing remaining handles
[process 'console-srv' koid=1051] exiting with code 0; closing remaining handles
[process 'fs-srv' koid=1057] exiting with code 0; closing remaining handles
[process 'abos-shell-0' koid=1065] exiting with code 0; closing remaining handles
[process 'abos-shell-2' koid=1083] exiting with code 0; closing remaining handles
[process 'abos-shell-3' koid=1091] exiting with code 0; closing remaining handles
[debuglog cpu=0 proc='userboot' koid=1025] [userboot] All 4 virtual terminals and user-space servers exited cleanly!
[process 'userboot' koid=1028] exiting with code 0; closing remaining handles
[kernel] LogDispatcher (koid=1025) reached 0 open handles!
[lk_main] ABOS Lesson 20 complete!
```

Notice the cascade when `exit` is entered on `tty1`:
1. `abos-shell-1` sends `FS_OP_SHUTDOWN` to `fs-srv` and `CON_OP_SHUTDOWN` to `console-srv`, then returns `0` from `main()` into `crt0.S -> sys_process_exit(0)`.
2. When `console-srv` exits, its 4 channel endpoints (`vts[0..3].ch`) close automatically inside `ab_process_exit_current()`, asserting `AB_SIGNAL_PEER_CLOSED` on the peer channels held by `abos-shell-0`, `abos-shell-2`, and `abos-shell-3`!
3. Woken from `shell_readline()`, the remaining three shells observe `!(obs & AB_SIGNAL_READABLE)` and exit cleanly.
4. `userboot` wakes from its `sys_object_wait_one(..., AB_SIGNAL_TASK_TERMINATED)` loop, logs completion, and exits, dropping the very last handle to `LogDispatcher (koid=1025)` to `0` (`log_on_zero_handles`) and waking `lk_main()`! *(Press **`Ctrl+A x`** to close QEMU.)*

---

## 17.4 Inspecting Binary Artifacts and Disassembly

Before or during debugging, five standard host commands let you X-ray every archive, symbol address, and instruction generated by the build system.

### 17.4.1 Auditing USTAR Archives (`tar -tvf`)

Because `make_bootfs.py` and `make_tar_disk.py` generate standards-compliant POSIX USTAR archives (even with `disk.img`'s 1 MB trailing zero-padding), standard host `tar` inspects both directly:

```text
$ tar -tvf image/bootfs.tar
-rw-r--r-- 0/0           15508 1970-01-01 00:00 console-srv.bin
-rw-r--r-- 0/0           13172 1970-01-01 00:00 fs-srv.bin
-rw-r--r-- 0/0           20516 1970-01-01 00:00 shell.bin

$ tar -tvf image/disk.img
-rw-r--r-- 0/0             168 1970-01-01 00:00 motd.txt
-rw-r--r-- 0/0             304 1970-01-01 00:00 zircon.txt
-rw-r--r-- 0/0           11212 1970-01-01 00:00 calc.bin
```

Notice `0/0` (`uid=0, gid=0`) and `1970-01-01 00:00` (`mtime=0`), confirming deterministic reproducibility.

### 17.4.2 Inspecting Physical vs. Higher-Half Kernel Symbols (`nm -n`)

Running `aarch64-linux-gnu-nm -n image/kernel.elf` sorts all kernel symbols numerically by address, visibly exposing the `linker.ld` split between the physical `.idmap` trampoline (`0x40080000..0x40083fff`) and the higher-half `TTBR1_EL1` kernel (`0xffffffff80084000..0xffffffff80124000`):

```text
$ aarch64-linux-gnu-nm -n image/kernel.elf | head -n 16
0000000000000705 a PTE_RAM
0000000000000800 a PTE_NG_BIT
000000000000ff00 a MAIR_VALUE
0000000040000000 A BASE_PA
0000000040080000 T _start
0000000040081000 t mmu_tables_ready
0000000040082000 T idmap_table
0000000040083000 T kernel_table
00000000b5213521 a TCR_VALUE
0060000000000401 a PTE_DEVICE
ffffffff40000000 A KERNEL_VA_OFF
ffffffff80084000 T __code_start
ffffffff80084000 t paddr_to_physmap
ffffffff80084000 T _text_start
ffffffff80084020 t be32_to_cpu
ffffffff80084168 T dtb_parse
```

And at the end of `kernel.elf`, we see `page_array` (`32,768 * sizeof(vm_page_t)` = `512` KB in `.bss`), the 32 KB boot stacks (`4 * 8` KB), and `_end` at `0xffffffff80124000`:

```text
$ aarch64-linux-gnu-nm -n image/kernel.elf | tail -n 8
ffffffff8009b120 b page_array
ffffffff8011b120 b dlog_lock
ffffffff8011b128 b timer_freq_hz
ffffffff8011b138 b cpu_timer_ticks
ffffffff8011c000 B _boot_stacks_bottom
ffffffff8011c000 B _bss_end
ffffffff80124000 B _end
ffffffff80124000 B _stack_top
```

### 17.4.3 Interleaved Source & Disassembly (`objdump -d -S`)

Because `config.mk` compiles every `.c` and `.S` file with `-g`, `objdump -d -S` interleaves C/assembly source lines directly above the generated AArch64 machine instructions. For example, inspecting `syscall_dispatch` (`0xffffffff8008e104`) reveals how GCC compiles the 34-case `switch (sys_num)` into a bounds-checked PC-relative jump table:

```text
$ aarch64-linux-gnu-objdump -d -S --start-address=0xffffffff8008e104 \
    --stop-address=0xffffffff8008e160 image/kernel.elf

ffffffff8008e104 <syscall_dispatch>:
void syscall_dispatch(iframe_t *frame) {
ffffffff8008e104:   a9bd7bfd    stp     x29, x30, [sp, #-48]!
ffffffff8008e108:   910003fd    mov     x29, sp
ffffffff8008e10c:   f9000fe0    str     x0, [sp, #24]
    uint64_t sys_num = frame->r[8];
ffffffff8008e110:   f9400fe0    ldr     x0, [sp, #24]
ffffffff8008e114:   f9402000    ldr     x0, [x0, #64]      // offset 64 = r[8] (x8)
ffffffff8008e118:   f90013e0    str     x0, [sp, #32]
    ab_status_t ret = AB_ERR_NOT_SUPPORTED;
ffffffff8008e11c:   12800020    mov     w0, #0xfffffffe    // -2 (AB_ERR_NOT_SUPPORTED)
ffffffff8008e120:   b9002fe0    str     w0, [sp, #44]
    switch (sys_num) {
ffffffff8008e124:   f94013e0    ldr     x0, [sp, #32]
ffffffff8008e128:   d1000400    sub     x0, x0, #0x1       // zero-index: sys_num - 1
ffffffff8008e12c:   f100841f    cmp     x0, #0x21          // 0x21 = 33 -> valid 1..34!
ffffffff8008e130:   540032a8    b.hi    ffffffff8008e784 <syscall_dispatch+0x680>
...
ffffffff8008e144:   b8605820    ldr     w0, [x1, w0, uxtw #2]
ffffffff8008e148:   10000061    adr     x1, ffffffff8008e154 <syscall_dispatch+0x50>
ffffffff8008e14c:   8b20c820    add     x0, x1, w0, sxtw #2
ffffffff8008e150:   d61f0000    br      x0                 // O(1) indirect dispatch!
```

Similarly, disassembling `image/shell.elf` confirms that `user/link.ld` placed `_start` (`crt0.o`) right at `0x20000000`, zeroing `.bss` (`0x20004fe0..0x20005020`) before calling `__ulibc_init`, `main` (`0x20000a68`), and `sys_process_exit`:

```text
$ aarch64-linux-gnu-objdump -d image/shell.elf | head -n 24

0000000020000000 <_start>:
    20000000:   aa0003f3    mov     x19, x0
    20000004:   aa0103f4    mov     x20, x1
    20000008:   580001c2    ldr     x2, 20000040 <_start+0x40>   // __bss_start
    2000000c:   580001e3    ldr     x3, 20000048 <_start+0x48>   // __bss_end
    20000010:   eb03005f    cmp     x2, x3
    20000014:   54000062    b.cs    20000020 <_start+0x20>
    20000018:   f800845f    str     xzr, [x2], #8
    2000001c:   17fffffd    b       20000010 <_start+0x10>
    20000020:   aa1303e0    mov     x0, x19
    20000024:   aa1403e1    mov     x1, x20
    20000028:   94000596    bl      20001680 <__ulibc_init>
    2000002c:   9400028f    bl      20000a68 <main>
    20000030:   94000702    bl      20001c38 <sys_process_exit>
```

---

## 17.5 Source-Level Debugging with QEMU (-s -S) and gdb-multiarch

### 17.5.1 Starting QEMU with the GDB Stub Frozen at Reset

QEMU includes a built-in RSP (Remote Serial Protocol) hardware GDB server controlled by two flags:
* **`-s`**: Shorthand for `-gdb tcp::1234`, opening a GDB server on TCP port `1234`.
* **`-S`**: Freezes all 4 vCPUs at reset **before** fetching the very first instruction at `0x40080000`.

In **Terminal 1**, start QEMU paused under GDB control from `lesson_20/`:

```bash
qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 \
    -kernel image/kernel.img -initrd image/bootfs.tar \
    -nographic -serial mon:stdio \
    -drive file=image/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0 \
    -s -S
```

### 17.5.2 `.gdbinit` Recipe for Simultaneous `EL1` + `EL0` Symbol Debugging

In ABOS Lesson 20, `kernel.elf` lives at `0x40080000` (`.idmap`) and `0xffffffff80084000+` (`TTBR1_EL1`), completely disjoint from `EL0` user space (`0x20000000`). Therefore, GDB can hold **`kernel.elf` symbols and an `EL0` executable's symbols loaded at the same time**!

What if you want to switch your `EL0` symbol view from `shell.elf` (`0x20000000`) to `console-srv.elf` (`0x20000000`)? Because both link `.text` at `0x20000000` (distinguished at runtime by the ASID in `TTBR0_EL1[63:48]`), you can either load both with `add-symbol-file` or define tiny GDB helper macros (`sym-shell`, `sym-console`, `sym-fs`, `sym-calc`) to swap the `0x20000000` overlay cleanly!

Save the following recipe (e.g., in `~/.gdbinit-abos` and launch `gdb-multiarch -x ~/.gdbinit-abos`):

```gdb
# ~/.gdbinit-abos — Source-level EL1 + EL0 debugging recipe for ABOS Lesson 20
set confirm off
set pagination off
set architecture aarch64

# 1. Load EL1 higher-half microkernel + .idmap + userboot DWARF symbols
symbol-file image/kernel.elf

# 2. Overlay standalone EL0 symbols at VA 0x20000000
#    (Load shell.elf initially; or load console-srv.elf when debugging UART/VT!)
add-symbol-file image/shell.elf 0x20000000

# Convenience helpers to switch which EL0 binary owns VA 0x20000000 in GDB:
define sym-console
    remove-symbol-file -a 0x20000000
    add-symbol-file image/console-srv.elf 0x20000000
end
define sym-fs
    remove-symbol-file -a 0x20000000
    add-symbol-file image/fs-srv.elf 0x20000000
end
define sym-shell
    remove-symbol-file -a 0x20000000
    add-symbol-file image/shell.elf 0x20000000
end
define sym-calc
    remove-symbol-file -a 0x20000000
    add-symbol-file image/calc.elf 0x20000000
end

# 3. Connect to QEMU's RSP stub (CPU 0 halted at PC = 0x40080000)
target remote localhost:1234
```
*(Note: on macOS Homebrew, use `aarch64-elf-gdb` in place of `gdb-multiarch`.)*

### 17.5.3 Step-by-Step GDB Walkthrough Across `EL1` and `EL0`

**Rule of thumb — Hardware Breakpoints (`hbreak`) vs. Software Breakpoints (`break`)**:
Before the MMU is enabled (or before a demand-paged `EL0` VMO page at `0x20000000` is faulted into RAM!), GDB cannot write a software `brk #0` instruction into an unmapped virtual address. Always use **`hbreak`** (ARMv8 hardware debug breakpoint comparators) when breaking across MMU enablement or `EL0` process creation!

```text
(gdb) # CPU 0 is halted at _start (0x40080000) with x0 = FDT physical address!
(gdb) p/x $pc
$1 = 0x40080000
(gdb) p/x $x0
$2 = 0x44200000

(gdb) # Set hardware breakpoints across the entire boot & EL0 transition pipeline:
(gdb) hbreak high_kernel_entry
Hardware assisted breakpoint 1 at 0xffffffff80096320: file start.S, line 145.
(gdb) hbreak lk_main
Hardware assisted breakpoint 2 at 0xffffffff80084bc8: file main.c, line 50.
(gdb) hbreak syscall_dispatch
Hardware assisted breakpoint 3 at 0xffffffff8008e104: file syscalls.c, line 1048.
(gdb) hbreak *0x20000000
Hardware assisted breakpoint 4 at 0x20000000: file crt0.S, line 18.

(gdb) # 1. Continue across SCTLR_EL1.M enablement into Higher-Half VA!
(gdb) continue
Continuing.
Thread 1 hit Breakpoint 1, high_kernel_entry () at start.S:145
145         ldr     x0, =arm64_el1_exception_base

(gdb) # Inspect MMU system registers right after jumping to 0xffffffff80096320:
(gdb) p/x $sctlr_el1
$3 = 0xc5183d
(gdb) p/x $tcr_el1
$4 = 0x10b5213521
(gdb) p/x $ttbr1_el1
$5 = 0x40083000

(gdb) # 2. Continue into C kernel entry lk_main(dtb_paddr):
(gdb) continue
Thread 1 hit Breakpoint 2, lk_main (dtb_paddr=1142947840) at main.c:50
50      void lk_main(uint64_t dtb_paddr) {

(gdb) # 3. Remove early boot breakpoints, set a breakpoint on shell's main(), and run!
(gdb) delete 1 2 3
(gdb) hbreak *0x20000a68
Hardware assisted breakpoint 5 at 0x20000a68: file shell.c, line 212.
(gdb) continue
```

When GDB hits `*0x20000000` (`_start` in `crt0.S`), how do you know *which* `EL0` process (`console-srv`, `fs-srv`, or `abos-shell-0..3`) is currently running at `0x20000000`? Inspect **`$ttbr0_el1`** (where bits `[63:48]` hold the hardware ASID!) or check `current_threads` in the kernel:

```text
Thread 1 hit Breakpoint 4, _start () at crt0.S:18
18          mov     x19, x0

(gdb) # Extract the 16-bit ASID from TTBR0_EL1[63:48]:
(gdb) p/u ($ttbr0_el1 >> 48) & 0xffff
$6 = 2
(gdb) # ASID 2 is console-srv! Swap symbol overlay to console-srv.elf:
(gdb) sym-console
(gdb) list main
```

### 17.5.4 Inspecting All 4 SMP Cores and Fault Syndrome Registers in GDB

Because QEMU `-smp 4` exposes each Cortex-A53 core as a separate GDB thread (`Thread 1` = CPU 0 .. `Thread 4` = CPU 3), you can inspect per-CPU scheduler state and freeze other cores while single-stepping:

```gdb
# List what all 4 Cortex-A53 cores are executing simultaneously:
(gdb) info threads
  Id   Target Id                    Frame
* 1    Thread 1.1 (CPU#0 [running]) syscall_dispatch (frame=0xffffffff8013bfc0) at syscalls.c:1049
  2    Thread 1.2 (CPU#1 [halted ]) thread_become_idle () at thread.c:236
  3    Thread 1.3 (CPU#2 [running]) 0x00000000200018a0 in sys_object_wait_one ()
  4    Thread 1.4 (CPU#3 [halted ]) thread_become_idle () at thread.c:236

# Lock QEMU to single-step ONLY the current CPU core without timer preemption races:
(gdb) set scheduler-locking on
(gdb) next
(gdb) set scheduler-locking off

# Inspect exception syndrome registers inside arm64_sync_exception:
(gdb) p/x $esr_el1
(gdb) p/x ($esr_el1 >> 26) & 0x3f     # EC (Exception Class: 0x15=SVC, 0x20=IAbort, 0x24=DAbort)
(gdb) p/x $far_el1                    # Faulting Virtual Address on page/permission faults
(gdb) p/x $elr_el1                    # Saved EL0 instruction address to return to on eret
(gdb) p/x $spsr_el1                   # Saved PSTATE (0x0 = EL0t with IRQ/FIQ unmasked)
```

---

## 17.6 Hands-On Kernel & User-Space Hacking Exercises

The best way to master ABOS is to extend it yourself. Below are three progressive laboratory exercises that walk you through adding a new system call, building a new disk-resident user application, and testing hardware-enforced microkernel fault isolation.

---

### Exercise 17-1: Adding System Call 35 (`SYS_CLOCK_GET_MONOTONIC`)

**Goal**: Expose the kernel's monotonic nanosecond clock (`current_time_ns()` in `kernel/dev/timer.c`) as ABOS System Call **35** (`SYS_CLOCK_GET_MONOTONIC`), wrap it in `libulibc.a`, and add an `uptime` command to `abos-shell`.

1. **Assign syscall number `35` in `include/syscalls.h`** (right after `SYS_VMAR_UNMAP` on line 44):
   ```c
   #define SYS_VMAR_UNMAP          34
   #define SYS_CLOCK_GET_MONOTONIC 35
   ```
2. **Implement the `EL1` handler and dispatch case in `kernel/core/syscalls.c`**:
   Include `"timer.h"` at the top of `syscalls.c`, add a user-pointer-validated handler before `syscall_dispatch`, and wire `case SYS_CLOCK_GET_MONOTONIC:` into the `switch`:
   ```c
   #include "timer.h"

   static ab_status_t sys_clock_get_monotonic(ab_time_t *user_out) {
       if (!user_out) return AB_ERR_INVALID_ARGS;
       ab_time_t now = current_time_ns();
       return copy_to_user(user_out, &now, sizeof(now));
   }
   ```
   Inside `syscall_dispatch(iframe_t *frame)`:
   ```c
           case SYS_CLOCK_GET_MONOTONIC:
               ret = sys_clock_get_monotonic((ab_time_t *)frame->r[0]);
               break;
   ```
3. **Add the `EL0` wrapper to `user/ulibc/ulibc.h` and `user/ulibc/ulibc.c`**:
   * In `user/ulibc/ulibc.h`:
     ```c
     ab_status_t sys_clock_get_monotonic(ab_time_t *out_ns);
     ```
   * In `user/ulibc/ulibc.c`:
     ```c
     ab_status_t sys_clock_get_monotonic(ab_time_t *out_ns) {
         return __ab_syscall4((uintptr_t)out_ns, 0, 0, 0, SYS_CLOCK_GET_MONOTONIC);
     }
     ```
4. **Add an `uptime` command in `user/shell/shell.c`**:
   ```c
   } else if (u_strcmp(line, "uptime") == 0) {
       ab_time_t k_ns = 0;
       sys_clock_get_monotonic(&k_ns);
       ab_time_t u_ns = ulibc_clock_get_monotonic();
       con_printf(con_ch, "Monotonic clock (EL1 syscall 35): %lu ms (%lu ns)\n",
                  k_ns / 1000000ULL, k_ns);
       con_printf(con_ch, "Monotonic clock (EL0 vDSO-style): %lu ms (%lu ns)\n",
                  u_ns / 1000000ULL, u_ns);
   ```
5. **Rebuild, run, and reflect**: Run `make run` and type `uptime`. Notice *why* real Zircon implements `zx_clock_get_monotonic()` via a user-space vDSO reading `cntpct_el0` directly (just like `ulibc_clock_get_monotonic()`, enabled by `msr cntkctl_el1, #1` in `timer.c` line 24) rather than trapping into `EL1` via `svc #0`! Measure the nanosecond delta `u_ns - k_ns` to observe the exact cost of an `EL0 <-> EL1` round-trip!

---

### Exercise 17-2: Writing and Packing `hello.bin` onto `disk.img`

**Goal**: Create a brand-new standalone `EL0` application `user/apps/hello.c`, watch the build system auto-pack `hello.bin` onto `disk.img` with zero `Makefile` changes, and extend `shell.c` to spawn any `<name>.bin` from the VirtIO-Blk TAR filesystem.

1. **Create `user/apps/hello.c`**:
   ```c
   #include "ulibc.h"

   int main(void) {
       ab_handle_t log_h = ulibc_get_startup_handle_by_type(AB_PA_LOG);
       ab_handle_t con_h = ulibc_get_startup_handle_by_type(AB_PA_CONSOLE);
       ab_handle_t res_h = ulibc_get_startup_handle_by_type(AB_PA_RESOURCE);

       printf("\nHello from standalone EL0 app @ main=%p!\n", (void *)main);
       printf("  AB_PA_LOG handle      = 0x%x\n", log_h);
       printf("  AB_PA_CONSOLE handle  = 0x%x\n", con_h);
       printf("  AB_PA_RESOURCE handle = 0x%x (0 = isolated!)\n", res_h);

       char name[64];
       printf("What is your name? ");
       u_gets(name, sizeof(name));
       printf("Welcome to ABOS Microkernel hacking, %s!\n\n", name);
       dlog_printf("[hello.bin] Greeted '%s' successfully!", name);
       return 0;
   }
   ```
2. **Build and verify auto-packaging**:
   Run `make` and `tar -tvf image/disk.img`—without touching any `Makefile` or Python script, `user/apps/Makefile` compiles `image/hello.elf` and `image/hello.bin`, and `image/make_tar_disk.py` packs `hello.bin` alongside `calc.bin`!
3. **Generalize `shell.c` to execute any `<cmd>.bin` on `disk.img`**:
   Factor lines 357–428 of `user/shell/shell.c` (the `calc.bin` VMO-fetch + `sys_process_create` + `sys_vmar_map` at `0x10000000` offset -> VA `0x20000000` + `sys_process_start` sequence) into a helper function:
   ```c
   static int spawn_disk_app(ab_handle_t con_ch, ab_handle_t fs_ch,
                             ab_handle_t job_h, ab_handle_t log_h,
                             const char *bin_name);
   ```
   Call `spawn_disk_app(...)` in the `else` branch of `shell.c` (appending `".bin"` if omitted). Reboot with `make run`, type `ls` (confirming `- hello.bin` appears!), and type `hello` to run your new process at `0x20000000`!

---

### Exercise 17-3: Testing `EL0` Fault Isolation with `crash.bin`

**Goal**: In a monolithic kernel (or single-address-space RTOS), a wild pointer write or privileged instruction inside a component crashes the entire machine (`[PANIC]`). In ABOS, prove that an unmapped pointer write or privileged `EL1` register read inside `crash.bin` is cleanly caught by `arm64_sync_exception()`, terminates *only* `crash.bin`, reclaims all its handles and pages, and returns control to `abos-shell` unharmed!

1. **Create `user/apps/crash.c`**:
   ```c
   #include "ulibc.h"

   int main(void) {
       printf("\n[crash.bin] Preparing to write to unmapped VA 0xDEADB000...\n");
       dlog_printf("[crash.bin] Triggering intentional EL0 Data Abort!");

       volatile uint64_t *bad_ptr = (volatile uint64_t *)0x00000000DEADB000ULL;
       *bad_ptr = 0xCAFEBABEULL;

       printf("[crash.bin] ERROR: You should never see this line!\n");
       return 0;
   }
   ```
2. **Run `crash` from `abos[tty0]$ `** (using your generic `spawn_disk_app` loader from Exercise 17-2) and observe the kernel diagnostic output:
   ```text
   abos[tty0]$ crash
   [debuglog cpu=1 proc='crash.bin' koid=1025] [crash.bin] Triggering intentional EL0 Data Abort!

   [EXCEPTION] Terminating faulting EL0 process 'crash.bin' (koid=1105) on CPU 1!
     ESR=0x96000045 (EC=0x24, ISS=0x45) FAR=0x00000000deadb000
   iframe 0xffffffff80143ef0 (CPU 1):
    x0  0x0000000000000000  x1  0x00000000deadb000  x2  0x00000000cafebabe ...
    elr 0x0000000020000084  spsr 0x0  usp 0x0000000000805fb0
   [process 'crash.bin' koid=1105] exiting with code -1; closing remaining handles
   abos[tty0]$ 
   ```
3. **Decode the hardware fault syndrome** (using Appendix A3):
   * `ESR = 0x96000045`:
     * `EC = (0x96000045 >> 26) & 0x3F = 0x24` (**Data Abort from a lower Exception Level, `EL0`**).
     * `IL = 1` (bit 25: 32-bit AArch64 instruction).
     * `ISS = 0x0000045`: Bit `6` (`WnR = 1`, **Write** fault) and `DFSC = 0x05` (**Translation Fault, Level 1**, because `0xdeadb000 >= 0x80000000` exceeds `T0SZ=33`'s 2 GB user VA limit!).
   * `arm64_sync_exception()` (`exceptions_c.c` lines 37–60) calls `ab_process_handle_page_fault()`, finds no VMAR mapping covering `FAR = 0xdeadb000`, sees `ARM64_EXCEPTION_FLAG_LOWER_EL`, logs `[EXCEPTION]`, and calls `ab_process_exit_current(-1)`.
   * Run `ps -T` immediately afterward: `crash.bin`'s VMO pages, user stack, ASID, and duplicated console/log handles have all been reclaimed with zero memory or handle leaks, and all 4 virtual terminals continue running normally!
   * **Bonus experiment**: Replace `*bad_ptr = ...` with `__asm__ volatile("mrs x0, ttbr1_el1");` in `crash.c` and observe `EC = 0x18` (Trapped `MSR`/`MRS` system register access from `EL0`)!

---

## 17.7 Summary

In this chapter, we bridged static source code and dynamic runtime behavior across ABOS Lesson 20:
1. We mapped every QEMU `virt` flag (`-M virt -cpu cortex-a53 -smp 4 -kernel -initrd -nographic -serial mon:stdio -drive -device virtio-blk-device`) to physical addresses, FDT boot handoff rules, and QEMU Monitor (`Ctrl+A c`) diagnostics.
2. We traced the 4-core SMP boot sequence line-by-line from `start.S` and `lk_main` through `PSCI_CPU_ON`, `BOOTFS` VMO creation, and concurrent scheduling of `userboot`, `console-srv`, `fs-srv`, and `abos-shell-0..3`.
3. We used `caps`, `ps -T`, `ps -J`, and `vmaps all` to verify capability isolation, handle encoding, proportional memory accounting, and ASID-isolated virtual memory at `0x20000000`.
4. We inspected ELF symbol tables, compiler jump tables, and `gdb-multiarch` multi-core / multi-address-space debugging sessions, culminating in three hands-on kernel and user-space extensions.
