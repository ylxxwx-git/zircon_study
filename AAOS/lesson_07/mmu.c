#include "mmu.h"
#include "mmu.h"
#include "output.h"
#include "pmm.h"

// External helpers
extern void set_ttbr0_el1(uint64_t);
extern void set_ttbr1_el1(uint64_t);
extern void set_tcr_el1(uint64_t);
extern void set_mair_el1(uint64_t);
extern uint64_t get_sctlr_el1(void);
extern void set_sctlr_el1(uint64_t);
extern void tlb_invalidate_all(void);

extern void safe_puts(const char *s);

// External Page Table from boot.S
extern uint64_t idmap_table[];

void mmu_init(void) {
    // We rely on boot.S initialization for now.
    // Re-initializing here might break Higher Half Kernel if we are not careful.
    // For now, just print status.
    safe_puts("MMU Initialized in boot.S.\n");
}

// Map a single 4KB page at VA 0x0 to PA `pa` for User (EL0)
// This requires splitting the first 1GB Block (Identity Map) into L2/L3 tables.
void map_user_page(uint64_t pa) {
    // 1. Allocate L2 Table
    uint64_t *l2_table = pmm_alloc_page();
    if (!l2_table) return;
    
    // 2. Allocate L3 Table (for the first 2MB)
    uint64_t *l3_table = pmm_alloc_page();
    if (!l3_table) return;
    
    // 3. Initialize L2 Table
    // We want to preserve the Identity Map for 0x0 - 0x40000000 (1GB)
    // But we are replacing the 1GB Block at L1[0] with this L2 table.
    // So we must fill L2 with 2MB Blocks that match the original 1GB Block (Device).
    // EXCEPT L2[0] which will point to L3.
    
    // Fill L2 with Device Blocks (0x00000000 + offset)
    for (int i = 0; i < 512; i++) {
        uint64_t block_pa = i * 0x200000; // 2MB * i
        l2_table[i] = block_pa | PTE_BLOCK_DEVICE;
    }
    
    // Point L2[0] to L3 Table (Use Physical Address)
    uint64_t l3_pa = (uint64_t)l3_table - 0xFFFFFFFF40000000;
    l2_table[0] = l3_pa | PTE_TABLE_DESC;
    
    // 4. Initialize L3 Table
    // L3[0] -> User Page (PA)
    // AP[1]=1 (User RW)
    l3_table[0] = pa | PTE_PAGE_RAM | PTE_ATTR_AP_RW_USER;
    
    // 5. Update L1 Table (idmap_table)
    uint64_t l2_pa = (uint64_t)l2_table - 0xFFFFFFFF40000000;
    idmap_table[0] = l2_pa | PTE_TABLE_DESC;
    
    // 6. Invalidate TLB
    tlb_invalidate_all();
    
    safe_puts("Mapped User Page at 0x0\n");
}
