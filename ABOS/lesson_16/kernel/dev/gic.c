/* =========================================================================
 * gic.c - ARM GICv2 Driver Implementation
 * ========================================================================= */
#include "gic.h"

void gic_dist_init(void) {
    GICD_CTLR = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    GICD_CTLR = 1;
    __asm__ volatile("dsb sy" ::: "memory");
}

void gic_cpu_init(void) {
    /* Accept all priority levels (0xFF) and enable Group 0/1 delivery */
    GICC_PMR  = 0xFF;
    GICC_CTLR = 1;
    __asm__ volatile("dsb sy" ::: "memory");
}

void gic_enable_interrupt(uint32_t int_id) {
    uint32_t reg = int_id / 32;
    uint32_t bit = int_id % 32;
    GICD_ISENABLER[reg] = (1U << bit);

    /* Route SPIs (ID >= 32) to CPU 0 by default */
    if (int_id >= 32) {
        uint32_t target_reg   = int_id / 4;
        uint32_t target_shift = (int_id % 4) * 8;
        uint32_t val = GICD_ITARGETSR[target_reg];
        val &= ~(0xFFU << target_shift);
        val |=  (0x01U << target_shift);
        GICD_ITARGETSR[target_reg] = val;
    }
}

void gic_disable_interrupt(uint32_t int_id) {
    uint32_t reg = int_id / 32;
    uint32_t bit = int_id % 32;
    GICD_ICENABLER[reg] = (1U << bit);
}

uint32_t gic_acknowledge_interrupt(void) {
    return GICC_IAR;
}

void gic_end_interrupt(uint32_t iar) {
    GICC_EOIR = iar;
}
