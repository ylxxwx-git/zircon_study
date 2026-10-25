/* =========================================================================
 * main.c - ABOS Lesson 05: LK Threads (thread_t) & Preemptive Multitasking
 * References:
 *   - zircon_org/kernel/top/main.cpp (lk_main, bootstrap2)
 *   - zircon_org/kernel/kernel/thread.cpp
 * ========================================================================= */
#include <stdint.h>
#include "debuglog.h"
#include "spinlock.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "dtb.h"
#include "pmm.h"
#include "heap.h"
#include "mmu.h"
#include "thread.h"

extern char _end[];
extern void _start(void);

static volatile uint32_t worker_finished_count = 0;

static int psci_cpu_on(uint64_t core_id, uintptr_t entry_point) {
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = core_id;
    register uint64_t x2 __asm__("x2") = (uint64_t)entry_point;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (int)x0;
}

static int worker_thread_entry(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    thread_t *self = get_current_thread();

    for (int step = 1; step <= 3; step++) {
        dprintf(INFO, "[thread '%s' tid=%u] step %d/3 on CPU %u (uptime=%lu ms)\n",
                self->name, self->tid, step, arch_curr_cpu_num(),
                current_time_ns() / 1000000ULL);
        /* Sleep 30ms between steps so scheduler preempts & interleaves threads */
        thread_sleep_ns(30000000ULL);
    }

    dprintf(INFO, "[thread '%s' tid=%u] worker #%lu exiting cleanly\n",
            self->name, self->tid, id);
    __atomic_fetch_add(&worker_finished_count, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static int bootstrap2(void *arg) {
    (void)arg;
    dprintf(INFO, "[bootstrap2] Running in thread '%s' (tid=%u) on CPU %u\n",
            get_current_thread()->name, get_current_thread()->tid, arch_curr_cpu_num());

    /* Spawn 4 concurrent LK kernel threads across the SMP pool */
    thread_t *w1 = thread_create("worker-1", worker_thread_entry, (void *)1UL);
    thread_t *w2 = thread_create("worker-2", worker_thread_entry, (void *)2UL);
    thread_t *w3 = thread_create("worker-3", worker_thread_entry, (void *)3UL);
    thread_t *w4 = thread_create("worker-4", worker_thread_entry, (void *)4UL);

    thread_resume(w1);
    thread_resume(w2);
    thread_resume(w3);
    thread_resume(w4);

    while (__atomic_load_n(&worker_finished_count, __ATOMIC_ACQUIRE) < 4) {
        thread_sleep_ns(20000000ULL);
    }

    dprintf(INFO, "[bootstrap2] All 4 LK worker threads completed! Lesson 05 complete.\n");
    return 0;
}

void arm64_secondary_entry(uint32_t cpu_num) {
    thread_secondary_cpu_init();
    gic_cpu_init();
    timer_init_percpu(100); /* 100 Hz (10ms) preemptive timer tick */
    dprintf(INFO, "[cpu %u] secondary scheduler active\n", cpu_num);
    thread_become_idle();
}

void lk_main(uint64_t dtb_paddr) {
    dlog_init_early();
    thread_init_early();

    dprintf(INFO, "\n==========================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 05: LK Threads (thread_t) & Preemptive Scheduler\n");
    dprintf(INFO, "==========================================================\n");

    mmu_init();

    boot_info_t boot_info;
    dtb_parse(dtb_paddr, &boot_info);
    pmm_init(boot_info.ram_base, boot_info.ram_size, physmap_to_paddr(_end),
             boot_info.dtb_paddr, boot_info.dtb_size);
    heap_init();

    gic_dist_init();
    gic_cpu_init();
    timer_init_percpu(100); /* 100 Hz (10ms) preemptive timer tick */

    /* Create Zircon's bootstrap2 completion thread (like zircon_org/kernel/top/main.cpp) */
    thread_t *bs2 = thread_create("bootstrap2", bootstrap2, NULL);
    thread_resume(bs2);

    /* Wake secondary CPUs to participate in SMP thread scheduling */
    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
        psci_cpu_on(cpu, 0x40080000UL);
    }

    thread_become_idle();
}
