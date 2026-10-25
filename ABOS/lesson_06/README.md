# ABOS Lesson 06: LK Blocking Synchronization — Wait Queues (`wait_queue_t`) & Events (`event_t`)

## Overview
Lesson 06 completes **Phase 1 (the LK Kernel Core)** by implementing Zircon's blocking synchronization primitives (`zircon_org/kernel/kernel/wait.cpp` and `event.cpp`), which underpin all Zircon object waiting (`ab_object_wait_one`, `ab_port_wait`, `ab_interrupt_wait`):

1. **Wait Queues (`wait_queue_t`)**:
   * Places calling threads into `THREAD_BLOCKED` (`wait_queue_block_locked`) with an optional nanosecond deadline (`deadline_ns`), consuming zero CPU cycles until woken via `wait_queue_wake_one_locked` or `wait_queue_wake_all_locked` (or timed out by `thread_timer_tick` returning `AB_ERR_TIMED_OUT`).
2. **LK Events (`event_t`)**:
   * Supports both **Broadcast** mode (`flags = 0`, waking all blocked waiters when signaled) and **Auto-Unsignal** mode (`EVENT_FLAG_AUTOUNSIGNAL`, waking exactly one waiter per `event_signal` and automatically clearing the signal).

## Building & Running
```bash
make
make run
```
