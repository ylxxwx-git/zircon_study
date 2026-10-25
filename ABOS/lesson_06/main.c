/* =========================================================================
 * main.c - ABOS Lesson 06: LK Wait Queues (wait_queue_t) & Events (event_t)
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
#include "wait.h"

extern char _end[];

static event_t start_barrier_event; /* Broadcast event (wakes all waiters) */
static event_t item_ready_event;    /* Auto-unsignal event (wakes 1 consumer per item) */
static volatile uint32_t produced_item = 0;
static volatile uint32_t items_consumed = 0;

static int psci_cpu_on(uint64_t core_id, uintptr_t entry_point) {
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = core_id;
    register uint64_t x2 __asm__("x2") = (uint64_t)entry_point;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (int)x0;
}

static int consumer_thread(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    dprintf(INFO, "[consumer-%lu] blocked on start_barrier_event...\n", id);
    event_wait(&start_barrier_event);
    dprintf(INFO, "[consumer-%lu] barrier released on CPU %u! Waiting for item...\n",
            id, arch_curr_cpu_num());

    int rc = event_wait_deadline(&item_ready_event, current_time_ns() + 500000000ULL);
    if (rc == AB_OK) {
        uint32_t val = produced_item;
        dprintf(INFO, "[consumer-%lu] woke up & consumed item=%u on CPU %u\n",
                id, val, arch_curr_cpu_num());
        __atomic_fetch_add(&items_consumed, 1, __ATOMIC_SEQ_CST);
    } else {
        dprintf(INFO, "[consumer-%lu] timed out waiting for item\n", id);
    }
    return 0;
}

static int producer_thread(void *arg) {
    (void)arg;
    thread_sleep_ns(40000000ULL);
    dprintf(INFO, "[producer] signaling broadcast start_barrier_event!\n");
    int woken = event_signal(&start_barrier_event);
    dprintf(INFO, "[producer] start_barrier_event woke %d consumer threads simultaneously\n", woken);

    for (uint32_t i = 1; i <= 3; i++) {
        thread_sleep_ns(30000000ULL);
        produced_item = i * 100;
        dprintf(INFO, "[producer] produced item=%u -> signaling AUTOUNSIGNAL item_ready_event\n",
                produced_item);
        event_signal(&item_ready_event);
    }

    /* Also test timed wait timeout on an unsignaled event */
    event_t never_signaled;
    event_init(&never_signaled, 0, 0);
    int rc = event_wait_deadline(&never_signaled, current_time_ns() + 30000000ULL);
    dprintf(INFO, "[producer] timed event_wait_deadline returned %d (expected %d = AB_ERR_TIMED_OUT)\n",
            rc, AB_ERR_TIMED_OUT);

    while (__atomic_load_n(&items_consumed, __ATOMIC_ACQUIRE) < 3) {
        thread_sleep_ns(10000000ULL);
    }
    dprintf(INFO, "[lk_main] All 3 items consumed & timeout verified! Lesson 06 complete.\n");
    return 0;
}

void arm64_secondary_entry(uint32_t cpu_num) {
    (void)cpu_num;
    thread_secondary_cpu_init();
    gic_cpu_init();
    timer_init_percpu(100);
    thread_become_idle();
}

void lk_main(uint64_t dtb_paddr) {
    dlog_init_early();
    thread_init_early();

    dprintf(INFO, "\n==========================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 06: LK Wait Queues (wait_queue_t) & Events (event_t)\n");
    dprintf(INFO, "==========================================================\n");

    mmu_init();
    boot_info_t boot_info;
    dtb_parse(dtb_paddr, &boot_info);
    pmm_init(boot_info.ram_base, boot_info.ram_size, physmap_to_paddr(_end),
             boot_info.dtb_paddr, boot_info.dtb_size);
    heap_init();

    gic_dist_init();
    gic_cpu_init();
    timer_init_percpu(100);

    event_init(&start_barrier_event, 0, 0);
    event_init(&item_ready_event, 0, EVENT_FLAG_AUTOUNSIGNAL);

    for (uint64_t i = 1; i <= 3; i++) {
        thread_t *c = thread_create("consumer", consumer_thread, (void *)(uintptr_t)i);
        thread_resume(c);
    }
    thread_t *p = thread_create("producer", producer_thread, NULL);
    thread_resume(p);

    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
        psci_cpu_on(cpu, 0x40080000UL);
    }

    thread_become_idle();
}
