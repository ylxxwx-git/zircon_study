/* =========================================================================
 * mmu.h - AArch64 MMU, Kernel Physmap/Periphmap & Zircon-Style VmAspace
 * References:
 *   - zircon_org/kernel/arch/arm64/mmu.cpp
 *   - zircon_org/kernel/vm/include/vm/vm_aspace.h
 *   - zircon_org/kernel/vm/include/vm/physmap.h
 * ========================================================================= */
#ifndef ABOS_MMU_H
#define ABOS_MMU_H

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"
#include "spinlock.h"

typedef uint64_t vaddr_t;
typedef uint64_t pte_t;

/*
 * Higher-Half Kernel Address Space Layout (TTBR1_EL1):
 *   0xFFFFFFFF80000000 .. 0xFFFFFFFFBFFFFFFF : 1GB Physmap -> PA 0x40000000..0x7FFFFFFF
 *   0xFFFFFFFFC0000000 .. 0xFFFFFFFFFFFFFFFF : 1GB Periphmap -> PA 0x00000000..0x3FFFFFFF
 */
#define KERNEL_PHYSMAP_OFFSET   0xFFFFFFFF40000000ULL
#define KERNEL_PERIPHMAP_OFFSET 0xFFFFFFFFC0000000ULL

static inline void *paddr_to_physmap(paddr_t pa) {
    return (void *)(uintptr_t)(pa + KERNEL_PHYSMAP_OFFSET);
}

static inline paddr_t physmap_to_paddr(const void *va) {
    return (paddr_t)((uintptr_t)va - KERNEL_PHYSMAP_OFFSET);
}

static inline void *paddr_to_periphmap(paddr_t pa) {
    return (void *)(uintptr_t)(pa + KERNEL_PERIPHMAP_OFFSET);
}

/* AArch64 Stage-1 Page/Table Descriptor Bits */
#define PTE_VALID           (1ULL << 0)
#define PTE_TABLE           (1ULL << 1) /* For L1/L2 table descriptors */
#define PTE_PAGE            (1ULL << 1) /* For L3 4KB page descriptors */
#define PTE_ATTR_DEVICE     (0ULL << 2) /* MAIR AttrIdx 0 = Device-nGnRnE */
#define PTE_ATTR_NORMAL     (1ULL << 2) /* MAIR AttrIdx 1 = Normal WBWA */
#define PTE_AP_USER         (1ULL << 6) /* AP[1]=1: EL0 accessible */
#define PTE_AP_RO           (1ULL << 7) /* AP[2]=1: Read-only */
#define PTE_SH_INNER        (3ULL << 8) /* Inner Shareable */
#define PTE_AF              (1ULL << 10)/* Access Flag */
#define PTE_NG              (1ULL << 11)/* Non-Global (ASID-tagged TLB entry) */
#define PTE_PXN             (1ULL << 53)/* Privileged Execute Never */
#define PTE_UXN             (1ULL << 54)/* Unprivileged Execute Never */

#define PTE_ADDR_MASK       0x0000FFFFFFFFF000ULL

/* High-level mapping permission flags (modeling Zircon's ARCH_MMU_FLAG_*) */
#define MMU_FLAG_PERM_READ      (1U << 0)
#define MMU_FLAG_PERM_WRITE     (1U << 1)
#define MMU_FLAG_PERM_EXECUTE   (1U << 2)
#define MMU_FLAG_PERM_USER      (1U << 3)
#define MMU_FLAG_CACHE_DEVICE   (1U << 4)

/* User space virtual address range (TTBR0_EL1 with T0SZ=33 -> 0 .. 2GB) */
#define USER_ASPACE_BASE        0x0000000000001000ULL
#define USER_ASPACE_SIZE        0x000000007FFFF000ULL

typedef struct vm_aspace {
    spin_lock_t lock;
    paddr_t     ttbr0_phys;    /* Physical address of L1 page table */
    pte_t      *l1_virt;       /* Kernel physmap pointer to L1 table */
    uint16_t    asid;
    uint32_t    mapped_pages;
} vm_aspace_t;

void mmu_init(void);
uint16_t ab_alloc_asid(void);
void ab_free_asid(uint16_t asid);
vm_aspace_t *vmm_create_user_aspace(uint16_t asid);
int vmm_map_page(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags);
int vmm_unmap_page(vm_aspace_t *aspace, vaddr_t vaddr);
int vmm_resolve_vaddr(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t *out_paddr, pte_t *out_pte);
void vmm_switch_aspace(vm_aspace_t *aspace);
void vmm_destroy_aspace(vm_aspace_t *aspace);

#endif /* ABOS_MMU_H */
