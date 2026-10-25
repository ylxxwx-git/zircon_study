/* =========================================================================
 * main.c - ABOS Lesson 04: Higher-Half Kernel & User VmAspace Isolation
 * ========================================================================= */
#include <stdint.h>
#include "debuglog.h"
#include "spinlock.h"
#include "exceptions.h"
#include "gic.h"
#include "timer.h"
#include "dtb.h"
#include "pmm.h"
#include "heap.h"
#include "mmu.h"

extern char __code_start[];
extern char _end[];

static void thread_become_idle(void) {
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void arm64_secondary_entry(uint32_t cpu_num) {
    (void)cpu_num;
    thread_become_idle();
}

void lk_main(uint64_t dtb_paddr) {
    dlog_init_early();

    dprintf(INFO, "\n==========================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 04: Higher-Half Kernel & VmAspace (TTBR0 / TTBR1)\n");
    dprintf(INFO, "==========================================================\n");
    dprintf(INFO, "[init] Executing in Higher-Half VA: lk_main=%p __code_start=%p _end=%p\n",
            (void *)(uintptr_t)lk_main, (void *)__code_start, (void *)_end);

    mmu_init();

    boot_info_t boot_info;
    dtb_parse(dtb_paddr, &boot_info);

    paddr_t kernel_end_pa = physmap_to_paddr(_end);
    pmm_init(boot_info.ram_base, boot_info.ram_size, kernel_end_pa,
             boot_info.dtb_paddr, boot_info.dtb_size);
    heap_init();

    /* Demonstrate Zircon-style VmAspace isolation across two processes */
    vm_aspace_t *aspace_a = vmm_create_user_aspace(1);
    vm_aspace_t *aspace_b = vmm_create_user_aspace(2);

    paddr_t pa_a = 0, pa_b = 0;
    pmm_alloc_page(VM_PAGE_STATE_OBJECT, &pa_a);
    pmm_alloc_page(VM_PAGE_STATE_OBJECT, &pa_b);

    /* Map the SAME virtual address (0x00400000) in both address spaces to DIFFERENT physical pages */
    vaddr_t test_va = 0x00400000ULL;
    vmm_map_page(aspace_a, test_va, pa_a, MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_WRITE);
    vmm_map_page(aspace_b, test_va, pa_b, MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_WRITE);

    /* Write distinct strings via each address space */
    vmm_switch_aspace(aspace_a);
    volatile uint64_t *ptr = (volatile uint64_t *)(uintptr_t)test_va;
    *ptr = 0xAAAAAAAAAAAAAAAAULL;

    vmm_switch_aspace(aspace_b);
    *ptr = 0xBBBBBBBBBBBBBBBBULL;

    /* Switch back to A and verify A's mapping at 0x00400000 was not overwritten by B */
    vmm_switch_aspace(aspace_a);
    uint64_t val_a = *ptr;

    vmm_switch_aspace(aspace_b);
    uint64_t val_b = *ptr;

    dprintf(INFO, "[vmm] Aspace A (ASID %u, TTBR0=%p): VA %p -> PA %p, value=0x%lx\n",
            aspace_a->asid, (void *)aspace_a->ttbr0_phys, (void *)test_va, (void *)pa_a, val_a);
    dprintf(INFO, "[vmm] Aspace B (ASID %u, TTBR0=%p): VA %p -> PA %p, value=0x%lx\n",
            aspace_b->asid, (void *)aspace_b->ttbr0_phys, (void *)test_va, (void *)pa_b, val_b);

    pmm_dump_state_counts();

    vmm_switch_aspace(NULL);
    vmm_destroy_aspace(aspace_a);
    vmm_destroy_aspace(aspace_b);
    pmm_free_page(pa_a);
    pmm_free_page(pa_b);

    dprintf(INFO, "[lk_main] Lesson 04 complete!\n");
    thread_become_idle();
}
