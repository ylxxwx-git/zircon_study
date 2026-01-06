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
void map_user_page(uint64_t pa) {
    // 1. Allocate NEW L1 Table
    uint64_t *l1_table = pmm_alloc_page();
    if (!l1_table) return;
    
    // 2. Allocate L2 Table
    uint64_t *l2_table = pmm_alloc_page();
    if (!l2_table) return;
    
    // 3. Allocate L3 Table (for the first 2MB)
    uint64_t *l3_table = pmm_alloc_page();
    if (!l3_table) return;
    
    // 4. Initialize Tables (Zero them out)
    memset(l1_table, 0, 4096);
    memset(l2_table, 0, 4096);
    memset(l3_table, 0, 4096);
    
    // 5. Link Tables
    // L1[0] -> L2
    uint64_t l2_pa = (uint64_t)l2_table - 0xFFFFFFFF40000000;
    l1_table[0] = l2_pa | PTE_TABLE_DESC;
    
    // L2[0] -> L3
    uint64_t l3_pa = (uint64_t)l3_table - 0xFFFFFFFF40000000;
    l2_table[0] = l3_pa | PTE_TABLE_DESC;
    
    // 6. Map User Page at L3[0]
    // L3[0] -> User Page (PA)
    // AP[1]=1 (User RW)
    l3_table[0] = pa | PTE_PAGE_RAM | PTE_ATTR_AP_RW_USER;
    
    // 7. Switch TTBR0 to new L1 Table
    uint64_t l1_pa = (uint64_t)l1_table - 0xFFFFFFFF40000000;
    set_ttbr0_el1(l1_pa);
    
    // 8. Invalidate TLB
    tlb_invalidate_all();
    
    kputs("Mapped User Page at 0x0 (New Table)\n");
}
