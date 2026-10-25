/* =========================================================================
 * wait.c - LK Wait Queues (wait_queue_t) & Events (event_t) Implementation
 * References:
 *   - zircon_org/kernel/kernel/wait.cpp
 *   - zircon_org/kernel/kernel/event.cpp
 * ========================================================================= */
#include "wait.h"
#include "spinlock.h"
#include "debuglog.h"

void wait_queue_init(wait_queue_t *wq) {
    wq->head  = NULL;
    wq->tail  = NULL;
    wq->count = 0;
}

static void wq_remove_thread_locked(wait_queue_t *wq, thread_t *target) {
    thread_t *prev = NULL;
    thread_t *curr = wq->head;
    while (curr) {
        if (curr == target) {
            if (prev) {
                prev->wait_next = curr->wait_next;
            } else {
                wq->head = curr->wait_next;
            }
            if (wq->tail == curr) {
                wq->tail = prev;
            }
            curr->wait_next = NULL;
            if (wq->count > 0) wq->count--;
            return;
        }
        prev = curr;
        curr = curr->wait_next;
    }
}

int wait_queue_block_locked(wait_queue_t *wq, ab_time_t deadline_ns) {
    thread_t *cur = get_current_thread();

    if (deadline_ns != 0 && current_time_ns() >= deadline_ns) {
        return AB_ERR_TIMED_OUT;
    }

    cur->wait_status  = AB_OK;
    cur->wake_time_ns = deadline_ns;
    cur->wait_next    = NULL;
    cur->state        = THREAD_BLOCKED;

    if (wq->tail) {
        wq->tail->wait_next = cur;
        wq->tail = cur;
    } else {
        wq->head = wq->tail = cur;
    }
    wq->count++;

    sched_reschedule_locked();

    /* If woken by timer timeout, ensure we are removed from wq */
    wq_remove_thread_locked(wq, cur);
    return cur->wait_status;
}

int wait_queue_wake_one_locked(wait_queue_t *wq, int wait_status) {
    while (wq->head) {
        thread_t *t = wq->head;
        wq->head = t->wait_next;
        if (!wq->head) wq->tail = NULL;
        t->wait_next = NULL;
        if (wq->count > 0) wq->count--;

        if (t->state == THREAD_BLOCKED) {
            t->wait_status  = wait_status;
            t->wake_time_ns = 0;
            t->state        = THREAD_READY;
            __asm__ volatile("sev");
            return 1;
        }
    }
    return 0;
}

int wait_queue_wake_all_locked(wait_queue_t *wq, int wait_status) {
    int woken = 0;
    while (wait_queue_wake_one_locked(wq, wait_status)) {
        woken++;
    }
    return woken;
}

void event_init(event_t *e, int initial, uint32_t flags) {
    e->signaled = initial ? 1 : 0;
    e->flags    = flags;
    wait_queue_init(&e->wait);
}

int event_wait_deadline(event_t *e, ab_time_t deadline_ns) {
    uint64_t irqstate;
    sched_lock_acquire(&irqstate);

    int ret = AB_OK;
    if (e->signaled) {
        if (e->flags & EVENT_FLAG_AUTOUNSIGNAL) {
            e->signaled = 0;
        }
    } else {
        ret = wait_queue_block_locked(&e->wait, deadline_ns);
    }

    sched_lock_release(irqstate);
    return ret;
}

int event_wait(event_t *e) {
    return event_wait_deadline(e, 0);
}

int event_signal(event_t *e) {
    uint64_t irqstate;
    sched_lock_acquire(&irqstate);

    int woken = 0;
    if (!e->signaled) {
        if (e->flags & EVENT_FLAG_AUTOUNSIGNAL) {
            woken = wait_queue_wake_one_locked(&e->wait, AB_OK);
            if (woken == 0) {
                e->signaled = 1;
            }
        } else {
            e->signaled = 1;
            woken = wait_queue_wake_all_locked(&e->wait, AB_OK);
        }
    }

    sched_lock_release(irqstate);
    return woken;
}

void event_unsignal(event_t *e) {
    uint64_t irqstate;
    sched_lock_acquire(&irqstate);
    e->signaled = 0;
    sched_lock_release(irqstate);
}
