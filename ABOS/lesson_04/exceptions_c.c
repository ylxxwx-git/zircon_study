/* =========================================================================
 * exceptions_c.c - Synchronous Exception & IRQ Dispatching
 * References:
 *   - zircon_org/kernel/arch/arm64/exceptions_c.cpp
 * ========================================================================= */
#include "exceptions.h"
#include "debuglog.h"
#include "spinlock.h"
#include "gic.h"
#include "timer.h"

void dump_iframe(const iframe_t *iframe) {
    dprintf(CRITICAL, "iframe %p (CPU %u):\n", (void *)iframe, arch_curr_cpu_num());
    dprintf(CRITICAL, " x0  %p  x1  %p  x2  %p  x3  %p\n",
            (void *)iframe->r[0], (void *)iframe->r[1],
            (void *)iframe->r[2], (void *)iframe->r[3]);
    dprintf(CRITICAL, " x4  %p  x5  %p  x6  %p  x7  %p\n",
            (void *)iframe->r[4], (void *)iframe->r[5],
            (void *)iframe->r[6], (void *)iframe->r[7]);
    dprintf(CRITICAL, " x8  %p  x29 %p  lr  %p  usp %p\n",
            (void *)iframe->r[8], (void *)iframe->r[29],
            (void *)iframe->lr,   (void *)iframe->usp);
    dprintf(CRITICAL, " elr %p  spsr 0x%lx\n",
            (void *)iframe->elr, iframe->spsr);
}

void arm64_sync_exception(iframe_t *iframe, uint32_t exception_flags, uint32_t esr) {
    uint32_t ec  = (esr >> 26) & 0x3F;
    uint32_t iss = esr & 0x1FFFFFF;
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    /* EC 0x15 = SVC instruction execution in AArch64 */
    if (ec == 0x15) {
        dprintf(INFO, "[exception] CPU %u caught SVC #%u (from %s, ELR=%p, x0=0x%lx)\n",
                arch_curr_cpu_num(), iss & 0xFFFF,
                (exception_flags & ARM64_EXCEPTION_FLAG_LOWER_EL) ? "EL0" : "EL1",
                (void *)iframe->elr, iframe->r[0]);
        /* Return a confirmation value in x0 */
        iframe->r[0] = 0xAB050002UL;
        return;
    }

    dprintf(CRITICAL, "\n[PANIC] Unhandled Synchronous Exception on CPU %u!\n",
            arch_curr_cpu_num());
    dprintf(CRITICAL, "  ESR=0x%x (EC=0x%x, ISS=0x%x) FAR=%p\n",
            esr, ec, iss, (void *)far);
    dump_iframe(iframe);
    for (;;) {
        __asm__ volatile("wfe");
    }
}

void arm64_irq_exception(iframe_t *iframe, uint32_t exception_flags) {
    (void)iframe;
    (void)exception_flags;

    uint32_t iar = gic_acknowledge_interrupt();
    uint32_t irq = iar & 0x3FF;

    /* Spurious interrupt check (ID 1022 / 1023) */
    if (irq >= 1020) {
        return;
    }

    if (irq == TIMER_ARCH_PHYS_IRQ) {
        timer_irq_handler();
    } else {
        dprintf(INFO, "[irq] CPU %u received unhandled IRQ %u\n",
                arch_curr_cpu_num(), irq);
    }

    gic_end_interrupt(iar);
}
