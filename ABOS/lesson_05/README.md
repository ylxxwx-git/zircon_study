# ABOS Lesson 05: LK Kernel Threads (`thread_t`), `arm64_context_switch` & Preemptive Scheduler

## Overview
Lesson 05 implements Zircon's LK threading and SMP scheduling layer (`zircon_org/kernel/kernel/thread.cpp`, `sched.cpp`, `arch/arm64/asm.S`, and `top/main.cpp`):

1. **`arm64_context_switch` (`switch.S`)**:
   * Saves and restores callee-saved registers (`x19`–`x30`) plus thread-local storage registers (`tpidr_el0`, `tpidrro_el0`) on each thread's 8KB kernel stack.
2. **LK `thread_t` Lifecycle (`thread.h`, `thread.c`)**:
   * Implements `thread_init_early`, `thread_create`, `thread_resume`, `thread_yield`, `thread_preempt`, `thread_sleep_ns`, and `thread_exit`.
   * Automatically switches `vm_aspace_t` (`TTBR0_EL1`) during context switches when threads belong to different user processes.
3. **`lk_main` $\rightarrow$ `bootstrap2` Pattern (`main.c`)**:
   * Just like `zircon_org/kernel/top/main.cpp`, `lk_main()` initializes early subsystems, spawns the `bootstrap2` thread, wakes secondary CPUs, and becomes CPU 0's idle thread while `bootstrap2` and worker threads execute preemptively across all 4 CPUs.

## Building & Running
```bash
make
make run
```
