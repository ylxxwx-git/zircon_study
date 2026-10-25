/* =========================================================================
 * timer.h - ARMv8 Generic Timer & Monotonic Clock (zx_time_t)
 * References:
 *   - zircon_org/kernel/dev/timer/arm_generic/arm_generic_timer.cpp
 * ========================================================================= */
#ifndef ABOS_TIMER_H
#define ABOS_TIMER_H

#include <stdint.h>

/* ARM Non-Secure Physical Timer PPI ID on QEMU virt */
#define TIMER_ARCH_PHYS_IRQ 30

typedef uint64_t ab_time_t;

void timer_init_percpu(uint32_t hz);
void timer_irq_handler(void);
uint64_t timer_get_ticks(uint32_t cpu);
ab_time_t current_time_ns(void);

#endif /* ABOS_TIMER_H */
