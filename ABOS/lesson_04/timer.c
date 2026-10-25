/* =========================================================================
 * timer.c - ARMv8 Generic Timer Implementation
 * References:
 *   - zircon_org/kernel/dev/timer/arm_generic/arm_generic_timer.cpp
 * ========================================================================= */
#include "timer.h"
#include "gic.h"
#include "spinlock.h"
#include "debuglog.h"

static uint64_t timer_freq_hz = 0;
static uint64_t timer_interval_cnt = 0;
static volatile uint64_t cpu_timer_ticks[SMP_MAX_CPUS] = {0};

void timer_init_percpu(uint32_t tick_hz) {
    uint32_t cpu = arch_curr_cpu_num();
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));

    if (cpu == 0) {
        timer_freq_hz = freq;
        timer_interval_cnt = freq / tick_hz;
        dprintf(INFO, "[timer] ARM Generic Timer freq=%lu Hz, tick_rate=%u Hz (interval=%lu ticks)\n",
                freq, tick_hz, timer_interval_cnt);
    }

    /* Program CNTP_TVAL_EL0 and enable timer (ENABLE=1, IMASK=0) */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval_cnt));
    __asm__ volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));
    __asm__ volatile("isb");

    /* Enable PPI 30 in GIC for this CPU */
    gic_enable_interrupt(TIMER_ARCH_PHYS_IRQ);
}

void timer_irq_handler(void) {
    uint32_t cpu = arch_curr_cpu_num();

    /* Rearm physical timer for the next tick */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval_cnt));

    uint64_t t = ++cpu_timer_ticks[cpu];
    if (t <= 3) {
        dprintf(INFO, "[timer] CPU %u tick #%lu (uptime=%lu ms)\n",
                cpu, t, current_time_ns() / 1000000UL);
    }
}

uint64_t timer_get_ticks(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) return 0;
    return cpu_timer_ticks[cpu];
}

/* Converts CNTPCT_EL0 counter into nanoseconds since boot (like Zircon's current_time()) */
ab_time_t current_time_ns(void) {
    uint64_t pct;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(pct));
    if (timer_freq_hz == 0) return 0;
    return (pct * 1000000000ULL) / timer_freq_hz;
}
