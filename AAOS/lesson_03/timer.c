#include "timer.h"
#include "gic.h"
#include "output.h"

// Timer Interval (approx 1 second if freq is 62.5MHz, but QEMU might differ)
// We will read CNTFRQ_EL0 to get the actual frequency.
static uint64_t timer_interval = 0;

void timer_init(void) {
    // 1. Read System Frequency
    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    puts("Timer Frequency: ");
    puthex(freq);
    puts(" Hz\n");
    
    timer_interval = freq / 1000; // 1 interrupt per millisecond (1000Hz)

    // 2. Set Timer Compare Value
    // We use CNTP_TVAL_EL0 (Timer Value) which is a down-counter from the current time?
    // Actually CNTP_TVAL_EL0 is "TimerValue = CompareValue - SystemCount".
    // Writing to TVAL sets CompareValue to "SystemCount + WrittenValue".
    asm volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));

    // 3. Enable Timer and Unmask Interrupt
    // CNTP_CTL_EL0:
    // Bit 0: ENABLE (1 = Enabled)
    // Bit 1: IMASK (1 = Masked, 0 = Unmasked)
    // Bit 2: ISTATUS (Read-only)
    // We want ENABLE=1, IMASK=0 -> 0x1
    asm volatile("msr cntp_ctl_el0, %0" :: "r"(1));

    // 4. Enable Timer Interrupt in GIC
    // ID 30 is Non-Secure Physical Timer (PPI)
    // ID 27 is Virtual Timer (PPI)
    // QEMU virt usually uses 30 for EL1 physical timer.
    gic_enable_interrupt(30);
}

void timer_handler(void) {
    static int count = 0;
    
    // 1. Reload Timer
    // Writing to TVAL clears the interrupt condition and sets the next deadline.
    asm volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));
    
    // 2. Print Message every 1000 ticks
    count++;
    if (count % 1000 == 0) {
        puts("Timer Trigger\n");
    }
}
