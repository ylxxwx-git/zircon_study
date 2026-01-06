#include "timer.h"
#include "gic.h"
#include "output.h"
#include "sched.h"

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
    
    timer_interval = freq / 10; // 1 interrupt per 100ms (10Hz)

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

uint64_t get_timer_freq(void) {
    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

extern int get_core_id();
extern void safe_puts(const char *s);
extern void safe_puthex(uint64_t v);
extern void lock_uart();
extern void unlock_uart();

void timer_handler(void) {
    int core_id = get_core_id();
    
    // Top Half: Fast, minimal work
    // 1. Reload Timer
    // Hardcode to approx 100ms (assuming 62.5MHz) -> 6,250,000
    uint64_t interval = 6250000;
    if (timer_interval > 0) interval = timer_interval;
    asm volatile("msr cntp_tval_el0, %0" :: "r"(interval));
    
    // 2. Timer Tick (Preemption)
    timer_tick();
}
