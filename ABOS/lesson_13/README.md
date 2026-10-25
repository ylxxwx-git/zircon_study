# ABOS Lesson 13: User-Space Device Drivers (`Resource`, Physical/DMA VMO & `Interrupt`)

## Overview
In a Zircon-style microkernel, device drivers do **not** run inside the kernel in EL1. Instead, the kernel exports three hardware capability objects gated by the **`Root Resource` (`ResourceDispatcher`)**:
1. **`SYS_VMO_CREATE_PHYSICAL`**: Wraps device MMIO physical addresses (e.g., VirtIO MMIO `0x0a000000`) in a VMO so an EL0 driver can map device registers into its `TTBR0_EL1` address space with `MMU_FLAG_CACHE_DEVICE`.
2. **`SYS_VMO_CREATE_CONTIG`**: Allocates physically contiguous DMA memory for Virtqueue descriptor/avail/used rings and returns both the VMO handle and physical base address.
3. **`InterruptDispatcher` (`SYS_INTERRUPT_CREATE` / `SYS_INTERRUPT_WAIT` / `SYS_INTERRUPT_ACK`)**: Routes GIC hardware interrupts to an EL0 driver thread.

## Building and Running
```bash
make
make run
```
