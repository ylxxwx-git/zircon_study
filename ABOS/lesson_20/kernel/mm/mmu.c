/* =========================================================================
 * mmu.c - Zircon-Style VmAspace & 3-Level (L1->L2->L3) Page Table Management
 * References:
 *   - zircon_org/kernel/arch/arm64/mmu.cpp
 *   - zircon_org/kernel/vm/vm_aspace.cpp
 * ========================================================================= */
#include "mmu.h"
#include "heap.h"
#include "debuglog.h"

static pte_t empty_ttbr0_l1[512] __attribute__((aligned(4096)));

#define MAX_ASIDS 256U
static spin_lock_t g_asid_lock = SPIN_LOCK_INITIAL_VALUE;
static uint8_t g_asid_used[MAX_ASIDS];

static inline void tlb_flush_page(uint16_t asid, vaddr_t vaddr) {
    uint64_t operand = ((uint64_t)asid << 48) | ((vaddr >> 12) & 0x00000FFFFFFFFFFFULL);
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vae1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(operand) : "memory");
}

static inline void tlb_flush_asid(uint16_t asid) {
    uint64_t operand = (uint64_t)asid << 48;
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi aside1is, %0\n"
        "dsb ish\n"
        "isb\n"
        :: "r"(operand) : "memory");
}

static inline void sync_icache_page(paddr_t paddr) {
    uintptr_t kva = (uintptr_t)paddr_to_physmap(paddr);
    for (size_t off = 0; off < PAGE_SIZE; off += 64) {
        __asm__ volatile("dc cvau, %0" :: "r"(kva + off) : "memory");
    }
    __asm__ volatile(
        "dsb ish\n"
        "ic ialluis\n"
        "dsb ish\n"
        "isb\n"
        ::: "memory");
}

uint16_t ab_alloc_asid(void) {
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&g_asid_lock, &irqstate);
    for (uint16_t asid = 1; asid < MAX_ASIDS; asid++) {
        if (!g_asid_used[asid]) {
            g_asid_used[asid] = 1;
            spin_lock_irqrestore(&g_asid_lock, irqstate);
            tlb_flush_asid(asid);
            return asid;
        }
    }
    spin_lock_irqrestore(&g_asid_lock, irqstate);
    return 1;
}

void ab_free_asid(uint16_t asid) {
    if (asid == 0 || asid >= MAX_ASIDS) return;
    tlb_flush_asid(asid);
    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&g_asid_lock, &irqstate);
    g_asid_used[asid] = 0;
    spin_lock_irqrestore(&g_asid_lock, irqstate);
}

void mmu_init(void) {
    g_asid_used[0] = 1; /* Reserve ASID 0 for kernel/idle empty TTBR0 */
    uint64_t sctlr, tcr, ttbr1;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("mrs %0, tcr_el1"   : "=r"(tcr));
    __asm__ volatile("mrs %0, ttbr1_el1" : "=r"(ttbr1));

    dprintf(INFO, "[mmu] Higher-half MMU + I/D-caches active: SCTLR_EL1=0x%lx TCR_EL1=0x%lx TTBR1_EL1=%p\n",
            sctlr, tcr, (void *)ttbr1);
}

vm_aspace_t *vmm_create_user_aspace(uint16_t asid) {
    vm_aspace_t *aspace = (vm_aspace_t *)kzalloc(sizeof(vm_aspace_t));
    if (!aspace) return NULL;

    paddr_t l1_pa = 0;
    vm_page_t *p = pmm_alloc_page(VM_PAGE_STATE_MMU, &l1_pa);
    if (!p) {
        kfree(aspace);
        return NULL;
    }

    aspace->lock.value   = 0;
    aspace->ttbr0_phys   = l1_pa;
    aspace->l1_virt      = (pte_t *)paddr_to_physmap(l1_pa);
    aspace->asid         = asid;
    aspace->mapped_pages = 0;
    return aspace;
}

static pte_t flags_to_pte(uint32_t flags) {
    pte_t attr = PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER;

    if (flags & MMU_FLAG_CACHE_DEVICE) {
        attr |= PTE_ATTR_DEVICE | PTE_PXN | PTE_UXN;
    } else {
        attr |= PTE_ATTR_NORMAL;
    }

    if (flags & MMU_FLAG_PERM_USER) {
        attr |= PTE_AP_USER | PTE_NG;
        /* Never allow EL1 to execute user pages (PXN) */
        attr |= PTE_PXN;
        if (!(flags & MMU_FLAG_PERM_EXECUTE)) {
            attr |= PTE_UXN;
        }
    } else {
        attr |= PTE_UXN;
        if (!(flags & MMU_FLAG_PERM_EXECUTE)) {
            attr |= PTE_PXN;
        }
    }

    if (!(flags & MMU_FLAG_PERM_WRITE)) {
        attr |= PTE_AP_RO;
    }

    return attr;
}

int vmm_map_page(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!aspace || vaddr < USER_ASPACE_BASE || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE)) {
        return -1;
    }

    vaddr &= ~(PAGE_SIZE - 1);
    paddr &= ~(PAGE_SIZE - 1);

    /* With T0SZ=33 (31-bit VA = 2GB), translation starts at Level 1:
     *   L1 index: bits [30:30] (0..1)
     *   L2 index: bits [29:21] (0..511)
     *   L3 index: bits [20:12] (0..511)
     */
    uint32_t l1_idx = (uint32_t)((vaddr >> 30) & 0x1FFU);
    uint32_t l2_idx = (uint32_t)((vaddr >> 21) & 0x1FFU);
    uint32_t l3_idx = (uint32_t)((vaddr >> 12) & 0x1FFU);

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&aspace->lock, &irqstate);

    pte_t *l1 = aspace->l1_virt;
    if (!(l1[l1_idx] & PTE_VALID)) {
        paddr_t l2_pa = 0;
        if (!pmm_alloc_page(VM_PAGE_STATE_MMU, &l2_pa)) {
            spin_lock_irqrestore(&aspace->lock, irqstate);
            return -1;
        }
        l1[l1_idx] = (l2_pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    }

    paddr_t l2_pa = l1[l1_idx] & PTE_ADDR_MASK;
    pte_t *l2 = (pte_t *)paddr_to_physmap(l2_pa);

    if (!(l2[l2_idx] & PTE_VALID)) {
        paddr_t l3_pa = 0;
        if (!pmm_alloc_page(VM_PAGE_STATE_MMU, &l3_pa)) {
            spin_lock_irqrestore(&aspace->lock, irqstate);
            return -1;
        }
        l2[l2_idx] = (l3_pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
    }

    paddr_t l3_pa = l2[l2_idx] & PTE_ADDR_MASK;
    pte_t *l3 = (pte_t *)paddr_to_physmap(l3_pa);

    if (!(l3[l3_idx] & PTE_VALID)) {
        aspace->mapped_pages++;
    }
    l3[l3_idx] = (paddr & PTE_ADDR_MASK) | flags_to_pte(flags);
    uint16_t asid = aspace->asid;

    spin_lock_irqrestore(&aspace->lock, irqstate);

    tlb_flush_page(asid, vaddr);
    if (flags & MMU_FLAG_PERM_EXECUTE) {
        sync_icache_page(paddr);
    }
    return 0;
}

int vmm_unmap_page(vm_aspace_t *aspace, vaddr_t vaddr) {
    if (!aspace || vaddr < USER_ASPACE_BASE || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE)) {
        return -1;
    }
    vaddr &= ~(PAGE_SIZE - 1);
    uint32_t l1_idx = (uint32_t)((vaddr >> 30) & 0x1FFU);
    uint32_t l2_idx = (uint32_t)((vaddr >> 21) & 0x1FFU);
    uint32_t l3_idx = (uint32_t)((vaddr >> 12) & 0x1FFU);

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&aspace->lock, &irqstate);

    pte_t *l1 = aspace->l1_virt;
    if (!(l1[l1_idx] & PTE_VALID)) {
        spin_lock_irqrestore(&aspace->lock, irqstate);
        return -1;
    }
    pte_t *l2 = (pte_t *)paddr_to_physmap(l1[l1_idx] & PTE_ADDR_MASK);
    if (!(l2[l2_idx] & PTE_VALID)) {
        spin_lock_irqrestore(&aspace->lock, irqstate);
        return -1;
    }
    pte_t *l3 = (pte_t *)paddr_to_physmap(l2[l2_idx] & PTE_ADDR_MASK);
    if (!(l3[l3_idx] & PTE_VALID)) {
        spin_lock_irqrestore(&aspace->lock, irqstate);
        return -1;
    }

    l3[l3_idx] = 0;
    if (aspace->mapped_pages > 0) aspace->mapped_pages--;
    uint16_t asid = aspace->asid;

    spin_lock_irqrestore(&aspace->lock, irqstate);

    tlb_flush_page(asid, vaddr);
    return 0;
}

int vmm_resolve_vaddr(vm_aspace_t *aspace, vaddr_t vaddr, paddr_t *out_paddr, pte_t *out_pte) {
    if (!aspace || vaddr < USER_ASPACE_BASE || vaddr >= (USER_ASPACE_BASE + USER_ASPACE_SIZE)) {
        return -1;
    }
    uint32_t l1_idx = (uint32_t)((vaddr >> 30) & 0x1FFU);
    uint32_t l2_idx = (uint32_t)((vaddr >> 21) & 0x1FFU);
    uint32_t l3_idx = (uint32_t)((vaddr >> 12) & 0x1FFU);

    spin_lock_saved_state_t irqstate;
    spin_lock_irqsave(&aspace->lock, &irqstate);

    pte_t *l1 = aspace->l1_virt;
    if (!(l1[l1_idx] & PTE_VALID)) {
        spin_lock_irqrestore(&aspace->lock, irqstate);
        return -1;
    }
    pte_t *l2 = (pte_t *)paddr_to_physmap(l1[l1_idx] & PTE_ADDR_MASK);
    if (!(l2[l2_idx] & PTE_VALID)) {
        spin_lock_irqrestore(&aspace->lock, irqstate);
        return -1;
    }
    pte_t *l3 = (pte_t *)paddr_to_physmap(l2[l2_idx] & PTE_ADDR_MASK);
    pte_t pte = l3[l3_idx];
    spin_lock_irqrestore(&aspace->lock, irqstate);

    if (!(pte & PTE_VALID)) return -1;

    if (out_paddr) *out_paddr = (pte & PTE_ADDR_MASK) | (vaddr & (PAGE_SIZE - 1));
    if (out_pte)   *out_pte   = pte;
    return 0;
}

void vmm_switch_aspace(vm_aspace_t *aspace) {
    uint64_t ttbr0_val;
    if (aspace) {
        ttbr0_val = ((uint64_t)aspace->asid << 48) | (aspace->ttbr0_phys & PTE_ADDR_MASK);
    } else {
        ttbr0_val = physmap_to_paddr(empty_ttbr0_l1) & PTE_ADDR_MASK;
    }
    __asm__ volatile(
        "msr ttbr0_el1, %0\n"
        "isb\n"
        :: "r"(ttbr0_val) : "memory");
}

void vmm_destroy_aspace(vm_aspace_t *aspace) {
    if (!aspace) return;
    pte_t *l1 = aspace->l1_virt;
    for (uint32_t i = 0; i < 2; i++) {
        if (l1[i] & PTE_VALID) {
            paddr_t l2_pa = l1[i] & PTE_ADDR_MASK;
            pte_t *l2 = (pte_t *)paddr_to_physmap(l2_pa);
            for (uint32_t j = 0; j < 512; j++) {
                if (l2[j] & PTE_VALID) {
                    pmm_free_page(l2[j] & PTE_ADDR_MASK);
                }
            }
            pmm_free_page(l2_pa);
        }
    }
    pmm_free_page(aspace->ttbr0_phys);
    ab_free_asid(aspace->asid);
    kfree(aspace);
}
