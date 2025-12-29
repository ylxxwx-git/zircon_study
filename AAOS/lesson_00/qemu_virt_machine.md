# QEMU `virt` Machine Hardware Details

The `virt` machine is a generic Arm platform provided by QEMU. Unlike the `raspi3` or `raspi4` machines, it does not attempt to emulate specific hardware quirks of a real board. Instead, it provides a clean, standards-compliant environment (using Device Tree or ACPI) ideal for OS development.

## Hardware Configuration

When you run QEMU with `-M virt`, it dynamically generates a system with the following characteristics:

*   **CPU**: Configurable (e.g., `-cpu cortex-a53`, `-cpu cortex-a72`, or `-cpu max`).
*   **RAM**: Configurable size (e.g., `-m 1024M`). Starts at address `0x40000000`.
*   **GIC**: Generic Interrupt Controller (v2 or v3 depending on `gic-version` flag).
*   **UART**: PL011 UART for serial communication.
*   **Timer**: Arm Generic Timer.
*   **PCIe**: Generic PCIe host bridge (ECAM).
*   **VirtIO**: Support for virtualized devices (network, disk, gpu) via MMIO or PCIe.

## Memory Map

The memory map of the `virt` machine is generally stable, but you should ideally parse the **Device Tree Blob (DTB)** passed in register `x0` at boot to be 100% sure.

Here is the standard memory map for `qemu-system-aarch64 -M virt`:

| Device / Region | Start Address | End Address | Size | Description |
| :--- | :--- | :--- | :--- | :--- |
| **Flash** | `0x0000_0000` | `0x07FF_FFFF` | 128 MB | Flash memory (holds firmware like UEFI/TF-A if used). |
| **GIC Distributor** | `0x0800_0000` | `0x0800_FFFF` | 64 KB | GICv2/v3 Distributor interface. |
| **GIC CPU / Red** | `0x0801_0000` | ... | ... | GICv2 CPU Interface or GICv3 Redistributors. |
| **PL011 UART** | `0x0900_0000` | `0x0900_0FFF` | 4 KB | **Primary Serial Console**. |
| **RTC** | `0x0901_0000` | `0x0901_0FFF` | 4 KB | PL031 Real Time Clock. |
| **VirtIO MMIO** | `0x0A00_0000` | `0x0A00_3FFF` | 16 KB | VirtIO transports (Net, Block, etc.). |
| **PCIe ECAM** | `0x1000_0000` | `0x2FFF_FFFF` | 256 MB | PCIe Configuration Space. |

| **DRAM (System Memory)** | **`0x4000_0000`** | ... | ... | **Main System Memory**. Size determined by `-m` flag. |

> [!IMPORTANT]
> **RAM Base Address**: The most important address to remember is **`0x40000000`**. Your kernel needs to be linked to run from this address (or higher), not `0x0` or `0x80000` (which is common on Raspberry Pi).

## Interrupts (SPIs)

Common Shared Peripheral Interrupts (SPIs):

*   **UART (PL011)**: IRQ 1 (SPI 33)
*   **VirtIO**: IRQ 16+ (SPI 48+)
*   **PCIe**: IRQ 3+ (SPI 35+)

*(Note: SPI numbers in Device Tree are often 0-indexed relative to SPI base 32. So DT might say "SPI 1", which is ID 33).*

## How to verify?

You can dump the Device Tree to see the exact configuration for your specific QEMU command:

```bash
# Dump the DTB
qemu-system-aarch64 -M virt -cpu cortex-a53 -machine dumpdtb=virt.dtb

# Convert to text (requires dtc)
dtc -I dtb -O dts -o virt.dts virt.dtb
```
