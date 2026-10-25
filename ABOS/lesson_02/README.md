# ABOS Lesson 02: Exception Vectors (`VBAR_EL1`), `iframe_t`, GICv2 & Generic Timer

## Overview
Building on Lesson 01, Lesson 02 implements Zircon's AArch64 exception and interrupt architecture (`zircon_org/kernel/arch/arm64/exceptions.S`, `exceptions_c.cpp`, `dev/interrupt/arm_gic/v2`, and `dev/timer/arm_generic`):

1. **Zircon's `iframe_t` (`exceptions.h`, `exceptions.S`)**:
   * Defines the 288-byte `struct arm64_iframe` (`x0`–`x29`, `lr`, `sp_el0`, `elr_el1`, `spsr_el1`, `mdscr_el1`) pushed onto the kernel stack during any synchronous exception or IRQ from EL1 or EL0.
   * Installs `arm64_el1_exception_base` into `VBAR_EL1` on all 4 CPUs.
2. **Interrupt-Safe Spinlocks (`spinlock.h`)**:
   * Adds `spin_lock_irqsave` and `spin_lock_irqrestore` (saving/restoring `DAIF` while masking IRQs) so `dprintf` can be safely called from both normal thread context and IRQ handlers without deadlocking.
3. **GICv2 & ARM Generic Timer (`gic.c`, `timer.c`)**:
   * Initializes the GICv2 Distributor (`0x08000000`) and per-CPU Interfaces (`0x08010000`).
   * Configures the per-CPU ARM Non-Secure Physical Timer (`CNTP`, PPI 30) and provides `current_time_ns()` (monotonic nanoseconds from `CNTPCT_EL0`, modeling Zircon's `current_time()`).

## Building & Running
```bash
make
make run
```
