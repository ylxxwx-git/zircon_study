# QEMU `virt` Machine Hardware Reference for ABOS

Like AAOS and Zircon's `qemu-arm64` target (`zircon_org/docs/qemu.md`), ABOS targets the QEMU `virt` AArch64 platform (`-M virt -cpu cortex-a53 -smp 4`).

## 1. Physical Memory Map

| Device / Region | Start Physical Address | Size | ABOS Usage |
| :--- | :--- | :--- | :--- |
| **GICv2 Distributor (`GICD`)** | `0x0800_0000` | 64 KB | Lesson 02+: Routing SPIs (UART IRQ 33, VirtIO IRQ 48+) and PPIs (Timer IRQ 30) |
| **GICv2 CPU Interface (`GICC`)** | `0x0801_0000` | 64 KB | Lesson 02+: Per-CPU interrupt acknowledge (`GICC_IAR`) and EOI (`GICC_EOIR`) |
| **PL011 UART0** | `0x0900_0000` | 4 KB | Lesson 01–12: Kernel `dprintf` debuglog; Lesson 13+: Mapped into EL0 Console Driver via Physical VMO + Interrupt Object! |
| **PL031 RTC** | `0x0901_0000` | 4 KB | Real-Time Clock |
| **VirtIO MMIO Transports** | `0x0A00_0000` – `0x0A00_3FFF` | 16 KB (32 slots $\times$ 512B) | Lesson 13+: Mapped into EL0 VirtIO Block Driver via `ab_vmo_create_physical` |
| **System DRAM** | `0x4000_0000` | 128 MB+ (default) | `0x4000_0000`: DTB blob placed by QEMU; `0x4008_0000`: Kernel load address |

## 2. Virtual Address Space Split (Lesson 04+)

Following Zircon (`zircon_org/kernel/arch/arm64/include/arch/kernel_aspace.h`), ABOS splits the 64-bit virtual address space between:
* **User Space (`TTBR0_EL1`)**: `0x0000_0000_0000_0000` – `0x0000_FFFF_FFFF_FFFF`
  * Managed per-process by a `VmAspace` and exposed to user space as a tree/table of **VMARs** (`VmAddressRegionDispatcher`) and **VMOs** (`VmObjectDispatcher`).
  * Device MMIO is **never** identity-mapped in normal user page tables; only a privileged EL0 driver process holding the `Resource` capability can request a physical VMO (`ab_vmo_create_physical`) and map a specific MMIO page into its `TTBR0_EL1` via `ab_vmar_map`.
* **Kernel Space (`TTBR1_EL1`)**: `0xFFFF_FFFF_0000_0000` – `0xFFFF_FFFF_FFFF_FFFF`
  * Shared across all processes (`PXN` / `UXN` / `AP` bits prevent EL0 access).
  * Maps kernel code/data, physical memory direct map (`physmap`), and kernel-internal MMIO (GIC, early debug UART).

## 3. Interrupt Numbers (GICv2 IDs)

* **ID 0–15 (SGIs)**: Software Generated Interrupts (used for inter-processor interrupts / IPIs).
* **ID 16–31 (PPIs)**: Private Peripheral Interrupts (per-CPU):
  * **ID 30**: ARM Non-Secure Physical Generic Timer (`CNTP`).
* **ID 32+ (SPIs)**: Shared Peripheral Interrupts:
  * **ID 33**: PL011 UART0 (SPI 1).
  * **ID 48..79**: VirtIO MMIO transports (SPI 16..47).
