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

extern int get_core_id();
extern void safe_puts(const char *s);
extern void safe_puthex(uint64_t v);
extern void lock_uart();
extern void unlock_uart();

// Per-core pending flags for Bottom Half
volatile int timer_bh_pending[4] = {0, 0, 0, 0};

void timer_handler(void) {
    int core_id = get_core_id();
    
    // Top Half: Fast, minimal work
    // 1. Reload Timer
    asm volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval));
    
    // 2. Schedule Bottom Half
    timer_bh_pending[core_id] = 1;
}

void timer_bottom_half(void) {
    int core_id = get_core_id();
    
    if (timer_bh_pending[core_id]) {
        // Clear flag first (or last, depending on re-entrancy needs, here it's simple)
        timer_bh_pending[core_id] = 0;
        
        // Bottom Half: Heavy work (UART output)
        static int counts[4] = {0};
        counts[core_id]++;
        
        if (counts[core_id] % 1000 == 0) {
            safe_puts("Timer Bottom Half: Core ");
            safe_puthex(core_id);
            safe_puts("\n");
        }
    }
}
