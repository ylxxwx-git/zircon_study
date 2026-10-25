/* =========================================================================
 * kernel/main.c - ABOS Lesson 16: Structured Microkernel Initialization
 *
 * Initializes MMU, DTB, PMM, Heap, GICv2, Generic Timer, and Root Kernel
 * Objects (Job, Log, Resource), then transitions to EL0 'userboot'.
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
#include "thread.h"
#include "wait.h"
#include "object.h"
#include "syscalls.h"
#include "ipc_proto.h"

extern char _end[];
extern uint16_t ab_alloc_asid(void);

void arm64_secondary_entry(uint32_t cpu_num) {
    (void)cpu_num;
    thread_secondary_cpu_init();
    gic_cpu_init();
    timer_init_percpu(100);
    thread_become_idle();
}

void lk_main(uint64_t dtb_paddr) {
    dlog_init_early();
    thread_init_early();

    dprintf(INFO, "\n==================================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 16: Modular OS Architecture (include/ kernel/ mm/ drivers/ user/ image/)\n");
    dprintf(INFO, "==================================================================\n");

    mmu_init();
    boot_info_t boot_info;
    dtb_parse(dtb_paddr, &boot_info);
    pmm_init(boot_info.ram_base, boot_info.ram_size, physmap_to_paddr(_end),
             boot_info.dtb_paddr, boot_info.dtb_size);
    heap_init();

    gic_dist_init();
    gic_cpu_init();
    timer_init_percpu(100);

    ab_object_t *root_job = NULL, *root_log = NULL, *root_res = NULL;
    ab_job_dispatcher_create(&root_job);
    ab_log_dispatcher_create(0, &root_log);
    ab_resource_dispatcher_create(0, &root_res);

    ab_process_t *userboot_proc = ab_process_create_kernel("userboot", ab_alloc_asid());
    ab_object_t *ub_thr = NULL;
    ab_thread_dispatcher_create(userboot_proc, "userboot-main", &ub_thr);

    ab_object_t *ch0 = NULL, *ch1 = NULL;
    ab_channel_dispatcher_create(&ch0, &ch1);

    ab_handle_t job_h = 0, log_h = 0, res_h = 0, ch1_h = 0;
    handle_alloc(userboot_proc, root_job, AB_RIGHTS_DEFAULT_JOB, &job_h);
    handle_alloc(userboot_proc, root_log, AB_RIGHTS_DEFAULT_LOG, &log_h);
    handle_alloc(userboot_proc, root_res, AB_RIGHTS_DEFAULT_RESOURCE, &res_h);
    handle_alloc(userboot_proc, ch1,      AB_RIGHTS_DEFAULT_CHANNEL, &ch1_h);

    ab_handle_t boot_h[3] = { job_h, log_h, res_h };
    uint32_t tag = 0x424F4F54U;
    ab_channel_write(userboot_proc, ch0, &tag, sizeof(tag), boot_h, 3);

    ab_object_release(root_job);
    ab_object_release(root_log);
    ab_object_release(root_res);
    ab_object_release(ch0);
    ab_object_release(ch1);

    ab_process_start_user(NULL, userboot_proc, (ab_thread_dispatcher_t *)ub_thr,
                          USER_KVA_TO_UVA(userboot_entry), 0, ch1_h, 0);

    arch_enable_ints();
    while (((ab_thread_dispatcher_t *)ub_thr)->kthread->state != THREAD_DEATH) {
        thread_yield();
        __asm__ volatile("wfi");
    }

    dprintf(INFO, "[lk_main] ABOS Lesson 16 complete! Modular OS verified.\n");
    thread_become_idle();
}
