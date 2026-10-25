/* =========================================================================
 * main.c - ABOS Lesson 02: Exception Vectors, iframe_t, GICv2 & Generic Timer
 * ========================================================================= */
#include <stdint.h>
#include "debuglog.h"
#include "spinlock.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"

extern void _start(void);
extern uint64_t boot_dtb_paddr;
extern uint64_t arch_boot_el;

static volatile uint32_t secondary_cpu_ready_count = 0;

static int psci_cpu_on(uint64_t core_id, uintptr_t entry_point) {
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = core_id;
    register uint64_t x2 __asm__("x2") = (uint64_t)entry_point;
    register uint64_t x3 __asm__("x3") = 0;

    __asm__ volatile(
        "hvc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "memory");
    return (int)x0;
}

static void thread_become_idle(void) {
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void arm64_secondary_entry(uint32_t cpu_num) {
    gic_cpu_init();
    timer_init_percpu(20); /* 20 Hz (50ms) timer ticks */

    dprintf(INFO, "[cpu %u] GIC CPU interface & timer initialized; enabling IRQs\n", cpu_num);
    __atomic_fetch_add(&secondary_cpu_ready_count, 1, __ATOMIC_SEQ_CST);
    arch_enable_ints();

    thread_become_idle();
}

void lk_main(void) {
    dlog_init_early();

    dprintf(INFO, "\n====================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 02: Exception Vectors, GICv2 & Generic Timer\n");
    dprintf(INFO, "====================================================\n");
    dprintf(INFO, "[init] VBAR_EL1 installed at %p (sizeof(iframe_t)=%lu)\n",
            (void *)arm64_el1_exception_base, (unsigned long)sizeof(iframe_t));

    /* 1. Verify synchronous exception handling & iframe_t save/restore */
    register uint64_t svc_arg __asm__("x0") = 0x12345678UL;
    __asm__ volatile("svc #0x42" : "+r"(svc_arg) :: "memory");
    dprintf(INFO, "[init] SVC returned x0=0x%lx (expected 0xab050002)\n", svc_arg);

    /* 2. Initialize GICv2 Distributor & CPU 0 interface + Timer */
    gic_dist_init();
    gic_cpu_init();
    timer_init_percpu(20); /* 20 Hz (50ms) timer ticks */

    /* 3. Wake secondary CPUs */
    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
        psci_cpu_on(cpu, (uintptr_t)_start);
    }
    while (__atomic_load_n(&secondary_cpu_ready_count, __ATOMIC_ACQUIRE) < (SMP_MAX_CPUS - 1)) {
        __asm__ volatile("wfe");
    }

    /* 4. Enable IRQs on CPU 0 and wait until all 4 CPUs receive 3 timer ticks */
    arch_enable_ints();

    while (timer_get_ticks(0) < 3 || timer_get_ticks(1) < 3 ||
           timer_get_ticks(2) < 3 || timer_get_ticks(3) < 3) {
        __asm__ volatile("wfi");
    }

    dprintf(INFO, "[lk_main] All 4 CPUs received >= 3 timer interrupts! Lesson 02 complete.\n");
    thread_become_idle();
}
