#include "gic.h"
#include "output.h"
#include "timer.h"

void irq_handler_c(void) {
    // 1. Acknowledge the interrupt
    uint32_t iar = gic_acknowledge_interrupt();
    uint32_t int_id = iar & 0x3FF; // Mask to get Interrupt ID

    // 2. Check if it's a spurious interrupt (ID 1023)
    if (int_id == 1023) {
        return;
    }

    // 3. Handle the interrupt
    if (int_id == 30) {
        timer_handler();
    } else {
        puts("Received IRQ: ");
        puthex(int_id);
        puts("\n");
    }

    // 4. End the interrupt
    gic_end_interrupt(iar);
}

extern int get_core_id();
extern volatile int timer_bh_pending[4];
extern void timer_bottom_half(void);

void check_and_run_softirqs(void) {
    int core_id = get_core_id();
    
    if (timer_bh_pending[core_id]) {
        // Enable Interrupts (allow nested IRQs or just BH execution)
        asm volatile("msr daifclr, #2");
        
        timer_bottom_half();
        
        // Disable Interrupts before returning to assembly restore
        asm volatile("msr daifset, #2");
    }
}
