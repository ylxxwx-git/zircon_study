# Inside ABOS (Lesson 20): A Zircon-Style AArch64 Microkernel Study Book

> [!TIP]
> **Looking for the Complete Line-by-Line Annotated Book?**  
> For the full-length book modeled after *A Heavily Commented Linux Kernel Source Code* — covering ARMv8-A architecture foundations plus a **100% verbatim, line-numbered commentary on all 58 files (9,526 lines)** in `lesson_20/` — see **[`book/ABOS_Book.md`](../book/ABOS_Book.md)** and the typeset A4 PDF **[`book/ABOS_Book.pdf`](../book/ABOS_Book.pdf)** (also browse individual chapters in [`book/chapters/`](../book/chapters/)).

Welcome to the architectural study guide for **ABOS (Lesson 20)**—a capability-based, Zircon-inspired Symmetric Multiprocessing (SMP) microkernel running on 64-bit ARM (**AArch64**).

In **Lesson 20**, ABOS achieves true microkernel separation:
- **Kernel Image ([`image/kernel.img`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/image/Makefile#L46-L48))**: Contains only the `EL1` microkernel and the tiny `EL0` bootstrap loader ([`userboot`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/userboot/userboot.c)).
- **Boot RAM Disk ([`image/bootfs.tar`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/image/Makefile#L50-L52))**: Passed to QEMU via `-initrd`, containing standalone `EL0` system servers ([`console-srv.bin`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/console_srv.c), [`fs-srv.bin`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/fs_srv.c)) and the interactive shell ([`shell.bin`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/shell/shell.c)), all linked at virtual address `0x20000000`.
- **VirtIO Block Disk ([`image/disk.img`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/image/Makefile#L54-L56))**: Attached via VirtIO-Blk DMA, containing user data files (`motd.txt`, `zircon.txt`) and standalone user applications ([`calc.bin`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/apps/calc.c)).

---

# Chapter 1: The QEMU `virt` Machine, Memory Map, Address Spaces & Stacks

## 1.1 The QEMU `virt` Hardware Platform & Exception Levels

When you run `make run` in [`Makefile`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/Makefile#L53-L61), QEMU launches a 4-core ARMv8-A virtual machine:

```bash
qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 \
    -kernel image/kernel.img \
    -initrd image/bootfs.tar \
    -nographic -serial mon:stdio \
    -drive file=image/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0
```

### ARMv8-A Exception Levels (`EL0` – `EL3`)
ARMv8-A defines four hardware privilege rings called **Exception Levels**:

| Exception Level | Privilege | Typical Software | Role in ABOS QEMU Setup |
| :--- | :--- | :--- | :--- |
| **`EL3`** | Highest (Secure Monitor) | ARM Trusted Firmware (`TF-A`) | Emulated internally by QEMU's built-in PSCI/SMC handler (`virtualization=off`, `secure=off`). |
| **`EL2`** | Hypervisor | KVM / Xen / Hafnium | Skipped by default when `-M virt` runs without `virtualization=on`. (If entered at `EL2`, [`start.S`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/start.S#L21-L42) configures `HCR_EL2.RW=1` and drops to `EL1h` via `eret`). |
| **`EL1`** | OS Kernel | **ABOS Microkernel** | Executes with `MMU` and I/D caches enabled using `TTBR1_EL1` (`0xFFFFFFFF80000000+`). |
| **`EL0`** | Unprivileged User Space | **`userboot`, `console-srv`, `fs-srv`, `shell`, `calc`** | Executes with `MMU` and I/D caches enabled using per-process `TTBR0_EL1` (`0x0000000000000000..0x7FFFFFFF`). |

When QEMU's built-in ARM64 Linux bootloader starts with `-kernel image/kernel.img -initrd image/bootfs.tar`:
1. **CPU 0** starts executing at physical address **`0x40080000`** ([`_start`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/start.S#L17)) in **`EL1h`** with the MMU off and register **`x0`** holding the physical address of the **Flattened Device Tree (DTB)** blob.
2. **CPUs 1, 2, and 3** are held powered-off in QEMU's PSCI emulation until CPU 0 wakes them via `HVC #0` (`PSCI_CPU_ON = 0xC4000003`, [`psci_cpu_on()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/main.c#L30-L37)).

---

## 1.2 Physical Memory & MMIO I/O Address Map

On the QEMU `virt` board, physical addresses below `0x40000000` (`0 .. 1 GB`) are reserved for **Memory-Mapped I/O (MMIO)** peripherals, and physical RAM begins at **`0x40000000`** (`1 GB` mark).

```
Physical Address (PA)
0x4800_0000 +-----------------------------------------------------------+  End of 128 MB RAM
            |  Flattened Device Tree (DTB) (~1 MB aligned near top)     |  <- Wired in PMM
            |  QEMU -initrd (bootfs.tar)   (Copied to VMO & reclaimed!) |  <- Freed to PMM after boot
            |-----------------------------------------------------------|
            |                                                           |
            |  Dynamic Physical Memory Pool (Managed by PMM)            |
            |  - Kernel Heap (128 pages = 512 KB contiguous)            |
            |  - Per-Process L1/L2/L3 Page Tables (VM_PAGE_STATE_MMU)   |
            |  - Per-Thread 8 KB Kernel Stacks (VM_PAGE_STATE_ALLOC)    |
            |  - VMO Data Pages & EL0 User Stacks (VM_PAGE_STATE_OBJECT)|
            |  - VirtIO-Blk 128 KB Contiguous DMA Ring & Cache          |
            |                                                           |
     _end ->|-----------------------------------------------------------|
            |  Kernel .text, .rodata, .data, .bss, Per-CPU Boot Stacks  |  <- Wired in PMM
0x4008_3000 |-----------------------------------------------------------|
            |  .idmap (start.S, idmap_table, kernel_table) [12 KB]      |  <- Wired in PMM
0x4008_0000 |-----------------------------------------------------------|  Kernel Load Address
            |  Reserved (512 KB below kernel entry)                     |  <- Wired in PMM
0x4000_0000 +===========================================================+  Start of 128 MB RAM
            |                     ... Unmapped ...                      |
0x0A00_3FFF |-----------------------------------------------------------|
            |  VirtIO-MMIO Transports (32 slots * 0x200, SPI IRQs 48+)  |  <- Mapped into fs-srv (EL0)
0x0A00_0000 |-----------------------------------------------------------|
            |  PL011 UART0 Registers (4 KB, SPI IRQ 33)                 |  <- Mapped into console-srv (EL0)
0x0900_0000 |-----------------------------------------------------------|
            |  GICv2 CPU Interface (GICC)                               |  <- Kernel Periphmap (EL1)
0x0801_0000 |-----------------------------------------------------------|
            |  GICv2 Distributor (GICD)                                 |  <- Kernel Periphmap (EL1)
0x0800_0000 +-----------------------------------------------------------+
```

### Summary of Physical Addresses

| Physical Range | Size | Hardware / Component | Accessed By |
| :--- | :--- | :--- | :--- |
| `0x08000000 .. 0x0800FFFF` | 64 KB | **GICv2 Distributor (`GICD`)** | `EL1` Kernel ([`gic.h`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/gic.h#L10)) |
| `0x08010000 .. 0x0801FFFF` | 64 KB | **GICv2 CPU Interface (`GICC`)** | `EL1` Kernel ([`gic.h`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/gic.h#L11)) |
| `0x09000000 .. 0x09000FFF` | 4 KB | **PL011 UART0** (IRQ 33) | Early `EL1` debuglog **+** `EL0` [`console-srv`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/console_srv.c#L17) via physical VMO |
| `0x0A000000 .. 0x0A003FFF` | 16 KB | **VirtIO-MMIO Bus** (32 slots, IRQs 48..79) | `EL0` [`fs-srv`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/fs_srv.c#L12) via physical VMO |
| `0x40000000 .. 0x4007FFFF` | 512 KB | Low RAM below kernel load address | Marked `WIRED` in [`pmm_init()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/pmm.c#L38-L76) |
| `0x40080000 .. 0x40082FFF` | 12 KB | **`.idmap` Boot Trampoline** (`start.S`, `idmap_table`, `kernel_table`) | Executed with MMU off & Non-Global identity-mapped during MMU enable |
| `0x40083000 .. _end` | ~116 KB | **Kernel Image** (`.text`, `.rodata`, `.data`, `.bss`, 32 KB boot stacks) | `EL1` Kernel via Higher-Half VA `0xFFFFFFFF80083000+` |
| `_end .. 0x47FFFFFF` | ~127 MB | **PMM Page Arena** (`page_array[32768]`) | Allocated on demand as 4 KB frames (`PAGE_SIZE = 4096`) |
| `initrd_start .. initrd_end` | ~60 KB | **BOOTFS Archive (`image/bootfs.tar`)** | Discovered via [`dtb_parse()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/dtb.c#L41-L140), copied into `bootfs.tar` VMO, then freed via [`pmm_free_wired_range()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/pmm.c#L175-L196) |
| `dtb_paddr .. +dtb_size` | ~1 MB | **Flattened Device Tree (DTB)** | Loaded by QEMU (`~0x44100000`), wired in PMM |

---

## 1.3 Virtual Address Space Architecture (`TTBR1_EL1` vs. `TTBR0_EL1`)

AArch64 provides two independent page table base registers so the kernel and user processes never collide and never require expensive kernel page-table switches on system calls:
- **`TTBR1_EL1` (Higher-Half Kernel Space)**: Shared globally across all CPUs and processes (`PTE_NG = 0`), accessible **only in `EL1`**.
- **`TTBR0_EL1` (Lower-Half User Space)**: Private to each `EL0` process (`vm_aspace_t`), tagged with an allocated hardware **ASID** (`1..255`, `PTE_NG = 1`) via [`ab_alloc_asid()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/mmu.c#L50-L63) so TLB entries from different processes coexist without context-switch TLB flushes.

In [`start.S`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/start.S#L9-L100), `TCR_EL1` is programmed from base `0x00000000B5213521` (plus `TCR_EL1.AS = 1` when 16-bit ASIDs are supported), which configures:
- **`T0SZ = 33`** and **`T1SZ = 33`**: Each address space spans $2^{64 - 33} = 2^{31} = \text{2 GB}$.
- **4 KB Granule (`TG0 = 4KB`, `TG1 = 4KB`)**: Page translation starts at **Level 1** (`L1 -> L2 -> L3`).

### 1. Higher-Half Kernel Virtual Address Space (`TTBR1_EL1`)

Defined in [`include/mmu.h`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/mmu.h#L18-L37) and [`start.S`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/start.S#L64-L73), `kernel_table` maps two 1 GB blocks at Level 1:

$$\text{Kernel Physmap VA} = \text{PA} + \text{0xFFFFFFFF40000000}$$
$$\text{Kernel Periphmap VA} = \text{PA} + \text{0xFFFFFFFFC0000000}$$

| Kernel Virtual Address (`TTBR1_EL1`) | Physical Address | Memory Type | Contents |
| :--- | :--- | :--- | :--- |
| `0xFFFFFFFF80000000 .. 0xFFFFFFFF87FFFFFF` | `0x40000000 .. 0x47FFFFFF` | Normal WBWA (`Attr1`) | **128 MB Linear Physmap**: Every byte of physical RAM is directly addressable by the kernel via [`paddr_to_physmap(pa)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/mmu.h#L27-L29). |
| `0xFFFFFFFF80083000 .. _end` | `0x40083000 .. _end_pa` | Normal WBWA (`Attr1`) | **Kernel `.text`, `.rodata`, `.data`, `.bss`, and Boot Stacks** ([`linker.ld`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/linker.ld#L25-L64)). |
| `0xFFFFFFFFC8000000` | `0x08000000` | Device-nGnRnE (`Attr0`) | **GICv2 Distributor (`GICD_BASE`)** ([`gic.h`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/gic.h#L10)). |
| `0xFFFFFFFFC8010000` | `0x08010000` | Device-nGnRnE (`Attr0`) | **GICv2 CPU Interface (`GICC_BASE`)** ([`gic.h`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/gic.h#L11)). |
| `0xFFFFFFFFC9000000` | `0x09000000` | Device-nGnRnE (`Attr0`) | **Early Kernel DebugLog UART0** (`dprintf`). |

### 2. Lower-Half Per-Process User Virtual Address Space (`TTBR0_EL1`)

Every `EL0` process (`userboot`, `console-srv`, `fs-srv`, `abos-shell-0..3`, `calc.bin`) gets its own [`vm_aspace_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/mmu.h#L66-L71) covering `0x0000000000001000 .. 0x000000007FFFFFFF`:

```
User Virtual Address (TTBR0_EL1, per-process)
0x0000_0000_8000_0000 +-------------------------------------------------------+  Top of 2 GB User Space
                      |                      Unmapped                         |
0x0000_0000_4000_0000 +-------------------------------------------------------+  End of Root VMAR
                      |                                                       |
                      |                                                       |
0x0000_0000_2000_0000 |  Standalone Executable Image (.text, .rodata, .data,  |  <- USER_APP_BASE (user/link.ld)
                      |  .bss) mapped at VMAR offset 0x10000000               |     console-srv, fs-srv, shell, calc
                      |-------------------------------------------------------|
                      |  Dynamic VMO Mappings (vmar_offset == 0, first-fit):  |
                      |  - userboot   : bootfs.tar (RO, unmapped after boot)  |
                      |  - console-srv: pl011-uart-mmio (RW Device)           |
                      |  - fs-srv     : virtio-blk-mmio, virtio-blk-dma (RW)  |
                      |  - shell      : cat <file> VMO mappings (RO, unmapped)|
0x0000_0000_1000_0000 +-------------------------------------------------------+  Start of Root VMAR (768 MB)
                      |                      Unmapped                         |
0x0000_0000_0080_6000 +-------------------------------------------------------+  Initial SP_EL0 (Stack Top)
                      |  EL0 User Stack (24 KB = 6 pages, RW, grows downward) |
0x0000_0000_0080_0000 +-------------------------------------------------------+  User Stack Base
                      |  (userboot only: embedded code @ 0x00083000..0x90000) |
0x0000_0000_0000_0000 +-------------------------------------------------------+  NULL Guard Page (Unmapped)
```

---

## 1.4 Linker Scripts: `linker.ld` vs. `user/link.ld`

ABOS uses **two** linker scripts to enforce clean separation between `EL1` kernel code and `EL0` user-space executables:

1. **Kernel Linker Script ([`linker.ld`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/linker.ld))**:
   - Places `.idmap` (`start.S`) at physical address `0x40080000`.
   - Advances the virtual location counter (`.`) by `KERNEL_VA_OFF = 0xFFFFFFFF40000000` so `.text`, `.rodata`, `.data`, and `.bss` are linked at **`0xFFFFFFFF80083000+`**, while the `AT(...)` directive keeps their physical load addresses contiguous in RAM starting at `0x40083000`.

2. **Common User-Space Linker Script ([`user/link.ld`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/link.ld))**:
   - Shared by **every** standalone user-space server, shell, and app (`console-srv.bin`, `fs-srv.bin`, `shell.bin`, `calc.bin`).
   - Sets `ENTRY(_start)` and links `.text._start` ([`user/ulibc/crt0.S`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/crt0.S)) at **`USER_APP_BASE = 0x20000000`**, followed by `.text`, `.rodata`, `.data`, `.bss`, and `.app_end`.

---

## 1.5 Complete Guide to Stacks in ABOS

Understanding an OS kernel requires knowing **which stack pointer is active at every moment**. In ABOS, there are **three distinct types of stacks**:

| Stack Type | Exception Level | Register | Virtual Address | Size | Where Allocated | Purpose |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **1. Per-CPU Boot / Idle Stack** | `EL1h` | `SP_EL1` | `0xFFFFFFFF8009xxxx` (`_boot_stacks_bottom .. _stack_top`) | **8 KB × 4 CPUs** (32 KB total) | Statically in kernel `.bss` ([`linker.ld:59-61`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/linker.ld#L59-L61)) | Set up in [`start.S:150-155`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/start.S#L150-L155): $\text{SP} = \text{\_stack\_top} - (\text{cpu\_id} \times 8192)$. Used during boot (`lk_main`, `arm64_secondary_entry`) and becomes each CPU's `idle` thread stack. |
| **2. Per-Thread Kernel Stack** | `EL1h` | `SP_EL1` | `0xFFFFFFFF80xxxxxx` (Kernel Physmap) | **8 KB** (`2 * PAGE_SIZE`) per thread | Dynamically via `pmm_alloc_contiguous(2, VM_PAGE_STATE_ALLOC)` in [`thread_create()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L122-L177); reclaimed by [`thread_reap_dead()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L87-L120) | Every `thread_t` has its own 8 KB `EL1` stack. When an `EL0` thread executes `svc #0` or takes an IRQ, the CPU hardware automatically switches `SP` from `SP_EL0` to `SP_EL1` and pushes the 288-byte [`iframe_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/exceptions.h#L19-L27) onto this stack! |
| **3. Per-Process User Stack** | `EL0t` | `SP_EL0` | `0x0000000000800000 .. 0x0000000000806000` | **24 KB** (`6 * PAGE_SIZE`) per process | Dynamically via `pmm_alloc_page(VM_PAGE_STATE_OBJECT)` in [`user_thread_trampoline()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1509-L1526); freed in [`process_destroy()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L223-L270) | Mapped into `TTBR0_EL1` with `USER | READ | WRITE` (no execute: `PTE_UXN | PTE_PXN`). `SP_EL0` is initialized to `0x00806000` in [`arm64_uspace_entry`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/uspace_entry.S#L14) and grows downward. |

---

# Chapter 2: The Microkernel Design — Bootup, Interrupts, Syscalls & Scheduler

## 2.1 Microkernel Philosophy: Capability-Based Object Model

Modeled after **Google Fuchsia's Zircon kernel**, ABOS follows strict microkernel principles:
1. **Minimal `EL1` TCB**: The kernel contains **no** filesystem code, **no** block device driver, **no** terminal emulator, and **no** shell. It provides only:
   - Virtual memory management (`VmAspace`, `VMO`, `VMAR`)
   - Preemptive SMP thread scheduling (`thread_t`, `wait_queue_t`, `event_t`)
   - Inter-Process Communication (`Channel`, `Port`, `Event`, `EventPair`)
   - Hardware capability brokering (`Resource`, `Interrupt`)
2. **Everything is a Kernel Object ([`ab_object_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L138-L147))**:
   Every resource in the kernel (Jobs, Processes, Threads, VMOs, VMARs, Channels, Ports, Events, EventPairs, Interrupts, Resources, DebugLogs) inherits from `ab_object_t` (Zircon's `Dispatcher`), which embeds:
   - A globally unique 64-bit Kernel Object ID (`koid`, starting at `1024`).
   - Dual reference counts: `ref_count` (kernel pointer lifetime) and `handle_count` (open user-space handles; triggers `on_zero_handles` when it drops to `0`, e.g., asserting `AB_SIGNAL_PEER_CLOSED`).
   - An embedded [`ab_state_tracker_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L118-L123) (`StateTracker`) that tracks active state signals (`READABLE`, `WRITABLE`, `PEER_CLOSED`, `SIGNALED`) and wakes waiting threads or `Port` observers.
3. **No Ambient Authority (Capability Handles)**:
   User-space processes cannot reference kernel objects by pointer or global ID. Instead, each process has a private 64-slot handle table (`proc->handles[]`, [`object.h:207`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L207)). A user-space handle (`ab_handle_t`) is a 32-bit capability token encoded by [`make_handle_value()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L186-L188):
   ```
   Bits [31:28] = 0x5 (Magic tag)
   Bits [23:8]  = 16-bit Generation Counter (incremented on close to catch use-after-close!)
   Bits [7:0]   = Per-process Handle Slot Index (0..63)
   ```
   Each handle entry pairs the `ab_object_t *` pointer with an [`ab_rights_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L68-L83) bitmask (`READ`, `WRITE`, `EXECUTE`, `MAP`, `DUPLICATE`, `TRANSFER`, `WAIT`, `SIGNAL`, `MANAGE_PROCESS`, etc.). Rights can be **attenuated** (reduced) when duplicating a handle, but can **never** be escalated.

---

## 2.2 Step-by-Step SMP Bootup Sequence

```
[QEMU Starts CPU 0 at PA 0x40080000 in EL1h with x0 = dtb_paddr]
       |
       v
1. start.S (_start in .idmap.text, MMU off)
   - Saves x0 (dtb_paddr) in x20; drops EL2 -> EL1h if needed
   - Enables FP/SIMD (CPACR_EL1.FPEN = 0b11)
   - Builds 1GB block page tables in .idmap.data:
     * idmap_table  (TTBR0_EL1, Non-Global): VA 0x00000000->PA 0x0, VA 0x40000000->PA 0x40000000
     * kernel_table (TTBR1_EL1, Global):     VA 0xFFFFFFFF80000000->PA 0x40000000 (Physmap)
                                             VA 0xFFFFFFFFC0000000->PA 0x00000000 (Periphmap)
   - Programs MAIR_EL1, TCR_EL1 (with 16-bit ASID support), TTBR0_EL1, TTBR1_EL1
   - Read-modify-writes SCTLR_EL1 to enable M | C | I (MMU + I/D caches ON!) and branches to high_kernel_entry
       |
       v
2. start.S (high_kernel_entry at Higher-Half VA 0xFFFFFFFF80083xxx)
   - Installs VBAR_EL1 = arm64_el1_exception_base
   - Sets CPU 0 stack pointer (SP = _stack_top)
   - Zeroes [_bss_start .. _bss_end) and calls lk_main(dtb_paddr)
       |
       v
3. kernel/core/main.c (lk_main)
   - dlog_init_early() & thread_init_early() (initializes idle_threads[0..3])
   - dtb_parse(dtb_paddr, &boot_info): validates bounds & depth; reads RAM + /chosen linux,initrd-start/end
   - pmm_init(): initializes page_array[32768]; wires kernel, DTB, and initrd pages
   - heap_init() & mmu_init(): allocates 128 contiguous pages (512 KB) for heap; initializes empty_ttbr0_l1
   - gic_dist_init(), gic_cpu_init(), timer_init_percpu(100) (100 Hz tick on IRQ 30 + CNTKCTL_EL1.EL0PCTEN)
   - Wakes CPUs 1..3 via psci_cpu_on() (HVC #0 -> _start -> arm64_secondary_entry)
   - Creates Root Objects: root_job, root_log, root_res, and bootfs_vmo ("bootfs.tar"), then reclaims initrd pages
   - Creates 'userboot' process, stuffs [job_h, log_h, res_h, ub_vmar_h, bootfs_h (RO)] into ch0
   - Starts 'userboot' in EL0 via ab_process_start_user(..., ch1_h)
```

---

## 2.3 Exception Vector Table & Trap Handling (`exceptions.S`, `exceptions_c.c`)

In AArch64, `VBAR_EL1` points to [`arm64_el1_exception_base`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions.S#L68), a 2 KB-aligned table with 16 entries (4 exception origins × 4 exception types: `Sync`, `IRQ`, `FIQ`, `SError`):

| Vector Offset | Origin | Handler in `exceptions.S` | C Handler in `exceptions_c.c` |
| :--- | :--- | :--- | :--- |
| `0x200` | `EL1h` Synchronous (Kernel fault) | `.Lel1_sync` | [`arm64_sync_exception(iframe, 0, esr)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions_c.c#L22-L68) |
| `0x280` | `EL1h` IRQ (Interrupt while in kernel) | `.Lel1_irq` | [`arm64_irq_exception(iframe, 0)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions_c.c#L70-L86) |
| `0x400` | `EL0` 64-bit Synchronous (`SVC #0` or Page Fault) | `.Lel0_sync` | [`arm64_sync_exception(iframe, LOWER_EL, esr)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions_c.c#L22-L68) |
| `0x480` | `EL0` 64-bit IRQ (Interrupt while in user space) | `.Lel0_irq` | [`arm64_irq_exception(iframe, LOWER_EL)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions_c.c#L70-L86) |

### How `regsave` and `regrestore` Work
1. When an exception or IRQ occurs in `EL0`, the CPU hardware automatically:
   - Saves the user PC into **`ELR_EL1`** and processor state into **`SPSR_EL1`**.
   - Switches privilege to **`EL1h`** and switches the active `sp` register to the current thread's **`SP_EL1`** (its 8 KB kernel stack).
2. The [`regsave`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions.S#L10-L35) macro subtracts `288` bytes (`IFRAME_SIZE`) from `sp` and saves `x0..x30`, `sp_el0`, `elr_el1`, `spsr_el1`, and `mdscr_el1`, forming an [`iframe_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/exceptions.h#L19-L27) struct on the kernel stack.
3. After the C handler returns, [`regrestore`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions.S#L37-L63) pops all registers from `iframe_t` (including any syscall return value written into `iframe->r[0]`) and executes **`eret`**, atomically returning to `EL0`.

### Synchronous Exception Dispatch (`arm64_sync_exception`)
[`arm64_sync_exception()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions_c.c#L22-L68) inspects the Exception Class (`EC = ESR_EL1[31:26]`):
- **`EC == 0x15` (`SVC #0` from AArch64 `EL0`)**: Re-enables interrupts (`arch_enable_ints()`), calls [`syscall_dispatch(iframe)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L1048-L1219), disables interrupts, and returns to `regrestore`.
- **`EC == 0x20` (Instruction Abort from `EL0`) or `EC == 0x24` (Data Abort from `EL0`)**: On Translation Faults (`FSC 0x04..0x07`), reads the faulting virtual address from **`FAR_EL1`** and calls [`ab_process_handle_page_fault()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1281-L1325), enforcing `READ` / `WRITE` / `EXECUTE` permissions on the matching `VMAR` mapping. If valid, the kernel commits a zeroed physical page on the spot, maps it into `TTBR0_EL1`, and resumes the faulting `EL0` instruction transparently (**demand paging**)!
- **Fault Isolation for Unhandled `EL0` Exceptions**: If an `EL0` process dereferences `NULL`, writes to a read-only mapping, or executes an illegal instruction, the kernel logs the fault frame and terminates **only** the offending process via [`ab_process_exit_current(-1)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L972-L1046) without halting the kernel or sibling processes.

---

## 2.4 Hardware Interrupts: GICv2, Generic Timer & User-Space IRQ Forwarding

When an IRQ fires (`arm64_irq_exception()` in [`exceptions_c.c:70-86`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/exceptions_c.c#L70-L86)):
1. **Acknowledge**: Reads `GICC_IAR` ([`gic_acknowledge_interrupt()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/dev/gic.c#L42-L44)) to get the hardware interrupt ID (`irq = iar & 0x3FF`).
2. **Route**:
   - **IRQ 30 (`TIMER_ARCH_PHYS_IRQ`, Per-CPU Physical Timer PPI)**:
     Calls [`timer_irq_handler()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/dev/timer.c#L32-L37), which reloads `CNTP_TVAL_EL0` for the next 10 ms tick (100 Hz) and calls [`thread_timer_tick()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L281-L308) to wake sleeping or timed-wait threads whose `wake_time_ns` has expired and set `need_resched[cpu] = 1`.
   - **Device SPIs (IRQ 33 = PL011 UART0, IRQ 48+ = VirtIO-Blk)**:
     Calls [`ab_interrupt_dispatch_hw_irq(irq)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1721-L1732):
     1. **Masks** `irq` at the GIC Distributor (`gic_disable_interrupt(irq)`) so the level-triggered device line doesn't cause an infinite IRQ storm before the `EL0` driver runs!
     2. Asserts `AB_SIGNAL_SIGNALED` on the bound [`ab_interrupt_dispatcher_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L489-L494) (which immediately enqueues a `Port` packet for `console-srv` or wakes `sys_interrupt_wait()` in `fs-srv`).
3. **End of Interrupt & Preemption**: Writes `iar` to `GICC_EOIR` (`gic_end_interrupt(iar)`) and calls [`thread_preempt()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L252-L258), which yields the CPU if `need_resched[cpu]` is set.

---

## 2.5 System Calls (`SVC #0`) & User-Pointer Validation

All 34 system calls in ABOS ([`include/syscalls.h`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/syscalls.h)) are invoked via [`__ab_syscall4()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/ulibc.c#L5-L17) in `ulibc.c`, passing the syscall number in **`x8`** and up to 4 arguments in **`x0..x3`**. Every user-space pointer passed into the kernel is validated and copied page-by-page via [`copy_from_user()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L54-L85) and [`copy_to_user()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L87-L118), which enforce `USER_ASPACE` bounds, resolve lazy VMAR pages on demand, check `PTE_AP_USER` / `PTE_AP_RO` permissions, and reject Device MMIO pointers.

| Category | Syscall Numbers & Names | Primary Kernel Implementation |
| :--- | :--- | :--- |
| **Handles & Objects** | `1` `SYS_HANDLE_CLOSE`, `2` `SYS_HANDLE_DUPLICATE`, `3` `SYS_OBJECT_GET_INFO`, `31` `SYS_OBJECT_SET_NAME` | [`handle_close()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L372-L403), [`handle_duplicate()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L405-L425) |
| **Debug & Time** | `4` `SYS_DEBUGLOG_WRITE`, `6` `SYS_NANOSLEEP` | [`sys_debuglog_write()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L162-L187), [`thread_sleep_ns()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L260-L268) |
| **Signals & Events** | `7` `SYS_EVENT_CREATE`, `8` `SYS_EVENTPAIR_CREATE`, `9` `SYS_OBJECT_SIGNAL`, `10` `SYS_OBJECT_SIGNAL_PEER`, `11` `SYS_OBJECT_WAIT_ONE` | [`state_tracker_update_signals()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L83-L128), [`state_tracker_wait_one()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L134-L181) |
| **Ports (Async Mux)** | `12` `SYS_PORT_CREATE`, `13` `SYS_PORT_QUEUE`, `14` `SYS_PORT_WAIT`, `15` `SYS_OBJECT_WAIT_ASYNC` | [`ab_port_wait()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L599-L630), [`state_tracker_wait_async()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L632-L680) |
| **Channels (IPC)** | `16` `SYS_CHANNEL_CREATE`, `17` `SYS_CHANNEL_WRITE`, `18` `SYS_CHANNEL_READ` | [`ab_channel_write()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L826-L888), [`ab_channel_read()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L890-L954) |
| **Virtual Memory** | `19` `SYS_VMO_CREATE`, `20` `SYS_VMO_READ`, `21` `SYS_VMO_WRITE`, `22` `SYS_VMAR_MAP`, `34` `SYS_VMAR_UNMAP` | [`ab_vmo_write()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1051-L1077), [`ab_vmar_map()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1127-L1226), [`ab_vmar_unmap()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1228-L1279) |
| **Tasks & Lifecycle** | `5` `SYS_PROCESS_EXIT`, `23` `SYS_PROCESS_CREATE`, `24` `SYS_THREAD_CREATE`, `25` `SYS_PROCESS_START` | [`sys_process_create()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L631-L691), [`ab_process_start_user()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1537-L1591) |
| **Driver DDK Caps** | `26` `SYS_VMO_CREATE_PHYSICAL`, `27` `SYS_VMO_CREATE_CONTIG`, `28` `SYS_INTERRUPT_CREATE`, `29` `SYS_INTERRUPT_WAIT`, `30` `SYS_INTERRUPT_ACK` | [`ab_vmo_create_physical()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1611-L1634), [`ab_interrupt_dispatcher_create()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1684-L1701) |
| **Diagnostics** | `32` `SYS_JOB_GET_PS_INFO`, `33` `SYS_PROCESS_GET_MAPS` | [`ab_job_get_ps_info()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1798-L1908), [`ab_job_get_process_maps()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1930-L2030) |

---

## 2.6 Preemptive SMP Scheduler & Context Switch (`thread.c`, `switch.S`)

[`sched_reschedule_locked()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L195-L243) performs fair round-robin scheduling across all 4 Cortex-A53 cores:
1. Starting after the current thread's index in `thread_table[64]`, it scans for the next `THREAD_READY` thread that is either unpinned (`pinned_cpu == -1`) or pinned to the current CPU.
2. If no ready thread exists, the CPU switches to its per-CPU `idle_threads[cpu]` (which reaps detached dead threads via [`thread_reap_dead()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L87-L120) and executes `wfi` in a loop).
3. If `next->aspace != prev->aspace`, it calls [`vmm_switch_aspace(next->aspace)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/mmu.c#L264-L275) to load `TTBR0_EL1` with `(asid << 48) | ttbr0_phys` (or the zeroed `empty_ttbr0_l1` when switching to a kernel/idle thread)—relying on hardware ASIDs (`PTE_NG = 1`) so **zero TLB flushes** occur during context switches.
4. Finally, [`arm64_context_switch(&prev->kernel_sp, next->kernel_sp)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/switch.S#L11-L42) pushes the 112-byte [`context_switch_frame_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/thread.h#L30-L45) (`tpidr_el0`, `tpidrro_el0`, `x19..x29`, and `x30/lr`) onto `prev`'s kernel stack, swaps `sp` to `next->kernel_sp`, pops `next`'s callee-saved registers, and executes `ret`.

---

# Chapter 3: Memory Management, System Servers, Device Drivers, Console & Shells

## 3.1 Physical & Virtual Memory Management (`PMM`, `VMO`, `VMAR`)

### 1. Physical Memory Manager (`PMM` — [`kernel/mm/pmm.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/pmm.c))
The 128 MB RAM arena (`0x40000000 .. 0x48000000`) is divided into 32,768 pages of 4 KB each, tracked by `static vm_page_t page_array[32768]`. Every physical page is tagged with a Zircon-style state (`FREE`, `WIRED`, `HEAP`, `MMU`, `OBJECT(VMO)`, `IPC`, `ALLOC`). Single pages are allocated in $O(1)$ from an intrusive singly-linked free list (`free_list_head`), and every newly allocated page is immediately zeroed via the kernel physmap (`zero_page_physmap()`).

### 2. Virtual Memory Objects (`VMO` — [`ab_vmo_dispatcher_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L174-L182))
A **VMO** represents a logical array of up to `VMO_MAX_PAGES` (`128` pages = 512 KB) independent of any process's address space. ABOS supports three flavours of VMOs:
- **Anonymous VMO (`sys_vmo_create`)**: Pages can be populated eagerly via `sys_vmo_write()` (used for loading executables and files) or allocated lazily on first `EL0` page fault ([`ab_process_handle_page_fault()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1281-L1325)).
- **Physical MMIO VMO (`sys_vmo_create_physical`)**: Requires the `Resource` capability (`root_res`). Wraps device MMIO physical addresses (`is_physical = 1`) so user-space drivers can map device registers into `EL0` with `PTE_ATTR_DEVICE` (non-cacheable, non-executable).
- **Contiguous DMA VMO (`sys_vmo_create_contiguous`)**: Requires the `Resource` capability. Allocates physically contiguous RAM pages via `pmm_alloc_contiguous()` and returns both the VMO handle and the base physical address (`dma_pa`) so `EL0` drivers can program hardware DMA rings.

### 3. Virtual Memory Address Regions (`VMAR` — [`ab_vmar_dispatcher_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L184-L189))
Every process has a Root VMAR (`0x10000000 .. 0x40000000`) protected by `proc->vmar_lock`. Calling `sys_vmar_map(vmar_h, vmar_offset, vmo_h, vmo_offset, len, perms, &out_va)` and `sys_vmar_unmap(vmar_h, addr, len)`:
- Verifies that `vmar_h` has `AB_RIGHT_MAP` and that `vmo_h` has matching `READ` / `WRITE` / `EXECUTE` capability rights ([`syscalls.c:599-607`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L599-L607)).
- Checks for overlaps via [`vmar_range_overlaps_locked()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1114-L1125); chooses `map_va = vmar->base_va + vmar_offset` (if `vmar_offset != 0`, e.g., `0x10000000 + 0x10000000 = 0x20000000`) or finds the first free 4 KB-guarded gap inside the Root VMAR when `vmar_offset == 0`.
- Installs L3 PTEs in the process's `TTBR0_EL1` page table via [`vmm_map_page()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/mmu.c#L134-L192), cleaning the D-cache to PoU (`dc cvau`) and invalidating the I-cache (`ic ialluis`) via [`sync_icache_page()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/mmu.c#L37-L48) whenever `AB_VM_PERM_EXECUTE` is set.
- `sys_vmar_unmap()` removes the mapping, clears L3 PTEs via [`vmm_unmap_page()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/mm/mmu.c#L194-L230) (issuing targeted single-page `tlbi vae1is` invalidations), and releases the VMO reference.

---

## 3.2 `userboot`: The Root Bootstrap Process ([`user/userboot/userboot.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/userboot/userboot.c))

Just like Zircon's `userboot`, ABOS's `userboot` is the **first `EL0` process** created by the kernel (`lk_main` in [`main.c:106-140`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/main.c#L106-L140)):
1. It reads its 5 bootstrap handles from `boot_ch`: `[root_job_h, root_log_h, root_res_h, ub_vmar_h, bootfs_vmo_h]`.
2. It maps `bootfs_vmo_h` (`image/bootfs.tar`) read-only into its own address space at `0x10000000`.
3. It creates 8 IPC channel pairs (`con_srv_end[0..3] <-> con_cli_end[0..3]` and `fs_srv_end[0..3] <-> fs_cli_end[0..3]`).
4. Using [`spawn_from_bootfs()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/userboot/userboot.c#L40-L105), it locates `"console-srv.bin"`, `"fs-srv.bin"`, and `"shell.bin"` inside the mapped TAR archive, copies each binary into a fresh VMO, maps it at **`0x20000000`**, packs [`ab_proc_args_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/ipc_proto.h#L24-L28) (`AB_PA_*` handle type tags), and starts:
   - **`console-srv`** (given `root_log`, `root_res`, and `con_srv_end[0..3]`)
   - **`fs-srv`** (given `root_log`, `root_res`, and `fs_srv_end[0..3]`)
   - **`abos-shell-0` .. `abos-shell-3`** (given `root_log`, `child_vmar`, `con_cli_end[i]`, `fs_cli_end[i]`, and `root_job`, **without** `root_res`!)
5. Immediately after spawning the servers and shells, `userboot` calls `sys_vmar_unmap()` and `sys_handle_close(bootfs_vmo_h)` so `bootfs.tar`'s VMO pages are freed right away, then waits on `AB_SIGNAL_TASK_TERMINATED` for all shells and servers to exit cleanly before closing its remaining handles.

---

## 3.3 `console-srv`: EL0 UART Driver, Keyboard Parser & 4 Virtual Terminals ([`user/drivers/console_srv.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/console_srv.c))

`console-srv` is a completely unprivileged `EL0` driver and terminal multiplexer:
1. **Hardware Setup in `EL0`**:
   - Calls `sys_vmo_create_physical(res_h, 0x09000000, 4096, &uart_vmo_h)` and maps the PL011 UART registers into its `EL0` VMAR (`uart_va = 0x10000000`).
   - Calls `sys_interrupt_create(res_h, 33, &uart_irq_h)` to claim SPI IRQ 33, enables the 16-byte PL011 hardware FIFOs (`UART_LCR_H_WLEN8 | UART_LCR_H_FEN`), and unmasks both RX FIFO threshold and RX timeout interrupts (`UART_IMSC_RXIM | UART_IMSC_RTIM`).
2. **Single-Threaded `Port` Event Loop**:
   - Registers both `uart_irq_h` (`key = 100`) and all 4 shell channels (`key = 0..3`) with a single Zircon Port (`sys_object_wait_async`), then blocks in `sys_port_wait()`.
3. **4 Virtual Terminals (`tty0 .. tty3`), Per-VT RX Ring & Screen History**:
   - Each VT has its own `vt_state_t` with a 512-byte typeahead ring buffer (`rx_ring`), a 128-byte input line buffer (`line_buf`), and a 1 KB scrolling output history buffer (`screen_buf`).
   - Background VTs can write output concurrently (`CON_OP_WRITE`); `console-srv` appends their output to `vt->screen_buf` and only writes to the physical UART if `vt_idx == active_vt`.
   - When the user switches VTs ([`switch_active_vt()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/console_srv.c#L152-L174)), `console-srv` prints the VT banner, replays `vts[target_vt].screen_buf` + any partially typed `line_buf`, and flushes any queued typeahead via [`process_vt_rx()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/console_srv.c#L120-L150).
4. **Always-Drain Keyboard & Escape Sequence State Machine ([`drain_uart_rx()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/console_srv.c#L215-L282))**:
   - Always drains `UART_DR` whenever IRQ 33 fires (even if the active shell isn't currently waiting in `CON_OP_READ_LINE`), preventing level-triggered IRQ storms and keeping VT hotkeys responsive at all times.
   - **`prefix_mode == 0` (Normal)**:
     - Printable ASCII, Backspace (`\b` / `127`), and Enter (`\r` / `\n`, with `\r\n` deduplication) are queued into `vt_push_rx(&vts[*active_vt], ch)` and processed into lines for `deliver_line_to_vt()`.
     - **Direct VT Hotkeys**:
       - `0x1C` (`Ctrl+4` / `Ctrl+\`) $\rightarrow$ `tty0`
       - `0x1D` (`Ctrl+5` / `Ctrl+]`) $\rightarrow$ `tty1`
       - `0x00` (`Ctrl+2` / `Ctrl+@`) $\rightarrow$ `tty2`
       - `0x1B` (`Ctrl+3` / standalone `ESC`, disambiguated cleanly using a 25 ms `sys_port_wait()` deadline via [`ulibc_clock_get_monotonic()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/ulibc.c#L247-L255)) $\rightarrow$ `tty3`
       - `0x0E` (`Ctrl+N`) $\rightarrow$ cycles to the next VT (`(active_vt + 1) % 4`)
       - `0x14` (`Ctrl+T`) or `'^'` $\rightarrow$ enters `prefix_mode = 1` (next `'0'..'3'` switches VT)
       - `0x1B` followed within 25 ms by another byte $\rightarrow$ enters `prefix_mode = 2` (`ESC` sequence)
   - **`prefix_mode == 2` (After `ESC`)**:
     - `'0'..'3'` (`Alt+0 .. Alt+3`) $\rightarrow$ switches to `tty0..tty3`.
     - `'['` $\rightarrow$ enters `prefix_mode = 3` (CSI sequence).
   - **`prefix_mode == 3` (CSI Sequence)**:
     - Parses `modifyOtherKeys` (`\x1b[27;5;48~`) and kitty (`\x1b[48;5u`) sequences for `Ctrl+0..4`.

---

## 3.4 `fs-srv`: EL0 VirtIO-Blk DMA Driver & TAR Filesystem Server ([`user/drivers/fs_srv.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/fs_srv.c))

`fs-srv` drives the QEMU VirtIO block device (`image/disk.img`) entirely from `EL0`:
1. **VirtIO-MMIO Bus Discovery**:
   Maps 16 KB at `PA 0x0A000000` (`virtio-blk-mmio`) and scans slots `0..31` (`slot * 0x200`) for `MAGIC == 0x74726976` (`"virt"`) and `DEVICE_ID == 2` (Block Device), discovering its IRQ (`48 + slot`) and reading its sector capacity from `VIRTIO_MMIO_CONFIG`.
2. **Contiguous DMA Ring & Buffer Allocation**:
   Calls `sys_vmo_create_contiguous(res_h, 32 * PAGE_SIZE, &dma_vmo, &dma_pa)` and maps the 128 KB DMA region at `dma_uva`:
   - Pages `0..1`: Virtqueue Descriptor Table (`virtq_desc_t[16]`), Available Ring (`virtq_avail_t`), and Used Ring (`QUEUE_PFN = dma_pa >> 12`).
   - Page `2`: `virtio_blk_req_t` header (`type = 0` READ, `sector = 0`) and 1-byte completion status (`*req_status = 0xFF`).
   - Pages `3..31` (116 KB = 232 sectors, clamped to disk capacity): `tar_cache` buffer.
3. **Interrupt-Driven DMA Read**:
   Chains 3 descriptors (`desc[0]` request header $\rightarrow$ `desc[1]` `tar_cache` $\rightarrow$ `desc[2]` status byte), writes `VIRTIO_MMIO_QUEUE_NOTIFY = 0`, blocks in `sys_interrupt_wait(irq_h)` until the DMA transfer completes, acknowledges the interrupt, and verifies `*req_status == 0`.
4. **Multi-Client Port Loop**:
   Multiplexes all 4 shell filesystem channels (`fs_ch[0..3]`) on a Zircon `Port`, serving:
   - **`FS_OP_LIST` (`ls`)**: Walks the 512-byte POSIX USTAR headers in `tar_cache` and returns the formatted file list.
   - **`FS_OP_OPEN_VMO` (`cat` / `calc`)**: Finds the requested file in `tar_cache`, creates a fresh VMO (`sys_vmo_create`), writes the file payload into it (`sys_vmo_write`), and transfers the VMO handle over the shell's channel!

---

## 3.5 `abos-shell-0..3`: Interactive Shells & Introspection ([`user/shell/shell.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/shell/shell.c))

Four instances of `shell.bin` run concurrently on `tty0..tty3`. Each shell retrieves its 5 capabilities (`log_h`, `vmar_h`, `con_ch`, `fs_ch`, `job_h`) by `AB_PA_*` type tag via [`ulibc_get_startup_handle_by_type()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/crt_env.c#L61-L68) and holds **zero** hardware `Resource` or `Interrupt` capabilities.

Built-in shell commands:
- **`ps [-T|-J|help]`**: Calls `sys_job_get_ps_info(job_h, ...)` to display the Zircon-style Job $\rightarrow$ Process $\rightarrow$ Thread tree, memory metrics (`PSS`, `PRIVATE`, `SHARED`), and per-process capability handle counts (`#jb #pr #th #vo #vm #ch #ev #po #ir #rs #lg`).
- **`vmaps [koid|all|help]`**: Calls `sys_process_get_maps(job_h, ...)` to inspect a process's `VmAspace` (`/A`), Root `VMAR` (`/R`), and every `VMO` mapping (`M`) with virtual address range, `rwx` permissions, committed pages, and backing VMO KOID.
- **`caps`**: Calls `sys_object_get_info()` on each of the shell's 5 startup handles to display its KOID, object type, and rights bitmask.
- **`vt <0..3>`**: Sends `CON_OP_SWITCH_VT` to `console-srv` to switch the active virtual terminal.
- **`ls` / `cat <file>`**: Queries `fs-srv` over `fs_ch`; `cat` maps the full `rep.file_size` read-only into the shell's VMAR, prints exactly `rep.file_size` bytes in bounded chunks, unmaps the region via `sys_vmar_unmap()`, and closes `file_vmo_h`.
- **`calc`**: Fetches `calc.bin` as a VMO from `fs-srv` and launches it as a child process at `0x20000000`.
- **`exit`**: Sends shutdown messages to `fs-srv` and `console-srv` and exits.

---

# Chapter 4: Inter-Process Communication (IPC), Signals & Synchronization

## 4.1 The `StateTracker` & Observer Pattern ([`kernel/core/object.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L83-L181))

Every kernel object embeds an `ab_state_tracker_t` containing:
- `signals`: Current active signal bitmask (`AB_SIGNAL_READABLE`, `AB_SIGNAL_WRITABLE`, `AB_SIGNAL_PEER_CLOSED`, `AB_SIGNAL_SIGNALED` / `AB_SIGNAL_TASK_TERMINATED`, `AB_USER_SIGNAL_0..3`).
- `head` / `tail`: Doubly-linked list of registered [`ab_wait_observer_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L106-L115) observers.

Whenever an object's state changes (for example, a message is written to a channel, a hardware IRQ fires, or a process exits), the kernel calls [`state_tracker_update_signals(obj, clear_mask, set_mask)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L83-L128):
1. Updates `st->signals = (old_sigs & ~clear_mask) | set_mask`.
2. Walks the observer list (`st->head`):
   - **Synchronous Observer (`sys_object_wait_one`)**: Sets `obs->wakeup_reasons = new_sigs` and signals `obs->event`, waking the blocked thread!
   - **Asynchronous Port Observer (`sys_object_wait_async`)**: Unlinks the one-shot observer, constructs an [`ab_port_packet_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L289-L297) (`type = AB_PKT_TYPE_SIGNAL_ONE`, `key = obs->key`, `observed = new_sigs`), enqueues it into `obs->port_obj` via `ab_port_queue()`, and frees the observer.

---

## 4.2 Channels & Capability Transfer (`ChannelDispatcher`)

A **Channel** consists of two entangled [`ab_channel_dispatcher_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L341-L348) endpoints (`ch0` and `ch1`) created by `sys_channel_create(0, &h0, &h1)` and synchronized across peer detachment via `g_peer_lock`. Each endpoint holds a FIFO queue of [`ab_message_packet_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L332-L339) packets. Each packet carries:
- Up to **256 bytes** of raw message payload (`data[256]`)
- Up to **8 capability handles** (`objs[8]` + `rights[8]`)

### How Capability Transfer Works Across Processes

```
Sender Process (e.g., fs-srv)               Kernel MessagePacket              Receiver Process (e.g., shell)
+---------------------------+               +------------------+              +----------------------------+
| handles[slot 4]:          |   1. Write    | pkt->objs[0] = --+----+         | handles[slot 5]:           |
|   obj = calc.bin VMO      | ------------> |   calc.bin VMO   |    | 2. Read |   obj = calc.bin VMO       |
|   rights = R|W|X|MAP      |  (detached,   | pkt->rights[0] = | ---+-------> |   rights = R|W|X|MAP       |
|   gen++ (slot now empty!) |   ref & hc    |   R|W|X|MAP      |    |         |   new handle = 0x50000105  |
+---------------------------+   preserved)  +------------------+    |         +----------------------------+
                                                                    v
                                                       [ab_vmo_dispatcher_t]
                                                       (koid=1055, ref_count, handle_count)
```

1. **During `sys_channel_write()` ([`ab_channel_write()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L826-L888))**:
   - For each handle in the message, [`handle_take_for_transfer()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L685-L726) verifies that the handle has **`AB_RIGHT_TRANSFER`** (and is not the channel endpoint itself).
   - It **atomically removes** the entry from the sender process's handle table (incrementing `entry->generation` so the sender can no longer use the old handle value) and stores the raw `ab_object_t *` pointer and `ab_rights_t` inside the kernel `ab_message_packet_t` while keeping `obj->ref_count` and `obj->handle_count` intact.
   - It appends the packet to `peer->msg_tail` and calls `state_tracker_update_signals(&peer->base, 0, AB_SIGNAL_READABLE)` while holding `peer->lock`.
2. **During `sys_channel_read()` ([`ab_channel_read()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L890-L954))**:
   - Dequeues the `ab_message_packet_t` from `self->msg_head` (clearing `AB_SIGNAL_READABLE` if `msg_count` reaches `0`).
   - Calls [`handle_install_transferred()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L728-L750) to allocate free slots in the **receiver process's** handle table (rolling back cleanly if the receiver's handle table fills up) and copies the message bytes and newly minted `ab_handle_t` values to user space!

---

## 4.3 Ports (`PortDispatcher`): Event-Driven Server Multiplexing

A **Port** ([`ab_port_dispatcher_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/object.h#L301-L310)) is Zircon's equivalent of Linux's `epoll` or BSD's `kqueue`. It holds a 32-slot ring buffer of `ab_port_packet_t` packets and an auto-unsignaling `readable_event`.
- Servers (`console-srv` and `fs-srv`) bind multiple channels and interrupt objects to one Port using `sys_object_wait_async(obj_h, port_h, key, watched_signals)`.
- If the watched signals are **already active** when `sys_object_wait_async` is called, [`state_tracker_wait_async()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L632-L680) immediately enqueues a packet so no edge is ever lost.
- Because `sys_object_wait_async` is **one-shot**, a server processes all available messages/characters on the triggered `key` and then re-arms `sys_object_wait_async` for the next event.

---

# Chapter 5: End-to-End Walkthrough — How an Application (`calc.bin`) is Launched

Let's trace the complete lifecycle of **`calc.bin`**—from source code to execution and termination—when you type `calc` at the `abos[tty0]$ ` prompt.

```
abos-shell-0 (EL0)                  fs-srv (EL0)                   ABOS Kernel (EL1)               calc.bin (EL0)
        |                                |                                 |                             |
        | 1. FS_OP_OPEN_VMO ("calc.bin") |                                 |                             |
        |------------------------------->|                                 |                             |
        |                                | 2. sys_vmo_create & vmo_write   |                             |
        |                                |-------------------------------->| (allocates PA pages &       |
        |                                |                                 |  copies calc.bin bytes)     |
        | 3. Reply + bin_vmo_h handle    |                                 |                             |
        |<-------------------------------|                                 |                             |
        |                                                                  |                             |
        | 4. sys_process_create(job_h, "calc.bin", &calc_proc, &calc_vmar) |                             |
        |----------------------------------------------------------------->| (allocates ASID & TTBR0 L1) |
        |                                                                  |                             |
        | 5. sys_vmar_map(calc_vmar, 0x10000000, bin_vmo_h, RWX)           |                             |
        |----------------------------------------------------------------->| (maps VA 0x20000000 -> PA)  |
        |                                                                  |                             |
        | 6. sys_channel_create(&p_ch, &c_ch)                              |                             |
        |    sys_channel_write(p_ch, &pargs, [dup_log, dup_con])           |                             |
        |    sys_process_start(calc_proc, calc_thr, 0x20000000, 0, c_ch, 0)|                             |
        |----------------------------------------------------------------->| (allocates 24KB stack @     |
        |                                                                  |  0x00800000, transfers c_ch,|
        | 7. sys_object_wait_one(calc_proc, AB_SIGNAL_TASK_TERMINATED)     |  eret to EL0 @ 0x20000000)  |
        |----------------------------------------------------------------->|---------------------------->|
        |                           (Shell sleeps)                         |                             | 8. _start (crt0.S):
        |                                                                  |                             |    - zeroes .bss
        |                                                                  |                             |    - __ulibc_init(c_ch)
        |                                                                  |                             |    - calls main()
        |                                                                  |                             |    - printf / u_gets
        |                                                                  | 9. sys_process_exit(0)      |      via dup_con
        |                                                                  |<----------------------------|
        | 10. Woken by AB_SIGNAL_TASK_TERMINATED!                          | (switches TTBR0, frees VMAR |
        |<-----------------------------------------------------------------|  & handles, unlinks job)    |
```

## 5.1 Step 1: Compilation, Linking & Packaging at Build Time
1. [`user/apps/Makefile`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/apps/Makefile) compiles [`user/apps/calc.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/apps/calc.c) into `calc.o`.
2. The linker combines [`user/ulibc/crt0.o`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/crt0.S), `calc.o`, and `user/ulibc/libulibc.a` using the shared user linker script [`user/link.ld`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/link.ld):
   - Entry point `_start` (from `crt0.S`) is placed at the very first instruction at **`0x20000000`**.
   - `objcopy -O binary` produces the raw flat image `image/calc.bin`.
3. [`image/make_tar_disk.py`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/image/make_tar_disk.py) packs `motd.txt`, `zircon.txt`, and `calc.bin` into the USTAR archive `image/disk.img` attached to QEMU's VirtIO block device.

## 5.2 Step 2: Fetching `calc.bin` as a VMO from `fs-srv`
When you type `calc` in `abos-shell-0` ([`user/shell/shell.c:356-372`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/shell/shell.c#L356-L372)):
1. The shell sends an `FS_OP_OPEN_VMO` request with `"calc.bin"` over its `fs_ch` channel and waits for `AB_SIGNAL_READABLE`.
2. `fs-srv` ([`user/drivers/fs_srv.c:111-147`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/drivers/fs_srv.c#L111-L147)) locates `"calc.bin"` inside its DMA-cached `tar_cache` buffer.
3. `fs-srv` calls `sys_vmo_create(vmo_sz, 0, &rw_vmo)` and `sys_vmo_write(rw_vmo, tar_cache + off + 512, 0, fsz)`. Inside the kernel, [`ab_vmo_write()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1051-L1077) allocates fresh 4 KB physical pages (`VM_PAGE_STATE_OBJECT`) and copies `calc.bin`'s machine code into them.
4. `fs-srv` transfers the VMO handle back to `abos-shell-0` over `fs_ch` (`sys_channel_write(ch_h, &rep, sizeof(rep), &ro_file_vmo, 1)`).

## 5.3 Step 3: Creating the Process, `TTBR0_EL1` Page Table & Mapping `0x20000000`
Back in `abos-shell-0` ([`user/shell/shell.c:374-396`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/shell/shell.c#L374-L396)):
1. **Create Process & Root VMAR**:
   ```c
   sys_process_create(job_h, "calc.bin", &calc_proc, &calc_vmar);
   ```
   The kernel verifies that `job_h` has `AB_RIGHT_MANAGE_PROCESS`, allocates a new `ab_process_t` with a fresh hardware ASID (`ab_alloc_asid()`) and a newly allocated `L1` page table (`vmm_create_user_aspace()`), registers it under the root job, and creates its Root VMAR (`base_va = 0x10000000`, `size = 0x30000000`).
2. **Map `calc.bin` VMO at `0x20000000`**:
   ```c
   sys_vmar_map(calc_vmar, 0x10000000ULL, bin_vmo_h, 0, map_sz,
                AB_VM_PERM_READ | AB_VM_PERM_WRITE | AB_VM_PERM_EXECUTE,
                &entry_va);
   ```
   [`ab_vmar_map()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1127-L1226) computes `map_va = vmar->base_va (0x10000000) + 0x10000000 = 0x20000000`, populates `L1 -> L2 -> L3` page table entries in `calc.bin`'s `TTBR0_EL1` table mapping `VA 0x20000000` to `bin_vmo_h`'s physical pages, cleans D-cache and invalidates I-cache (`dc cvau` + `ic ialluis`), and returns `entry_va = 0x20000000`.

## 5.4 Step 4: Bootstrap Handle Transfer & Entering `EL0`
Next ([`user/shell/shell.c:398-428`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/shell/shell.c#L398-L428)):
1. `abos-shell-0` creates a thread (`sys_thread_create(calc_proc, "calc.bin", &calc_thr)`) and a bootstrap channel pair (`sys_channel_create(0, &p_ch, &c_ch)`).
2. It duplicates its own DebugLog handle (`dup_log`) and Console channel handle (`dup_con`), tags them in an [`ab_proc_args_t`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/include/ipc_proto.h#L24-L28) header (`AB_PA_LOG`, `AB_PA_CONSOLE`), and writes the header + handles into `p_ch`.
3. It starts the process:
   ```c
   sys_process_start(calc_proc, calc_thr, entry_va /* 0x20000000 */, 0, c_ch, 0);
   ```
4. Inside [`ab_process_start_user()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1537-L1591) and [`user_thread_trampoline()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L1492-L1535):
   - The kernel transfers `c_ch` from `abos-shell-0`'s handle table into slot `0` of `calc.bin`'s handle table (`child_arg_h = 0x50000100`).
   - When the scheduler switches to `calc.bin`'s kernel thread, `user_thread_trampoline()` switches `TTBR0_EL1` to `calc.bin`'s page table (`vmm_switch_aspace(proc->aspace)`).
   - Because `td->user_sp == 0`, it allocates **6 physical pages (24 KB)** and maps them at **`0x00800000 .. 0x00806000`** for `calc.bin`'s `EL0` stack (`td->user_sp = 0x00806000`).
   - It calls [`arm64_uspace_entry(arg0, arg1, 0x20000000, 0x00806000, kstack_top)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/uspace_entry.S#L12-L50), which sets `SP_EL0 = 0x00806000`, `ELR_EL1 = 0x20000000`, `SPSR_EL1 = 0` (`EL0t`), zeroes `x2..x30` so no kernel pointers leak to user space, and executes **`eret`**.

## 5.5 Step 5: `crt0.S`, `__ulibc_init`, Interactive Execution & Exit
1. **C Runtime Initialization ([`user/ulibc/crt0.S`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/crt0.S) & [`user/ulibc/crt_env.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/crt_env.c))**:
   - Execution begins in `EL0` at `_start` (`0x20000000`) with `x0 = boot_ch` (`0x50000100`).
   - `_start` zeroes `[__bss_start .. __bss_end)` and calls [`__ulibc_init(boot_ch, arg1)`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/ulibc/crt_env.c#L20-L52).
   - `__ulibc_init()` calls `sys_channel_read(boot_ch, ...)` to unpack `ab_proc_args_t`, populates `__ulibc_log_h` (`AB_PA_LOG`) and `__ulibc_con_ch` (`AB_PA_CONSOLE`), and closes `boot_ch`.
2. **Running `main()` ([`user/apps/calc.c`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/apps/calc.c))**:
   - `_start` calls `bl main`.
   - `calc.c` calls standard `printf("Enter first number: ")` and `u_gets(num1_str, ...)`, which use `__ulibc_con_ch` to exchange `CON_OP_WRITE` and `CON_OP_READ_LINE` IPC messages directly with `console-srv` on the active Virtual Terminal!
3. **Process Exit & Full Resource Reclamation**:
   - When `main()` returns `0`, `crt0.S` passes `x0` to `bl sys_process_exit`.
   - Inside [`ab_process_exit_current()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/syscalls.c#L972-L1046), the kernel switches `TTBR0_EL1` to `empty_ttbr0_l1`, unmaps and releases all VMAR mappings, closes all open handles in `calc.bin`'s handle table, removes `calc.bin` from its parent job, asserts **`AB_SIGNAL_TASK_TERMINATED`** on `calc_proc`, and exits the thread (whose 8 KB kernel stack is reaped by [`thread_reap_dead()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/thread.c#L87-L120)).
   - `abos-shell-0` wakes up from `sys_object_wait_one(calc_proc, AB_SIGNAL_TASK_TERMINATED, ...)` ([`shell.c:422`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/user/shell/shell.c#L422)), closes `calc_proc` (triggering [`process_destroy()`](file:///usr/local/google/home/lipingliu/80_Zircon/arm_os/ABOS/lesson_20/kernel/core/object.c#L223-L270) to free `calc.bin`'s 24 KB user stack, page tables, and ASID), and prints the next `abos[tty0]$ ` prompt.
