/* =========================================================================
 * thread.h - LK Kernel Thread (thread_t) & Preemptive Scheduler
 * References:
 *   - zircon_org/kernel/include/kernel/thread.h
 *   - zircon_org/kernel/arch/arm64/include/arch/arch_thread.h
 * ========================================================================= */
#ifndef ABOS_THREAD_H
#define ABOS_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include "mmu.h"
#include "timer.h"

#define MAX_THREADS       64
#define THREAD_STACK_SIZE (2 * PAGE_SIZE) /* 8KB per kernel thread stack */

typedef enum {
    THREAD_INITIAL = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_DEATH
} thread_state_t;

typedef int (*thread_start_routine)(void *arg);

/* Matches struct context_switch_frame in zircon_org/kernel/arch/arm64/thread.cpp */
typedef struct arm64_context_switch_frame {
    uint64_t tpidr_el0;
    uint64_t tpidrro_el0;
    uint64_t r19;
    uint64_t r20;
    uint64_t r21;
    uint64_t r22;
    uint64_t r23;
    uint64_t r24;
    uint64_t r25;
    uint64_t r26;
    uint64_t r27;
    uint64_t r28;
    uint64_t r29;
    uint64_t lr;
} context_switch_frame_t;

typedef struct thread {
    uint32_t             tid;
    char                 name[32];
    volatile thread_state_t state;
    int                  pinned_cpu;     /* -1 = any CPU, 0..3 = pinned */
    vaddr_t              kernel_sp;      /* Saved SP for arm64_context_switch */
    vaddr_t              stack_base;     /* Bottom of 8KB kernel stack */
    size_t               stack_size;
    thread_start_routine entry;
    void                *arg;
    int                  retcode;
    ab_time_t            wake_time_ns;   /* Deadline for SLEEPING / timed BLOCKED */
    int                  wait_status;    /* Status returned when woken from wait_queue */
    struct wait_queue   *blocking_wq;    /* Wait queue this thread is currently on */
    struct thread       *wait_next;      /* Intrusive link inside wait_queue_t */
    int                  detached;       /* 1 = ready for kernel stack/struct reaping on DEATH */
    vm_aspace_t         *aspace;         /* NULL for kernel threads; user VmAspace for EL0 */
    void                *process_obj;    /* ProcessDispatcher pointer (Lesson 07+) */
    void                *thread_obj;     /* ThreadDispatcher pointer (Lesson 07+) */
} thread_t;

extern void arm64_context_switch(vaddr_t *old_sp, vaddr_t new_sp);

void thread_init_early(void);
void thread_secondary_cpu_init(void);
thread_t *get_current_thread(void);
thread_t *thread_create(const char *name, thread_start_routine entry, void *arg);
void thread_detach(thread_t *t);
void thread_reap_dead(void);
void thread_set_pinned_cpu(thread_t *t, int cpu);
void thread_resume(thread_t *t);
void thread_yield(void);
void thread_preempt(void);
void thread_sleep_ns(ab_time_t delay_ns);
void thread_exit(int retcode);
void thread_timer_tick(void);
void thread_become_idle(void);

/* Internal scheduler lock helpers used by wait_queue_t (Lesson 06+) */
void sched_lock_acquire(uint64_t *irqstate_out);
void sched_lock_release(uint64_t irqstate);
void sched_reschedule_locked(void);

#endif /* ABOS_THREAD_H */
