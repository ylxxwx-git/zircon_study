/* =========================================================================
 * main.c - ABOS Lesson 03: DTB Handoff, PMM Arena & Kernel Heap
 * References:
 *   - zircon_org/kernel/top/main.cpp (vm_init_preheap, heap_init)
 *   - zircon_org/kernel/vm/pmm.cpp
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

extern char _end[];
extern void _start(void);
extern uint64_t boot_dtb_paddr;

static void thread_become_idle(void) {
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void arm64_secondary_entry(uint32_t cpu_num) {
    (void)cpu_num;
    thread_become_idle();
}

void lk_main(void) {
    dlog_init_early();

    dprintf(INFO, "\n====================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 03: DTB Handoff, PMM Arena & Kernel Heap\n");
    dprintf(INFO, "====================================================\n");

    /* 1. Parse DTB passed by QEMU in x0 (at 0x40000000 when booting kernel.img) */
    boot_info_t boot_info;
    dtb_parse(boot_dtb_paddr, &boot_info);

    /* 2. Initialize Physical Memory Manager (vm_init_preheap) */
    pmm_init(boot_info.ram_base, boot_info.ram_size, (paddr_t)(uintptr_t)_end);
    pmm_dump_state_counts();

    /* 3. Initialize Kernel Heap (heap_init) */
    heap_init();

    /* 4. Test PMM page allocations with Zircon page states */
    paddr_t vmo_pa = 0, ipc_pa = 0, contig_pa = 0;
    vm_page_t *p1 = pmm_alloc_page(VM_PAGE_STATE_OBJECT, &vmo_pa);
    vm_page_t *p2 = pmm_alloc_page(VM_PAGE_STATE_IPC, &ipc_pa);
    vm_page_t *p_contig = pmm_alloc_contiguous(4, VM_PAGE_STATE_MMU, &contig_pa);

    dprintf(INFO, "[test] Allocated VMO page at %p (state=%s)\n",
            (void *)vmo_pa, vm_page_state_name((vm_page_state_t)p1->state));
    dprintf(INFO, "[test] Allocated IPC page at %p (state=%s)\n",
            (void *)ipc_pa, vm_page_state_name((vm_page_state_t)p2->state));
    dprintf(INFO, "[test] Allocated 4 contiguous MMU pages at %p (state=%s)\n",
            (void *)contig_pa, vm_page_state_name((vm_page_state_t)p_contig->state));
    pmm_dump_state_counts();

    /* 5. Test Kernel Heap (kmalloc / kzalloc / kfree + coalescing) */
    void *obj_a = kzalloc(128);
    void *obj_b = kzalloc(256);
    void *obj_c = kzalloc(512);
    dprintf(INFO, "[test] kmalloc objects: A=%p B=%p C=%p\n", obj_a, obj_b, obj_c);
    heap_dump_stats();

    kfree(obj_b);
    kfree(obj_a);
    kfree(obj_c);
    dprintf(INFO, "[test] After freeing A, B, C (coalesced back to 1 free block):\n");
    heap_dump_stats();

    /* Free PMM test pages */
    pmm_free_page(vmo_pa);
    pmm_free_page(ipc_pa);
    for (int i = 0; i < 4; i++) {
        pmm_free_page(contig_pa + (uint64_t)i * PAGE_SIZE);
    }
    pmm_dump_state_counts();

    dprintf(INFO, "[lk_main] Lesson 03 complete!\n");
    thread_become_idle();
}
