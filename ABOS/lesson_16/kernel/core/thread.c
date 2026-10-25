/* =========================================================================
 * thread.c - LK Threading & SMP Preemptive Round-Robin Scheduler
 * References:
 *   - zircon_org/kernel/kernel/thread.cpp
 *   - zircon_org/kernel/kernel/sched.cpp
 *   - zircon_org/kernel/arch/arm64/thread.cpp
 * ========================================================================= */
#include "thread.h"
#include "spinlock.h"
#include "heap.h"
#include "pmm.h"
#include "debuglog.h"

static spin_lock_t sched_lock = SPIN_LOCK_INITIAL_VALUE;
static thread_t *thread_table[MAX_THREADS];
static thread_t *current_threads[SMP_MAX_CPUS];
static thread_t  idle_threads[SMP_MAX_CPUS];
static volatile int need_resched[SMP_MAX_CPUS];
static uint32_t next_tid = 1;

static void copy_name(char *dst, const char *src, size_t max) {
    size_t i = 0;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

void sched_lock_acquire(uint64_t *irqstate_out) {
    spin_lock_irqsave(&sched_lock, irqstate_out);
}

void sched_lock_release(uint64_t irqstate) {
    spin_lock_irqrestore(&sched_lock, irqstate);
}

thread_t *get_current_thread(void) {
    return current_threads[arch_curr_cpu_num()];
}

static void initial_thread_trampoline(void) {
    /* Release sched_lock held across arm64_context_switch and enable IRQs */
    arch_spin_unlock(&sched_lock);
    arch_enable_ints();

    thread_t *cur = get_current_thread();
    int ret = 0;
    if (cur && cur->entry) {
        ret = cur->entry(cur->arg);
    }
    thread_exit(ret);
}

void thread_init_early(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        thread_table[i] = NULL;
    }
    for (uint32_t cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        thread_t *idle = &idle_threads[cpu];
        idle->tid        = 0;
        copy_name(idle->name, "idle", sizeof(idle->name));
        idle->state      = THREAD_RUNNING;
        idle->pinned_cpu = (int)cpu;
        idle->aspace     = NULL;
        current_threads[cpu] = idle;
        need_resched[cpu]    = 0;
    }
}

void thread_secondary_cpu_init(void) {
    uint32_t cpu = arch_curr_cpu_num();
    current_threads[cpu] = &idle_threads[cpu];
}

thread_t *thread_create(const char *name, thread_start_routine entry, void *arg) {
    thread_t *t = (thread_t *)kzalloc(sizeof(thread_t));
    if (!t) return NULL;

    paddr_t stack_pa = 0;
    if (!pmm_alloc_contiguous(THREAD_STACK_SIZE / PAGE_SIZE, VM_PAGE_STATE_WIRED, &stack_pa)) {
        kfree(t);
        return NULL;
    }

    t->stack_base  = (vaddr_t)paddr_to_physmap(stack_pa);
    t->stack_size  = THREAD_STACK_SIZE;
    t->entry       = entry;
    t->arg         = arg;
    t->state       = THREAD_INITIAL;
    t->pinned_cpu  = -1;
    t->aspace      = NULL;
    t->wait_next   = NULL;
    copy_name(t->name, name ? name : "kthread", sizeof(t->name));

    /* Set up initial context_switch_frame_t at the top of the 8KB stack */
    vaddr_t sp_top = (t->stack_base + t->stack_size) & ~15ULL;
    context_switch_frame_t *frame =
        (context_switch_frame_t *)(sp_top - sizeof(context_switch_frame_t));
    for (size_t i = 0; i < sizeof(*frame) / sizeof(uint64_t); i++) {
        ((uint64_t *)frame)[i] = 0;
    }
    frame->lr = (uint64_t)(uintptr_t)initial_thread_trampoline;
    t->kernel_sp = (vaddr_t)frame;

    uint64_t irqstate;
    spin_lock_irqsave(&sched_lock, &irqstate);
    t->tid = next_tid++;
    int placed = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i] == NULL) {
            thread_table[i] = t;
            placed = 1;
            break;
        }
    }
    spin_lock_irqrestore(&sched_lock, irqstate);

    if (!placed) {
        kfree(t);
        return NULL;
    }
    return t;
}

void thread_set_pinned_cpu(thread_t *t, int cpu) {
    if (t) t->pinned_cpu = cpu;
}

void thread_resume(thread_t *t) {
    if (!t) return;
    uint64_t irqstate;
    spin_lock_irqsave(&sched_lock, &irqstate);
    if (t->state == THREAD_INITIAL || t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        t->state = THREAD_READY;
    }
    spin_lock_irqrestore(&sched_lock, irqstate);
    __asm__ volatile("sev");
}

/* Must be called with sched_lock held and IRQs disabled */
void sched_reschedule_locked(void) {
    uint32_t cpu = arch_curr_cpu_num();
    thread_t *prev = current_threads[cpu];
    thread_t *next = NULL;

    /* Find the current thread's index in thread_table for round-robin fairness */
    int start_idx = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (thread_table[i] == prev) {
            start_idx = (i + 1) % MAX_THREADS;
            break;
        }
    }

    for (int step = 0; step < MAX_THREADS; step++) {
        int idx = (start_idx + step) % MAX_THREADS;
        thread_t *cand = thread_table[idx];
        if (!cand) continue;
        if (cand->state == THREAD_READY &&
            (cand->pinned_cpu == -1 || cand->pinned_cpu == (int)cpu)) {
            next = cand;
            break;
        }
    }

    if (!next) {
        if (prev->state == THREAD_RUNNING) {
            return; /* Keep running prev */
        }
        next = &idle_threads[cpu];
    }

    if (next == prev) {
        next->state = THREAD_RUNNING;
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
    }
    next->state = THREAD_RUNNING;
    current_threads[cpu] = next;

    if (prev->aspace != next->aspace) {
        vmm_switch_aspace(next->aspace);
    }

    arm64_context_switch(&prev->kernel_sp, next->kernel_sp);
}

void thread_yield(void) {
    uint64_t irqstate;
    spin_lock_irqsave(&sched_lock, &irqstate);
    sched_reschedule_locked();
    spin_lock_irqrestore(&sched_lock, irqstate);
}

void thread_preempt(void) {
    uint32_t cpu = arch_curr_cpu_num();
    if (need_resched[cpu]) {
        need_resched[cpu] = 0;
        thread_yield();
    }
}

void thread_sleep_ns(ab_time_t delay_ns) {
    uint64_t irqstate;
    spin_lock_irqsave(&sched_lock, &irqstate);
    thread_t *cur = get_current_thread();
    cur->wake_time_ns = current_time_ns() + delay_ns;
    cur->state = THREAD_SLEEPING;
    sched_reschedule_locked();
    spin_lock_irqrestore(&sched_lock, irqstate);
}

void thread_exit(int retcode) {
    uint64_t irqstate;
    spin_lock_irqsave(&sched_lock, &irqstate);
    thread_t *cur = get_current_thread();
    cur->retcode = retcode;
    cur->state   = THREAD_DEATH;
    sched_reschedule_locked();
    spin_lock_irqrestore(&sched_lock, irqstate);
    for (;;) __asm__ volatile("wfi");
}

void thread_timer_tick(void) {
    uint32_t cpu = arch_curr_cpu_num();
    ab_time_t now = current_time_ns();

    uint64_t irqstate;
    spin_lock_irqsave(&sched_lock, &irqstate);

    for (int i = 0; i < MAX_THREADS; i++) {
        thread_t *t = thread_table[i];
        if (!t) continue;
        if (t->state == THREAD_SLEEPING && now >= t->wake_time_ns) {
            t->state = THREAD_READY;
            __asm__ volatile("sev");
        } else if (t->state == THREAD_BLOCKED && t->wake_time_ns != 0 && now >= t->wake_time_ns) {
            /* Timed wait_queue wakeup (used in Lesson 06+) */
            t->wake_time_ns = 0;
            t->wait_status  = -1; /* AB_ERR_TIMED_OUT */
            t->state        = THREAD_READY;
            __asm__ volatile("sev");
        }
    }

    need_resched[cpu] = 1;
    spin_lock_irqrestore(&sched_lock, irqstate);
}

void thread_become_idle(void) {
    arch_enable_ints();
    for (;;) {
        thread_yield();
        __asm__ volatile("wfi");
    }
}
