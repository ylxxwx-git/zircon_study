#include "mmu.h"
#include "mmu.h"
#include "uart.h"
#include "print.h"
#include "string.h"
#include "pmm.h"

// External helpers
extern void set_ttbr0_el1(uint64_t);
extern void set_ttbr1_el1(uint64_t);
extern void set_tcr_el1(uint64_t);
extern void set_mair_el1(uint64_t);
extern uint64_t get_sctlr_el1(void);
extern void set_sctlr_el1(uint64_t);
extern void tlb_invalidate_all(void);



// External Page Table from boot.S
extern uint64_t idmap_table[];

void mmu_init(void) {
    // We rely on boot.S initialization for now.
    // Re-initializing here might break Higher Half Kernel if we are not careful.
    // For now, just print status.
    kputs("MMU Initialized in boot.S.\n");
}

// Map a single 4KB page at VA 0x0 to PA `pa` for User (EL0)
// This creates a NEW L1 table for the process and switches TTBR0.
// Map a single 4KB page at `va` to `pa` for User (EL0)
// Creates/Traverses L1/L2/L3 tables as needed.
void map_user_page(uint64_t va, uint64_t pa) {
    uint64_t l1_index = (va >> 30) & 0x1FF;
    uint64_t l2_index = (va >> 21) & 0x1FF;
    uint64_t l3_index = (va >> 12) & 0x1FF;
    
    // 1. Get or Allocate L1 Table
    // We need to know where the current L1 table is.
    // For now, we assume we are modifying the CURRENT TTBR0 table.
    // BUT wait, we switched TTBR0 in the previous version of this function.
    // We need a way to track the current process's page table.
    // For simplicity in this lesson, we will read TTBR0_EL1.
    
    uint64_t ttbr0;
    asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr0));
    
    uint64_t *l1_table;
    // Check if TTBR0 is 0 OR points to the Identity Map (which we want to replace)
    // idmap_table is at 0x4008xxxx (Physical).
    if (ttbr0 == 0 || ttbr0 == (uint64_t)idmap_table) {
        // Allocate New L1 Table
        l1_table = pmm_alloc_page();
        if (!l1_table) return;
        memset(l1_table, 0, 4096);
        
        // Set TTBR0
        uint64_t l1_pa = (uint64_t)l1_table - 0xFFFFFFFF40000000;
        set_ttbr0_el1(l1_pa);
    } else {
        l1_table = (uint64_t *)(ttbr0 + 0xFFFFFFFF40000000);
    }
    
    // 2. Traverse L1 -> L2
    if (!(l1_table[l1_index] & PTE_VALID)) {
        uint64_t *l2_table = pmm_alloc_page();
        if (!l2_table) return;
        memset(l2_table, 0, 4096);
        
        uint64_t l2_pa = (uint64_t)l2_table - 0xFFFFFFFF40000000;
        l1_table[l1_index] = l2_pa | PTE_TABLE_DESC;
    }
    
    uint64_t l2_pa = l1_table[l1_index] & 0x0000FFFFFFFFF000;
    uint64_t *l2_table = (uint64_t *)(l2_pa + 0xFFFFFFFF40000000);
    
    // 3. Traverse L2 -> L3
    if (!(l2_table[l2_index] & PTE_VALID)) {
        uint64_t *l3_table = pmm_alloc_page();
        if (!l3_table) return;
        memset(l3_table, 0, 4096);
        
        uint64_t l3_pa = (uint64_t)l3_table - 0xFFFFFFFF40000000;
        l2_table[l2_index] = l3_pa | PTE_TABLE_DESC;
    }
    
    uint64_t l3_pa = l2_table[l2_index] & 0x0000FFFFFFFFF000;
    uint64_t *l3_table = (uint64_t *)(l3_pa + 0xFFFFFFFF40000000);
    
    // 4. Map Page at L3
    l3_table[l3_index] = pa | PTE_PAGE_RAM | PTE_ATTR_AP_RW_USER;
    
    // 5. Invalidate TLB
    tlb_invalidate_all();
    
    // kputs("Mapped Page\n");
}
