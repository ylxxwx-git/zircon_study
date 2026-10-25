/* =========================================================================
 * exceptions_c.c - Exception, Syscall & VMO Demand-Paging Handler
 * ========================================================================= */
#include "exceptions.h"
#include "debuglog.h"
#include "spinlock.h"
#include "gic.h"
#include "timer.h"
#include "thread.h"
#include "object.h"
#include "syscalls.h"

void dump_iframe(const iframe_t *iframe) {
    dprintf(CRITICAL, "iframe %p (CPU %u):\n", (void *)iframe, arch_curr_cpu_num());
    dprintf(CRITICAL, " x0  %p  x1  %p  x2  %p  x3  %p\n",
            (void *)iframe->r[0], (void *)iframe->r[1],
            (void *)iframe->r[2], (void *)iframe->r[3]);
    dprintf(CRITICAL, " elr %p  spsr 0x%lx  usp %p\n",
            (void *)iframe->elr, iframe->spsr, (void *)iframe->usp);
}

void arm64_sync_exception(iframe_t *iframe, uint32_t exception_flags, uint32_t esr) {
    uint32_t ec  = (esr >> 26) & 0x3F;
    uint32_t iss = esr & 0x1FFFFFF;
    uint64_t far;
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    /* EC 0x15 = SVC instruction from AArch64 (System Call) */
    if (ec == 0x15 && (exception_flags & ARM64_EXCEPTION_FLAG_LOWER_EL)) {
        arch_enable_ints();
        syscall_dispatch(iframe);
        arch_disable_ints();
        return;
    }

    /* EC 0x24 = Data Abort from Lower EL (EL0) -> VMO demand paging! */
    if (ec == 0x24) {
        int is_write = ((iss >> 6) & 1U) ? 1 : 0;
        ab_process_t *proc = get_current_process();
        if (proc && ab_process_handle_page_fault(proc, (vaddr_t)far, is_write) == 0) {
            return;
        }
    }

    dprintf(CRITICAL, "\n[PANIC] Unhandled Sync Exception on CPU %u (from %s)!\n",
            arch_curr_cpu_num(),
            (exception_flags & ARM64_EXCEPTION_FLAG_LOWER_EL) ? "EL0" : "EL1");
    dprintf(CRITICAL, "  ESR=0x%x (EC=0x%x, ISS=0x%x) FAR=%p\n",
            esr, ec, iss, (void *)far);
    dump_iframe(iframe);
    for (;;) __asm__ volatile("wfe");
}

void arm64_irq_exception(iframe_t *iframe, uint32_t exception_flags) {
    (void)iframe;
    (void)exception_flags;

    uint32_t iar = gic_acknowledge_interrupt();
    uint32_t irq = iar & 0x3FF;
    if (irq >= 1020) return;

    if (irq == TIMER_ARCH_PHYS_IRQ) {
        timer_irq_handler();
    }

    gic_end_interrupt(iar);
    thread_preempt();
}
