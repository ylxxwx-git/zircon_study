#include "gic.h"

void gic_dist_init(void) {
    // 1. Disable Distributor
    GICD_CTLR = 0;

    // 2. Configure Interrupts
    // For simplicity, we assume all are Level Sensitive and target CPU 0.
    // Real implementation would iterate through all interrupts.
    
    // Enable SGI/PPI (ID 0-31) and some SPIs
    // We will enable them explicitly with gic_enable_interrupt

    // 3. Enable Distributor
    GICD_CTLR = 1;
}

void gic_cpu_init(void) {
    // 1. CPU Interface Configuration
    // Priority Mask: Allow all priorities (0xFF)
    GICC_PMR = 0xFF;
    
    // 2. Enable CPU Interface
    GICC_CTLR = 1;
}

void gic_enable_interrupt(uint32_t int_id) {
    // Enable the interrupt in ISENABLER
    // Each register holds 32 interrupts
    uint32_t reg = int_id / 32;
    uint32_t bit = int_id % 32;
    GICD_ISENABLER[reg] = (1 << bit);

    // Route to CPU 0 (for SPIs, ID >= 32)
    // ITARGETSR registers are byte-accessible or word-accessible.
    // Each byte corresponds to an interrupt ID.
    // We want to target CPU 0 (Interface 1 -> 0x01)
    if (int_id >= 32) {
        uint32_t target_reg = int_id / 4;
        uint32_t target_shift = (int_id % 4) * 8;
        
        // Read-Modify-Write to set target for this interrupt
        uint32_t current = GICD_ITARGETSR[target_reg];
        current &= ~(0xFF << target_shift); // Clear
        current |= (0x01 << target_shift);  // Set to CPU 0
        GICD_ITARGETSR[target_reg] = current;
    }
}

uint32_t gic_acknowledge_interrupt(void) {
    return GICC_IAR;
}

void gic_end_interrupt(uint32_t int_id) {
    GICC_EOIR = int_id;
}
