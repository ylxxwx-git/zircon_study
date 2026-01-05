#ifndef MMU_H
#define MMU_H

#include <stdint.h>

// Memory Attribute Indirection Register (MAIR_EL1)
// Attr0: Device-nGnRnE (0x00)
// Attr1: Normal Memory, Inner/Outer Write-Back Non-Transient, Read-Allocate, Write-Allocate (0xFF)
#define MAIR_ATTR_DEVICE  0
#define MAIR_ATTR_NORMAL  1
#define MAIR_VALUE        ((0x00ul << (MAIR_ATTR_DEVICE * 8)) | (0xFFul << (MAIR_ATTR_NORMAL * 8)))

// Translation Control Register (TCR_EL1)
// T0SZ[5:0]  = 25 (Region size 2^(64-25) = 2^39 = 512GB)
// TG0[15:14] = 00 (4KB Granule)
// IPS[34:32] = 000 (32-bit Physical Address Size) - QEMU virt default
// EPD1[23]   = 1 (Disable TTBR1 walk)
#define TCR_T0SZ          (64 - 39) 
#define TCR_TG0_4K        (0 << 14)
#define TCR_IPS_40BIT     (2ULL << 32) // 40-bit PA (Terabytes)
#define TCR_EPD1          (1 << 23)
#define TCR_VALUE         (TCR_T0SZ | TCR_TG0_4K | TCR_IPS_40BIT | TCR_EPD1)

// System Control Register (SCTLR_EL1)
#define SCTLR_M           (1 << 0)  // MMU Enable
#define SCTLR_C           (1 << 2)  // Data Cache Enable
#define SCTLR_I           (1 << 12) // Instruction Cache Enable
#define SCTLR_VALUE_MMU   (SCTLR_M | SCTLR_C | SCTLR_I)

// Page Table Entry (PTE) Attributes
#define PTE_VALID         (1 << 0)
#define PTE_TABLE         (1 << 1) // For L0, L1, L2 pointing to next table
#define PTE_BLOCK         (0 << 1) // For L1, L2 pointing to memory block
#define PTE_PAGE          (1 << 1) // For L3 pointing to page

#define PTE_ATTR_ACCESS   (1 << 10) // Access Flag
#define PTE_ATTR_SH_ISH   (3 << 8)  // Inner Shareable
#define PTE_ATTR_RO       (1 << 7)  // Read-Only
#define PTE_ATTR_UXN      (1ULL << 54) // User Execute Never
#define PTE_ATTR_PXN      (1ULL << 53) // Privileged Execute Never
#define PTE_ATTR_AP_RW_USER (1 << 6)   // AP[1]=1 (EL0 RW)

// Lower attributes (Index into MAIR)
#define PTE_ATTR_MEM_DEV  (MAIR_ATTR_DEVICE << 2)
#define PTE_ATTR_MEM_RAM  (MAIR_ATTR_NORMAL << 2)

// Block/Page Descriptor Construction
// L1 Block (1GB) or L2 Block (2MB)
#define PTE_BLOCK_DEVICE  (PTE_VALID | PTE_BLOCK | PTE_ATTR_ACCESS | PTE_ATTR_MEM_DEV | PTE_ATTR_UXN | PTE_ATTR_PXN)
#define PTE_BLOCK_RAM     (PTE_VALID | PTE_BLOCK | PTE_ATTR_ACCESS | PTE_ATTR_MEM_RAM | PTE_ATTR_SH_ISH)

// L3 Page (4KB)
#define PTE_PAGE_DEVICE   (PTE_VALID | PTE_PAGE | PTE_ATTR_ACCESS | PTE_ATTR_MEM_DEV | PTE_ATTR_UXN | PTE_ATTR_PXN)
#define PTE_PAGE_RAM      (PTE_VALID | PTE_PAGE | PTE_ATTR_ACCESS | PTE_ATTR_MEM_RAM | PTE_ATTR_SH_ISH)

// Table Descriptor (pointing to next level)
#define PTE_TABLE_DESC    (PTE_VALID | PTE_TABLE)

void mmu_init(void);

#endif
