/* =========================================================================
 * timer.c - ARMv8 Generic Timer Driving the LK Preemptive Scheduler
 * ========================================================================= */
#include "timer.h"
#include "gic.h"
#include "spinlock.h"
#include "debuglog.h"
#include "thread.h"

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
    }

    __asm__ volatile("msr cntkctl_el1, %0" :: "r"(1UL)); /* Allow EL0 to read cntpct_el0 */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval_cnt));
    __asm__ volatile("msr cntp_ctl_el0, %0"  :: "r"(1UL));
    __asm__ volatile("isb");

    gic_enable_interrupt(TIMER_ARCH_PHYS_IRQ);
}

void timer_irq_handler(void) {
    uint32_t cpu = arch_curr_cpu_num();
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(timer_interval_cnt));
    cpu_timer_ticks[cpu]++;
    thread_timer_tick();
}

uint64_t timer_get_ticks(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS) return 0;
    return cpu_timer_ticks[cpu];
}

ab_time_t current_time_ns(void) {
    uint64_t pct;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(pct));
    if (timer_freq_hz == 0) return 0;
    uint64_t secs = pct / timer_freq_hz;
    uint64_t rem  = pct % timer_freq_hz;
    return secs * 1000000000ULL + (rem * 1000000000ULL) / timer_freq_hz;
}
