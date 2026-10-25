/* =========================================================================
 * main.c - ABOS Lesson 01: LK-Style Kernel Initialization & SMP Bringup
 * References:
 *   - zircon_org/kernel/top/main.cpp (lk_main)
 *   - zircon_org/kernel/arch/arm64/mp.cpp
 * ========================================================================= */
#include <stdint.h>
#include "debuglog.h"
#include "spinlock.h"

/* Linker symbols */
extern char __code_start[];
extern char _text_end[];
extern char _data_start[];
extern char _bss_start[];
extern char _bss_end[];
extern char _end[];
extern void _start(void);

/* Populated by start.S on CPU 0 before .bss is cleared (stored in .data) */
extern uint64_t boot_dtb_paddr;
extern uint64_t arch_boot_el;

static volatile uint32_t secondary_cpu_ready_count = 0;

/* Issue PSCI_CPU_ON (0xC4000003) via HVC #0 */
static int psci_cpu_on(uint64_t core_id, uintptr_t entry_point) {
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = core_id;
    register uint64_t x2 __asm__("x2") = (uint64_t)entry_point;
    register uint64_t x3 __asm__("x3") = 0;

    __asm__ volatile(
        "hvc #0"
        : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
        :
        : "memory");
    return (int)x0;
}

static void thread_become_idle(void) {
    uint32_t cpu = arch_curr_cpu_num();
    dprintf(SPEW, "[cpu %u] entering idle loop (EL%u)\n", cpu, arch_boot_get_el());
    for (;;) {
        __asm__ volatile("wfe");
    }
}

/* Entry point for Secondary CPUs (Cores 1..3) */
void arm64_secondary_entry(uint32_t cpu_num) {
    dprintf(INFO, "[cpu %u] arm64_secondary_entry: online at EL%u\n",
            cpu_num, arch_boot_get_el());

    __atomic_fetch_add(&secondary_cpu_ready_count, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sev");

    thread_become_idle();
}

/* Primary CPU (Core 0) LK Main Entry Point (modeled after zircon_org/kernel/top/main.cpp) */
void lk_main(void) {
    /* 1. Early debuglog console init */
    dlog_init_early();

    dprintf(INFO, "\n========================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 01: LK Boot, EL Transition & SMP\n");
    dprintf(INFO, "========================================\n");

    dprintf(INFO, "[init] Reset Exception Level : EL%lu -> Running at EL%u\n",
            arch_boot_el, arch_boot_get_el());
    dprintf(INFO, "[init] Boot DTB / ZBI PAddr  : %p\n", (void *)boot_dtb_paddr);
    dprintf(INFO, "[init] Kernel Image Layout   : %p .. %p\n",
            (void *)__code_start, (void *)_end);
    dprintf(INFO, "[init] BSS Section Zeroed    : %p .. %p\n",
            (void *)_bss_start, (void *)_bss_end);

    /* 2. Bring up secondary CPUs via PSCI */
    dprintf(INFO, "[smp] Booting secondary CPUs (1..%u) via PSCI_CPU_ON...\n",
            SMP_MAX_CPUS - 1);

    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
        psci_cpu_on(cpu, (uintptr_t)_start);
    }

    /* Wait for all 3 secondary CPUs to check in */
    while (__atomic_load_n(&secondary_cpu_ready_count, __ATOMIC_ACQUIRE) < (SMP_MAX_CPUS - 1)) {
        __asm__ volatile("wfe");
    }

    dprintf(INFO, "[smp] All %u CPUs are online in EL1!\n", SMP_MAX_CPUS);
    dprintf(INFO, "[lk_main] Lesson 01 complete. Entering idle loop.\n");

    thread_become_idle();
}
