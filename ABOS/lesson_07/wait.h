/* =========================================================================
 * wait.h - LK Wait Queues (wait_queue_t) & Events (event_t)
 * ========================================================================= */
#ifndef ABOS_WAIT_H
#define ABOS_WAIT_H

#include <stdint.h>
#include "thread.h"

#define EVENT_FLAG_AUTOUNSIGNAL (1U << 0)

#ifndef AB_OK
#define AB_OK                 0
#define AB_ERR_TIMED_OUT     -21
#define AB_ERR_CANCELED      -23
#endif

typedef struct wait_queue {
    thread_t *head;
    thread_t *tail;
    uint32_t  count;
} wait_queue_t;

typedef struct event {
    volatile int signaled;
    uint32_t     flags;
    wait_queue_t wait;
} event_t;

void wait_queue_init(wait_queue_t *wq);
int  wait_queue_block_locked(wait_queue_t *wq, ab_time_t deadline_ns);
int  wait_queue_wake_one_locked(wait_queue_t *wq, int wait_status);
int  wait_queue_wake_all_locked(wait_queue_t *wq, int wait_status);

void event_init(event_t *e, int initial, uint32_t flags);
int  event_wait_deadline(event_t *e, ab_time_t deadline_ns);
int  event_wait(event_t *e);
int  event_signal(event_t *e);
void event_unsignal(event_t *e);

#endif /* ABOS_WAIT_H */
