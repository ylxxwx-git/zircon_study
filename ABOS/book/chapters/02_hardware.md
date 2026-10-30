# 2 QEMU virt Hardware Platform and System Structure

Just as Dr. Zhao Jiong's commentary on `Linux 0.12` began by examining the IBM PC/AT motherboard—its 8259A Programmable Interrupt Controller, 8253 Programmable Interval Timer, 16550A UART, and Western Digital hard-disk controller—understanding an ARM64 operating system requires an exact mental model of the hardware board on which it executes.

Instead of locking learners to a proprietary physical development board whose SoC BootROM and undocumented vendor peripherals obscure the core kernel, **ABOS** targets the industry-standard **QEMU `virt` AArch64 virtual machine** (`qemu-system-aarch64 -M virt`). Every component on the QEMU `virt` board is an architectural standard defined by ARM Limited or OASIS: ARM Cortex-A53 ARMv8-A SMP cores, ARM PSCI power management, the ARM Generic Interrupt Controller v2 (`GICv2`), the ARMv8 Generic Timer, the ARM PrimeCell `PL011` UART, the `Flattened Device Tree` (`DTB`), and the `VirtIO-MMIO` block storage bus. The exact same drivers and boot protocol used on QEMU `virt` carry directly over to KVM cloud virtual machines and server/embedded ARMv8 platforms.

This chapter dissects the virtual machine topology, physical memory and MMIO layout, DTB/`initrd` boot handoff, and peripheral register interfaces used throughout ABOS Lesson 20.

---

## 2.1 Virtual Machine Composition

When you execute `make run` in `lesson_20/Makefile`, the build system launches QEMU with the following command line:

```bash
qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 \
    -kernel image/kernel.img \
    -initrd image/bootfs.tar \
    -nographic -serial mon:stdio \
    -drive file=image/disk.img,format=raw,if=none,id=hd0 \
    -device virtio-blk-device,drive=hd0
```

Each flag configures a foundational piece of the hardware platform:

| QEMU Argument | Emulated Hardware Element | Role in ABOS Lesson 20 |
| :--- | :--- | :--- |
| `-M virt` | Generic ARMv8-A virtual motherboard | Instantiates standard MMIO buses (`0x00000000..0x3FFFFFFF`), 128 MB default DRAM starting at `0x40000000`, GICv2, PL011 UART0, VirtIO-MMIO slots, PSCI firmware, and auto-generated DTB. |
| `-cpu cortex-a53` | 64-bit ARMv8-A in-order core | Implements AArch64 state, `EL0`/`EL1` (and `EL2` if enabled), Stage-1 VMSAv8-64 MMU with 4 KB granules and 16-bit ASIDs, 64-byte cache lines, and ARMv8 Generic Timer. |
| `-smp 4` | 4-way Symmetric Multiprocessor | Creates four homogeneous Cortex-A53 cores (`MPIDR_EL1.Aff0 = 0, 1, 2, 3`) sharing inner-shareable coherent physical RAM and GICv2 Distributor (`GICD`). |
| `-kernel image/kernel.img` | Built-in ARM64 boot firmware | Loads raw flat binary `kernel.img` at physical address **`0x40080000`** (`BASE_PA + 512 KB`) and starts CPU 0 in `EL1h` with MMU disabled and `x0 = dtb_paddr`. |
| `-initrd image/bootfs.tar` | Initial RAM Disk (`BOOTFS`) | Loads `bootfs.tar` into physical RAM and records its `[start, end)` physical addresses in the DTB `/chosen` node (`linux,initrd-start` and `linux,initrd-end`). |
| `-nographic -serial mon:stdio` | Serial console multiplexing | Connects `PL011 UART0` (`PA 0x09000000`) directly to the host terminal's standard input/output (with `Ctrl+A x` / `Ctrl+A c` routed to QEMU's monitor). |
| `-drive ... -device virtio-blk-device` | VirtIO-MMIO Block Disk | Attaches the 1 MB USTAR disk image `image/disk.img` (2,048 × 512-byte sectors) to a VirtIO-MMIO transport slot (`0x0A000000 + slot × 0x200`). |

### 2.1.1 SMP CPU Topology and the ARM64 Boot Protocol

On a 4-core QEMU `virt` system, each processor identifies itself by reading the **Multiprocessor Affinity Register (`MPIDR_EL1`)**:

```c
static inline uint32_t arch_curr_cpu_num(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF); /* Aff0[7:0] -> CPU ID 0, 1, 2, or 3 */
}
```

Following the standard ARM64 Linux kernel boot protocol implemented by QEMU's built-in firmware when `-kernel` is passed a raw flat binary:

1. **Primary Core (`CPU 0`)** powers on at physical address **`0x40080000`** (`_start` in `kernel/core/start.S`) in **`EL1h`** (or `EL2h` if `-M virt,virtualization=on` is passed—which `start.S` handles seamlessly by dropping to `EL1h`). Interrupts (`DAIF`) are masked, the `MMU` and caches are disabled (`SCTLR_EL1.M = 0`), and general-purpose register **`x0`** holds the 4 KB-aligned physical address of the **Flattened Device Tree (`DTB`)** blob in RAM.
2. **Secondary Cores (`CPU 1`, `CPU 2`, `CPU 3`)** do **not** jump simultaneously into `_start` at power-on. Instead, QEMU holds them powered off inside its emulated **Power State Coordination Interface (PSCI)** firmware until `CPU 0` has initialized the `MMU` page tables, physical memory manager, heap, and `GICv2` distributor.

### 2.1.2 Secondary Core Wakeup via `PSCI_CPU_ON` (`HVC #0`)

ARM's **PSCI (DEN 0022)** standardizes multi-core power control so operating systems do not need board-specific SoC power-controller drivers. On QEMU `virt` (with `secure=off` by default), PSCI calls use the **Hypervisor Call (`HVC #0`)** conduit following the ARM **SMC Calling Convention (`SMCCC`)**:

- **`x0` = Function ID `0xC4000003` (`PSCI_0_2_FN64_CPU_ON`)**:
  - Bit `[31] = 1` (Fast Call), Bit `[30] = 1` (SMC64 / HVC64 64-bit register convention), Bits `[29:24] = 4` (Standard Secure Service Call), Bits `[15:0] = 0x0003` (`CPU_ON`).
- **`x1` = `target_cpu`**: Target `MPIDR` affinity value (`1`, `2`, or `3`).
- **`x2` = `entry_point_address`**: Physical address at which the awakened CPU begins executing with its MMU off—in ABOS (`kernel/core/main.c`), CPU 0 passes `(uintptr_t)_start` (`0x40080000`, since `_start` resides in `.idmap.text` whose symbol value equals its physical address!).
- **`x3` = `context_id`**: Opaque 64-bit value placed into `x0` of the awakened core (`0` in ABOS).

When QEMU handles `hvc #0` for `PSCI_CPU_ON`, the target core starts at `_start` (`0x40080000`), detects `mpidr & 0xFF != 0`, loads the already-built `idmap_table` and `kernel_table`, turns on its own `MMU` and caches, sets up its own 8 KB stack (`_stack_top - cpu_id × 8192`), and enters `arm64_secondary_entry(cpu_num)`.

---

## 2.2 Physical Memory and MMIO Address Map

On the QEMU `virt` board, the 64-bit physical address space is cleanly split at the **1 GB boundary (`0x40000000`)**:

- **`0x0000_0000 .. 0x3FFF_FFFF` (Lower 1 GB)**: Reserved exclusively for **Memory-Mapped I/O (MMIO)** peripheral control registers and platform flash/PCI windows. Memory accesses here have hardware side effects and must **never** be cached or speculatively reordered (`Device-nGnRnE`).
- **`0x4000_0000 .. 0x47FF_FFFF` (Default 128 MB DRAM Arena)**: Normal coherent system RAM (`32,768` frames × `4,096` bytes), cacheable with Inner-Shareable Write-Back Write-Allocate (`Normal WBWA`) attributes.

![Figure 2-1: QEMU virt Physical Address Space & 128 MB RAM Partitioning](../assets/fig_2_1_phys_mem_map.svg)

Table 2-1 documents every physical address region used by ABOS Lesson 20, how it is mapped into virtual memory, and which software component owns it:

| Physical Address Range | Size | Hardware / Memory Region | PMM Page State | Owner & Virtual Mapping in ABOS Lesson 20 |
| :--- | :--- | :--- | :--- | :--- |
| `0x0800_0000 .. 0x0800_FFFF` | 64 KB | **GICv2 Distributor (`GICD`)** | Device MMIO (outside RAM) | **`EL1` Kernel** via Periphmap `0xFFFFFFFFC8000000` (`include/gic.h`). |
| `0x0801_0000 .. 0x0801_FFFF` | 64 KB | **GICv2 CPU Interface (`GICC`)** | Device MMIO (outside RAM) | **`EL1` Kernel** via Periphmap `0xFFFFFFFFC8010000` (banked per CPU). |
| `0x0900_0000 .. 0x0900_0FFF` | 4 KB | **PrimeCell PL011 UART0** (SPI IRQ 33) | Device MMIO (outside RAM) | Early **`EL1` DebugLog** (`0xFFFFFFFFC9000000`) **+** **`EL0` `console-srv`** via Physical `VMO` mapped at `0x10000000`. |
| `0x0A00_0000 .. 0x0A00_3FFF` | 16 KB | **VirtIO-MMIO Bus** (32 × `0x200` slots, SPIs `48..79`) | Device MMIO (outside RAM) | **`EL0` `fs-srv`** via Physical `VMO` (`"virtio-blk-mmio"`) mapped at `0x10000000`. |
| `0x4000_0000 .. 0x4007_FFFF` | 512 KB | Low DRAM below ARM64 kernel load offset | `VM_PAGE_STATE_WIRED` | Reserved by ARM64 boot convention below `0x40080000`; kept wired in `pmm_init()`. |
| `0x4008_0000 .. 0x4008_2FFF` | 12 KB (3 pages) | **`.idmap` Trampoline** (`start.S` + `idmap_table` + `kernel_table`) | `VM_PAGE_STATE_WIRED` | Executed at `PA == VA` during MMU enable; holds permanent `TTBR1` L1 `kernel_table`. |
| `0x4008_3000 .. _end_pa` | ~116 KB | **Kernel Image** (`.text`, `.rodata`, `.data`, `.bss`, 32 KB boot stacks) | `VM_PAGE_STATE_WIRED` | **`EL1` Kernel** executing in Higher-Half VA `0xFFFFFFFF80083000+` (`linker.ld`). |
| `_end_pa .. 0x47FF_FFFF` | ~126 MB | **Dynamic PMM Frame Pool** (`page_array[32768]`) | `FREE` → `HEAP` / `MMU` / `ALLOC` / `OBJECT` | Dynamically allocated for the 512 KB heap, `TTBR0` L1/L2/L3 tables, 8 KB thread stacks, `VMO`s, 24 KB `EL0` stacks, and 128 KB VirtIO DMA ring/cache. |
| `initrd_start .. initrd_end` | ~60 KB | **QEMU `-initrd` (`image/bootfs.tar`)** | `WIRED` → **`FREE` (Reclaimed!)** | Placed by QEMU at `~0x44000000`; copied into `"bootfs.tar"` `VMO` in `lk_main()`, then freed back to PMM via `pmm_free_wired_range()`! |
| `dtb_paddr .. +dtb_size` | ~1 MB (`totalsize`) | **Flattened Device Tree (`DTB`)** | `VM_PAGE_STATE_WIRED` | Placed by QEMU after `initrd` (`~0x44100000`); parsed via kernel Physmap in `dtb_parse()` and kept wired. |

Observe the elegance of how ABOS handles the **`-initrd` (`bootfs.tar`)** physical pages during boot:

1. Before `pmm_init()` runs, `dtb_parse()` extracts `[initrd_start, initrd_end)` from `/chosen`.
2. `pmm_init()` initially marks those physical frames `VM_PAGE_STATE_WIRED` so that `heap_init()` (which immediately allocates 128 contiguous frames = 512 KB) and `ab_vmo_dispatcher_create()` **cannot accidentally overwrite `bootfs.tar` in place**!
3. Once `lk_main()` copies `bootfs.tar` into the kernel-managed `bootfs_vmo` (`ab_vmo_write`), it calls `pmm_free_wired_range(boot_info.initrd_start, boot_info.initrd_end)` (`main.c:102`), returning 100% of the bootloader's temporary RAM disk pages to the `PMM` free list before `userboot` even starts.

---

## 2.3 Flattened Device Tree (DTB) and BOOTFS RAM Disk Handoff

Unlike x86 PCs that historically queried BIOS `INT 0x15, EAX=0xE820` or ACPI tables to discover RAM, ARM64 platforms pass a binary **Flattened Device Tree (`FDT` / `DTB`)** whose physical address is delivered in register **`x0`** to `_start`. `start.S` saves `x0` into callee-saved register **`x20`** (`mov x20, x0`) while building page tables and clearing `.bss`, and then passes `x20` as the `uint64_t dtb_paddr` argument to `lk_main(dtb_paddr)`.

### 2.3.1 Binary Layout of an FDT Blob (`struct fdt_header`)

Every integer inside a Flattened Device Tree is stored in **big-endian (network) byte order**, whereas Cortex-A53 runs in **little-endian** mode. Therefore, `kernel/core/dtb.c` converts every 32-bit word via `be32_to_cpu()`. An FDT blob begins with a 40-byte header (`include/dtb.h`):

```
Offset  Field               Endian   Description
------  ------------------  -------  -------------------------------------------------------
0x00    magic               BE32     Must equal FDT_MAGIC = 0xD00DFEED
0x04    totalsize           BE32     Total byte length of the entire DTB blob
0x08    off_dt_struct       BE32     Byte offset from header to the Structure Block
0x0C    off_dt_strings      BE32     Byte offset from header to the Strings Block
0x10    off_mem_rsvmap      BE32     Byte offset to the Memory Reservation Block
0x14    version             BE32     FDT format version (typically 17)
0x18    last_comp_version   BE32     Lowest backward-compatible FDT version (typically 16)
0x1C    boot_cpuid_phys     BE32     Physical CPU ID of the boot processor (0)
0x20    size_dt_strings     BE32     Byte length of the Strings Block
0x24    size_dt_struct      BE32     Byte length of the Structure Block
```

### 2.3.2 Structure Block Lexical Tokens and Tree Depth

Starting at `dtb_va + off_dt_struct`, the **Structure Block** encodes the hardware device tree as a linear stream of 4-byte-aligned big-endian tokens (`dtb.c:8-12`):

| Token Constant | Value | Binary Payload Following Token |
| :--- | :--- | :--- |
| `FDT_BEGIN_NODE` | `0x00000001` | Null-terminated ASCII node name (e.g., `""` for root `/` at `depth=1`, `"memory@40000000"` or `"chosen"` at `depth=2`), padded with zeros to the next 4-byte boundary. |
| `FDT_END_NODE` | `0x00000002` | None; closes the current node and decrements `node_depth`. |
| `FDT_PROP` | `0x00000003` | Two `BE32` integers `(len, nameoff)` followed by `len` raw property bytes (padded to 4 bytes). The property's ASCII name resides at `strings_base + nameoff`. |
| `FDT_NOP` | `0x00000004` | None; ignored padding token. |
| `FDT_END` | `0x00000009` | None; marks the end of the Structure Block. |

### 2.3.3 Discovering DRAM (`/memory`) and BOOTFS (`/chosen`)

When QEMU `-M virt` boots with `-m 128M` (default) and `-initrd image/bootfs.tar`, it synthesizes two top-level child nodes (`node_depth == 2`) that `dtb_parse()` (`dtb.c:41-140`) inspects:

1. **`/memory@40000000` (`reg` property)**:
   On QEMU `virt`, `#address-cells = 2` and `#size-cells = 2`, so `reg` contains four 32-bit big-endian cells (`16` bytes):
   - `ram_base = (cells[0] << 32) | cells[1]` → `0x00000000_40000000`
   - `ram_size = (cells[2] << 32) | cells[3]` → `0x00000000_08000000` (`128 MB`)
2. **`/chosen` (`linux,initrd-start` and `linux,initrd-end` properties)**:
   Whenever `-initrd image/bootfs.tar` is passed on the QEMU command line, QEMU copies `bootfs.tar` into guest RAM and injects `linux,initrd-start` and `linux,initrd-end` (either 4-byte `u32` or 8-byte `u64` cells depending on QEMU version—`dtb.c:101-113` handles both!) into `/chosen`.

> **Design Note — FDT vs. Zircon ZBI**: Production Fuchsia bootloaders translate board-specific DTBs or UEFI tables into a flat container format called the **Zircon Boot Image (`ZBI`)**, which bundles the kernel item alongside a `ZBI_TYPE_STORAGE_BOOTFS` RAM disk item. Using QEMU's native DTB `/chosen` `initrd` handoff gives ABOS the exact same clean `BOOTFS VMO` architecture without requiring a custom UEFI bootloader stage!

---

## 2.4 Peripheral Controllers

ABOS interacts with four standard hardware peripheral controllers: **two inside `EL1`** (GICv2 and the ARM Generic Physical Timer, which drive preemptive scheduling and interrupt brokering) and **two completely inside unprivileged `EL0` driver processes** (PL011 UART0 in `console-srv` and VirtIO-MMIO Block in `fs-srv`, plus early spinlock-protected polled UART output in `EL1` `debuglog.c`).

### 2.4.1 ARM Generic Interrupt Controller v2 (`GICv2`)

In a multi-core ARMv8 system, external device interrupt wires do not connect directly to a single CPU's `IRQ` pin. Instead, an **ARM GICv2** controller splits interrupt management into two MMIO blocks:

1. **Distributor (`GICD` at `PA 0x08000000`, `VA 0xFFFFFFFFC8000000`)**: A single system-wide unit shared by all 4 CPUs. It prioritizes interrupt sources, enables or masks individual IRQ lines, and routes Shared Peripheral Interrupts to target CPUs.
2. **CPU Interface (`GICC` at `PA 0x08010000`, `VA 0xFFFFFFFFC8010000`)**: **Banked per CPU core**—when CPU 0, 1, 2, or 3 reads or writes `0xFFFFFFFFC8010000`, the hardware automatically accesses that specific core's private `GICC` register bank!

#### GICv2 Interrupt ID Numbering (`0 .. 1019`)

GICv2 partitions hardware Interrupt IDs (`INTID`) into three architectural ranges:

| INTID Range | Category | Banking | Usage in ABOS Lesson 20 |
| :--- | :--- | :--- | :--- |
| **`0 .. 15`** | **SGI** (Software Generated Interrupts) | Banked per CPU | Reserved for inter-processor interrupts (IPIs). |
| **`16 .. 31`** | **PPI** (Private Peripheral Interrupts) | Banked per CPU | **`INTID 30`** = Non-Secure Physical Timer (`CNTP`), firing independently at 100 Hz on each of the 4 cores! |
| **`32 .. 1019`** | **SPI** (Shared Peripheral Interrupts) | Global (shared) | **`INTID 33`** = PL011 UART0 (`console-srv`); **`INTID 48 + slot`** = VirtIO-MMIO devices (`fs-srv`). Routed to CPU 0 via `GICD_ITARGETSR`. |
| **`1020 .. 1023`** | Special / Spurious IDs | Per-acknowledge | **`1023`** returned by `GICC_IAR` when no pending interrupt of sufficient priority remains (`exceptions_c.c:76` ignores `irq >= 1020`). |

#### GICv2 Register Map Used in ABOS (`include/gic.h`, `kernel/dev/gic.c`)

| Register | Offset | Block | Width | Function in ABOS |
| :--- | :--- | :--- | :--- | :--- |
| `GICD_CTLR` | `+0x000` | `GICD` | 32-bit | Distributor Control: `gic_dist_init()` writes `0`, issues `dsb sy`, then writes `1` to enable global interrupt forwarding. |
| `GICD_TYPER` | `+0x004` | `GICD` | 32-bit | Interrupt Controller Type Register (reports number of supported SPI lines and CPU interfaces). |
| `GICD_ISENABLER[n]` | `+0x100 + 4n` | `GICD` | 32-bit | **Interrupt Set-Enable**: Writing `1 << (id % 32)` to `GICD_ISENABLER[id / 32]` unmasks interrupt `id` (write-`0` has no effect!). |
| `GICD_ICENABLER[n]` | `+0x180 + 4n` | `GICD` | 32-bit | **Interrupt Clear-Enable**: Writing `1 << (id % 32)` to `GICD_ICENABLER[id / 32]` masks interrupt `id`—crucial for masking level-triggered `EL0` SPIs until `sys_interrupt_ack()`! |
| `GICD_IPRIORITYR[n]` | `+0x400 + 4n` | `GICD` | 32-bit | 8-bit priority byte per interrupt (default `0x00` = highest priority, accepted whenever `GICC_PMR = 0xFF`). |
| `GICD_ITARGETSR[n]` | `+0x800 + 4n` | `GICD` | 32-bit | 8-bit CPU target bitmask per SPI (`4` interrupts per 32-bit word). For `id >= 32`, `gic_enable_interrupt()` sets byte `id % 4` to `0x01` (CPU 0). |
| `GICC_CTLR` | `+0x0000` | `GICC` | 32-bit | Per-CPU Interface Control: `gic_cpu_init()` writes `1` on every core to enable IRQ signaling to that core. |
| `GICC_PMR` | `+0x0004` | `GICC` | 32-bit | Priority Mask Register: Set to `0xFF` so all priority levels `0x00..0xFE` are accepted. |
| `GICC_IAR` | `+0x000C` | `GICC` | 32-bit | **Interrupt Acknowledge**: Reading transitions the highest-priority pending IRQ from *Pending* to *Active* and returns `INTID` in bits `[9:0]`. |
| `GICC_EOIR` | `+0x0010` | `GICC` | 32-bit | **End of Interrupt**: Writing the exact `iar` value read from `GICC_IAR` deactivates the interrupt in the GIC state machine. |

---

### 2.4.2 ARMv8 Generic Physical Timer (PPI 30)

Every Cortex-A53 core contains an architectural **Generic Timer** fed by a system-wide monotonic counter running at a constant frequency (`62.5 MHz` = `62,500,000` ticks/sec, i.e., `16 ns` per tick on QEMU `virt`). Unlike MMIO peripherals, the Generic Timer is programmed via fast internal **System Registers** (`MSR`/`MRS`) and signals **PPI `INTID 30`** (`TIMER_ARCH_PHYS_IRQ`) privately on each core:

| System Register | Access | Description & Usage in `kernel/dev/timer.c` and `ulibc.c` |
| :--- | :--- | :--- |
| `CNTFRQ_EL0` | `EL1`/`EL0` RO | Counter-timer Frequency in Hz (`62,500,000` on QEMU `virt`). Divided by `100` (`tick_hz`) to compute `timer_interval_cnt = 625,000` ticks (`10 ms`). |
| `CNTPCT_EL0` | `EL1`/`EL0` RO | 64-bit Physical Count value. Read by `current_time_ns()` in `EL1` and directly by `ulibc_clock_get_monotonic()` in `EL0` **without a syscall**! |
| `CNTKCTL_EL1` | `EL1` RW | Kernel Control Register: `timer_init_percpu()` sets bit `0` (`EL0PCTEN = 1`) on every CPU so `EL0` processes (`console-srv`'s 25 ms `ESC` timer!) can read `CNTPCT_EL0` directly without trapping. |
| `CNTP_TVAL_EL0` | `EL1` RW | 32-bit signed TimerValue countdown register. Writing `timer_interval_cnt` sets `CVAL = CNTPCT_EL0 + interval` and **de-asserts the current timer IRQ line**! |
| `CNTP_CTL_EL0` | `EL1` RW | Physical Timer Control: Bit `0` (`ENABLE = 1`), Bit `1` (`IMASK = 0`), Bit `2` (`ISTATUS` RO). Writing `1` arms the per-CPU timer. |

Notice why `timer_irq_handler()` writes `msr cntp_tval_el0, timer_interval_cnt` *before* `gic_end_interrupt(iar)` runs: the ARM Generic Timer output is **level-sensitive** (`CNTP_CTL_EL0.ISTATUS == 1` while `CNTPCT_EL0 >= CVAL`). Advancing `CVAL` into the future via `CNTP_TVAL_EL0` immediately drops the level-sensitive timer line so the CPU does not re-enter the IRQ vector upon `GICC_EOIR`.

---

### 2.4.3 ARM PrimeCell PL011 UART0 (`0x09000000`, SPI 33)

The **PL011** is ARM's standard AMBA PrimeCell Universal Asynchronous Receiver/Transmitter, located at physical address **`0x09000000`** (`4 KB` window) and wired to **SPI `INTID 33`**. In ABOS Lesson 20, `EL1` `debuglog.c` uses polled TX (`UART_DR` / `UART_FR`) for `[kernel]` diagnostic messages, while `EL0` `console-srv` maps `0x09000000` into user space via `sys_vmo_create_physical` and drives interrupt-driven RX + multi-VT TX completely from `EL0`!

| Register | Offset | Key Bits & Masks | Role in `debuglog.c` & `user/drivers/console_srv.c` |
| :--- | :--- | :--- | :--- |
| `UART_DR` | `+0x000` | `[7:0]` Data byte | Writing pushes a byte into the 16-byte TX FIFO; reading pops the oldest received byte from the 16-byte RX FIFO. |
| `UART_FR` | `+0x018` | `RXFE` (`1<<4`), `TXFF` (`1<<5`) | Flag Register (RO): `TXFF == 1` means TX FIFO is full (busy-wait before TX); `RXFE == 1` means RX FIFO is empty (stop draining RX). |
| `UART_LCR_H` | `+0x02C` | `FEN` (`1<<4`), `WLEN8` (`3<<5`) | Line Control: `console-srv` writes `WLEN8 \| FEN` (`0x70`) to enable **8-bit words** and **16-byte hardware TX/RX FIFOs** (preventing dropped bytes on multi-byte `CSI` escape paste bursts!). |
| `UART_IMSC` | `+0x038` | `RXIM` (`1<<4`), `RTIM` (`1<<6`) | Interrupt Mask Set/Clear: `console-srv` enables both **RX FIFO threshold (`RXIM`)** and **RX receive timeout (`RTIM`)** so even a single keystroke (which doesn't fill the FIFO threshold) triggers SPI 33! |
| `UART_ICR` | `+0x044` | `RXIC` (`1<<4`), `RTIC` (`1<<6`) | Interrupt Clear (WO): Writing `RXIM \| RTIM` clears latched UART interrupt conditions after draining `UART_DR`. |

---

### 2.4.4 VirtIO-MMIO Legacy Block Device (`0x0A000000`, SPI `48+`)

QEMU `virt` exposes **32 VirtIO-MMIO transport slots** starting at physical address **`0x0A000000`**, spaced `0x200` (`512`) bytes apart (`0x0A000000 .. 0x0A003FFF` = `16 KB` = `4` pages), wired to **SPI `INTID 48 .. 79`** (`dev_irq = 48 + slot`). By default, `-device virtio-blk-device` exposes the **Legacy (v1) VirtIO-MMIO** interface (`VERSION == 1`), driven entirely in `EL0` by `user/drivers/fs_srv.c`.

#### 1. VirtIO-MMIO Control Registers (`fs_srv.c:10-24`)

| Register | Offset | R/W | Description & Initialization Sequence in `fs-srv` |
| :--- | :--- | :--- | :--- |
| `MAGIC_VALUE` | `+0x000` | RO | Must read `0x74726976` (little-endian ASCII `"virt"`). |
| `VERSION` | `+0x004` | RO | Legacy VirtIO-MMIO version (`1`). |
| `DEVICE_ID` | `+0x008` | RO | Device type: `0` = empty slot, **`2` = Block Device (`virtio-blk`)**. |
| `GUEST_FEATURES` | `+0x020` | WO | Negotiated feature bits (`0` in `fs-srv` for basic sector read/write). |
| `GUEST_PAGE_SIZE` | `+0x028` | WO | Legacy v1 page size (`4096`), used by hardware to convert `QUEUE_PFN` into a physical address (`PFN × 4096`). |
| `QUEUE_SEL` | `+0x030` | WO | Selects Virtqueue index (`0` = the single request queue of `virtio-blk`). |
| `QUEUE_NUM` | `+0x038` | WO | Sets Virtqueue ring length (`VIRTIO_RING_SIZE = 16` descriptors). |
| `QUEUE_ALIGN` | `+0x03C` | WO | Legacy v1 Used Ring alignment (`4096` bytes). |
| `QUEUE_PFN` | `+0x040` | RW | Physical Page Frame Number (`dma_pa >> 12`) of the contiguous Virtqueue memory region! |
| `QUEUE_NOTIFY` | `+0x050` | WO | Doorbell register: writing `0` notifies QEMU that new descriptors are available in Queue `0`. |
| `INTERRUPT_STAT` | `+0x060` | RO | Bit `0` = Used Buffer Notification (DMA transfer complete). |
| `INTERRUPT_ACK` | `+0x064` | WO | Writing the value read from `INTERRUPT_STAT` de-asserts the device's level-triggered SPI wire! |
| `STATUS` | `+0x070` | RW | Device status state machine: write `0` (Reset) → `1\|2` (`ACKNOWLEDGE \| DRIVER`) → configure queue → `1\|2\|4` (`DRIVER_OK`). |
| `CONFIG` | `+0x100` | RO | Device-specific config: for `virtio-blk`, bytes `+0x100..+0x107` hold the 64-bit `capacity` in 512-byte sectors (`2,048` sectors = `1 MB` for `disk.img`). |

#### 2. Split Virtqueue DMA Layout and 3-Descriptor Block Request Chain

Because QEMU's emulated DMA engine accesses guest memory by **physical address**, `fs-srv` allocates a **32-page (`128 KB`) physically contiguous DMA VMO** via `sys_vmo_create_contiguous(res_h, 32 * 4096, &dma_vmo, &dma_pa)` and partitions it cleanly:

```
Contiguous 128 KB DMA Buffer (dma_pa .. dma_pa + 0x20000):
+-------------------------------------------------------------------------+
| Page 0 (dma_pa + 0x0000):                                               |
|   - virtq_desc_t  desc[16]  (16 * 16B = 256 bytes @ offset 0x000)       |
|   - virtq_avail_t avail     (flags, idx, ring[16] @ offset 0x100)       |
|-------------------------------------------------------------------------|
| Page 1 (dma_pa + 0x1000, aligned to QUEUE_ALIGN = 4096):                |
|   - virtq_used_t  used      (device writes completed descriptor IDs)    |
|-------------------------------------------------------------------------|
| Page 2 (dma_pa + 0x2000):                                               |
|   - virtio_blk_req_t req    (16B: type=0 READ, reserved=0, sector=0)    |
|   - uint8_t req_status      (1B @ +64: initialized 0xFF -> 0x00 on OK)  |
|-------------------------------------------------------------------------|
| Pages 3..31 (dma_pa + 0x3000 .. +0x1FFFF, 29 pages = 116 KB):           |
|   - uint8_t tar_cache[118784] (232 * 512-byte USTAR disk sectors!)      |
+-------------------------------------------------------------------------+
```

Every VirtIO block transaction requires a **three-descriptor chain** linked via `VIRTQ_DESC_F_NEXT (1)` and device-writable `VIRTQ_DESC_F_WRITE (2)`:

1. **`desc[0]` (Driver → Device, `16` bytes, `flags = NEXT`)**: Points to `virtio_blk_req_t` (`type = 0` `VIRTIO_blk_T_IN`, `sector = 0`), `next = 1`.
2. **`desc[1]` (Device → Driver, `116 KB`, `flags = NEXT | WRITE`)**: Points to `tar_cache` (`dma_pa + 3 × 4096`), `next = 2`.
3. **`desc[2]` (Device → Driver, `1` byte, `flags = WRITE`)**: Points to `req_status` (`dma_pa + 2 × 4096 + 64`), where QEMU writes `0` (`VIRTIO_BLK_S_OK`) when DMA completes.

After publishing `avail->ring[0] = 0`, issuing `dsb sy`, incrementing `avail->idx = 1`, issuing `dsb sy`, and ringing `QUEUE_NOTIFY = 0`, `fs-srv` sleeps in `sys_interrupt_wait(irq_h)` without burning a single CPU cycle until SPI `48 + slot` wakes it!

---

## 2.5 Summary

The QEMU `virt` platform provides a clean, standards-based 4-core ARMv8-A SMP machine: MMIO peripherals occupy `0x00000000..0x3FFFFFFF` while 128 MB of RAM begins at `0x40000000`. CPU 0 boots at `0x40080000` with `x0` pointing to a big-endian Flattened Device Tree (`DTB`) that describes both system RAM and the `-initrd` `BOOTFS` archive (`bootfs.tar`), and wakes CPUs `1..3` via `PSCI_CPU_ON` (`HVC #0`). Only `GICv2` and the per-CPU `Generic Timer` (`PPI 30`) remain managed by the `EL1` microkernel; both `PL011 UART0` (`SPI 33`) and `VirtIO-MMIO Block` (`SPI 48+` with 128 KB contiguous DMA) are delegated via capabilities to unprivileged `EL0` server processes. Next, Chapter 3 examines the GNU AArch64 compiler, assembler, ABI, linker scripts, and build system that turn ABOS's source files into bootable binary images.
