#include "mmu.h"
#include "output.h"

// External helpers
extern void set_ttbr0_el1(uint64_t);
extern void set_ttbr1_el1(uint64_t);
extern void set_tcr_el1(uint64_t);
extern void set_mair_el1(uint64_t);
extern uint64_t get_sctlr_el1(void);
extern void set_sctlr_el1(uint64_t);
extern void tlb_invalidate_all(void);

extern void safe_puts(const char *s);

// L1 Page Table (512 entries * 8 bytes = 4KB)
// Covers 512GB (Virtual Address Space size determined by TCR.T0SZ = 25 -> 39 bits)
// With 39 bits, translation starts at Level 1.
__attribute__((aligned(4096)))
static uint64_t l1_table[512];

void mmu_init(void) {
    // 1. Setup Identity Map
    // Entry 0: 0x00000000 - 0x3FFFFFFF (1GB) -> Device nGnRnE
    // Covers Flash, GIC, UART
    l1_table[0] = 0x00000000 | PTE_BLOCK_DEVICE;
    
    // Entry 1: 0x40000000 - 0x7FFFFFFF (1GB) -> Normal Cacheable
    // Covers RAM (up to 1GB)
    l1_table[1] = 0x40000000 | PTE_BLOCK_RAM;
    
    // 2. Configure MAIR
    set_mair_el1(MAIR_VALUE);
    
    // 3. Configure TCR
    set_tcr_el1(TCR_VALUE);
    
    // 4. Configure TTBR0 (User/Kernel low addresses)
    // We use TTBR0 for everything for now (Identity Map).
    set_ttbr0_el1((uint64_t)l1_table);
    // set_ttbr1_el1((uint64_t)l1_table); // Not using TTBR1 yet (EPD1=1 in TCR)
    
    // 5. Invalidate TLB
    tlb_invalidate_all();
    
    // 6. Enable MMU
    uint64_t sctlr = get_sctlr_el1();
    sctlr |= SCTLR_VALUE_MMU;
    set_sctlr_el1(sctlr);
    
    safe_puts("MMU Enabled.\n");
}
