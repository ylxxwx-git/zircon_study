# ABOS Lesson 04: Virtual Memory, Higher-Half Kernel (`physmap` & `periphmap`) & `VmAspace`

## Overview
Lesson 04 implements Zircon's virtual memory split (`zircon_org/kernel/arch/arm64/mmu.cpp`, `kernel/vm/vm_aspace.cpp`, and `kernel/vm/include/vm/physmap.h`):

1. **Higher-Half Kernel (`TTBR1_EL1`)**:
   * `start.S` builds early page tables in `.idmap.data`, enables the MMU (`SCTLR_EL1.M=1`), and branches to `high_kernel_entry` at `0xFFFFFFFF8008xxxx`.
   * **Physmap (`0xFFFFFFFF80000000+`)**: Direct-maps physical RAM (`0x40000000+`), providing `paddr_to_physmap(pa)` and `physmap_to_paddr(va)`.
   * **Periphmap (`0xFFFFFFFFC0000000+`)**: Maps MMIO peripherals (`0x00000000+` with `Device-nGnRnE`, `PXN`, `UXN`), so UART and GICv2 work in higher-half kernel VA independent of user `TTBR0_EL1`.
2. **Per-Process User Address Spaces (`vm_aspace_t`, `TTBR0_EL1`)**:
   * Implements 3-level (`L1` $\rightarrow$ `L2` $\rightarrow$ `L3`) 4KB page table management (`vmm_create_user_aspace`, `vmm_map_page`, `vmm_unmap_page`, `vmm_resolve_vaddr`, `vmm_switch_aspace`, `vmm_destroy_aspace`).

## Building & Running
```bash
make
make run
```
