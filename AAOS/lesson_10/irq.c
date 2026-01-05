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
    } else if (int_id == 33) {
        uart_handler();
    } else {
        puts("Received IRQ: ");
        puthex(int_id);
        puts("\n");
    }

    // 4. End the interrupt
    gic_end_interrupt(iar);
}


