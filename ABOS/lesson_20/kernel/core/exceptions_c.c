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

    /* EC 0x20 (Instruction Abort) / 0x24 (Data Abort) from EL0 -> VMO demand paging! */
    if (ec == 0x24 || ec == 0x20) {
        uint32_t fsc = iss & 0x3FU;
        /* Only demand-page on L1/L2/L3 Translation Faults (FSC 0x05..0x07) */
        if ((fsc & 0x3CU) == 0x04U) {
            int is_write = (ec == 0x24 && ((iss >> 6) & 1U)) ? 1 : 0;
            int is_exec  = (ec == 0x20) ? 1 : 0;
            ab_process_t *proc = get_current_process();
            if (proc && ab_process_handle_page_fault(proc, (vaddr_t)far, is_write, is_exec) == 0) {
                return;
            }
        }
    }

    /* Isolate EL0 crashes: terminate only the faulting user process, keep kernel alive! */
    if (exception_flags & ARM64_EXCEPTION_FLAG_LOWER_EL) {
        ab_process_t *proc = get_current_process();
        dprintf(CRITICAL, "\n[EXCEPTION] Terminating faulting EL0 process '%s' (koid=%lu) on CPU %u!\n",
                proc ? proc->name : "unknown", proc ? proc->base.koid : 0UL, arch_curr_cpu_num());
        dprintf(CRITICAL, "  ESR=0x%x (EC=0x%x, ISS=0x%x) FAR=%p\n",
                esr, ec, iss, (void *)far);
        dump_iframe(iframe);
        arch_enable_ints();
        ab_process_exit_current(-1);
    }

    dprintf(CRITICAL, "\n[PANIC] Unhandled EL1 Kernel Sync Exception on CPU %u!\n",
            arch_curr_cpu_num());
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
    } else {
        ab_interrupt_dispatch_hw_irq(irq);
    }

    gic_end_interrupt(iar);
    thread_preempt();
}
