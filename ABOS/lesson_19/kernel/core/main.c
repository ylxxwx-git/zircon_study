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
extern void _start(void);
extern uint16_t ab_alloc_asid(void);

static volatile uint32_t secondary_cpu_ready_count = 0;

/* Issue PSCI_CPU_ON (0xC4000003) via HVC #0 to wake secondary cores */
static int psci_cpu_on(uint64_t core_id, uintptr_t entry_point) {
    register uint64_t x0 __asm__("x0") = 0xC4000003UL;
    register uint64_t x1 __asm__("x1") = core_id;
    register uint64_t x2 __asm__("x2") = (uint64_t)entry_point;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (int)x0;
}

void arm64_secondary_entry(uint32_t cpu_num) {
    thread_secondary_cpu_init();
    gic_cpu_init();
    timer_init_percpu(100);
    dprintf(INFO, "[cpu %u] secondary scheduler active (EL%u)\n",
            cpu_num, arch_boot_get_el());
    __atomic_fetch_add(&secondary_cpu_ready_count, 1, __ATOMIC_SEQ_CST);
    __asm__ volatile("sev");
    thread_become_idle();
}

void lk_main(uint64_t dtb_paddr) {
    dlog_init_early();
    thread_init_early();

    dprintf(INFO, "\n==================================================================\n");
    dprintf(INFO, "Welcome to ABOS (Zircon-Style Microkernel)\n");
    dprintf(INFO, "Lesson 19: Hierarchical Subdirectory Build & Common User Linker\n");
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

    dprintf(INFO, "[smp] Booting secondary CPUs (1..%u) via PSCI_CPU_ON...\n",
            SMP_MAX_CPUS - 1);
    for (uint32_t cpu = 1; cpu < SMP_MAX_CPUS; cpu++) {
        psci_cpu_on(cpu, (uintptr_t)_start);
    }
    while (__atomic_load_n(&secondary_cpu_ready_count, __ATOMIC_ACQUIRE) < (SMP_MAX_CPUS - 1)) {
        __asm__ volatile("wfe");
    }
    dprintf(INFO, "[smp] All %u CPUs online and scheduling in EL1!\n", SMP_MAX_CPUS);

    ab_object_t *root_job = NULL, *root_log = NULL, *root_res = NULL;
    ab_job_dispatcher_create(&root_job);
    ab_log_dispatcher_create(0, &root_log);
    ab_resource_dispatcher_create(0, &root_res);

    ab_process_t *userboot_proc = ab_process_create_kernel("userboot", ab_alloc_asid());
    ab_job_add_process((ab_job_dispatcher_t *)root_job, userboot_proc);
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

    dprintf(INFO, "[lk_main] ABOS Lesson 19 complete!\n");
    thread_become_idle();
}
