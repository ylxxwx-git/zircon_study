#ifndef _IRQ_H
#define _IRQ_H

#include <stdint.h>

// Stack frame structure matches stp order in boot.S
struct pt_regs {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x30;
    uint64_t spsr, elr;
};

void irq_init_vectors(void);
void enable_interrupts(void);
void disable_interrupts(void);

#endif
