/* =========================================================================
 * kernel/core/main.c - ABOS Lesson 20: Zircon-Style BOOTFS & Standalone Servers
 *
 * Initializes MMU, DTB, PMM, Heap, GICv2, Generic Timer, and Root Kernel
 * Objects (Job, Log, Resource, and BOOTFS VMO), then transitions to EL0
 * 'userboot' to load standalone console-srv.bin, fs-srv.bin, and shell.bin.
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
    dprintf(INFO, "Lesson 20: Standalone EL0 System Servers via Zircon-Style BOOTFS\n");
    dprintf(INFO, "==================================================================\n");

    mmu_init();
    boot_info_t boot_info;
    dtb_parse(dtb_paddr, &boot_info);
    pmm_init(boot_info.ram_base, boot_info.ram_size, physmap_to_paddr(_end),
             boot_info.dtb_paddr, boot_info.dtb_size,
             boot_info.initrd_start, boot_info.initrd_end);
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

    /* Create BOOTFS VMO from the QEMU -initrd RAM disk discovered via DTB */
    size_t bootfs_sz = (size_t)(boot_info.initrd_end - boot_info.initrd_start);
    const void *bootfs_kva = paddr_to_physmap(boot_info.initrd_start);
    ab_object_t *bootfs_vmo = NULL;
    ab_vmo_dispatcher_create(bootfs_sz, 0, &bootfs_vmo);
    {
        ab_vmo_dispatcher_t *v = (ab_vmo_dispatcher_t *)bootfs_vmo;
        const char *bname = "bootfs.tar";
        for (size_t i = 0; i < 11; i++) v->name[i] = bname[i];
    }
    ab_vmo_write(bootfs_vmo, bootfs_kva, 0, bootfs_sz);
    dprintf(INFO, "[bootfs] Created BOOTFS VMO (koid=%lu, %lu bytes from initrd PA %p) for userboot\n",
            bootfs_vmo->koid, (unsigned long)bootfs_sz, (void *)boot_info.initrd_start);

    ab_process_t *userboot_proc = ab_process_create_kernel("userboot", ab_alloc_asid());
    ab_job_add_process((ab_job_dispatcher_t *)root_job, userboot_proc);
    ab_object_t *ub_vmar = NULL;
    ab_vmar_dispatcher_create(userboot_proc, &ub_vmar);
    ab_object_t *ub_thr = NULL;
    ab_thread_dispatcher_create(userboot_proc, "userboot-main", &ub_thr);

    ab_object_t *ch0 = NULL, *ch1 = NULL;
    ab_channel_dispatcher_create(&ch0, &ch1);

    ab_handle_t job_h = 0, log_h = 0, res_h = 0, ub_vmar_h = 0, bootfs_h = 0, ch1_h = 0;
    handle_alloc(userboot_proc, root_job,   AB_RIGHTS_DEFAULT_JOB,      &job_h);
    handle_alloc(userboot_proc, root_log,   AB_RIGHTS_DEFAULT_LOG,      &log_h);
    handle_alloc(userboot_proc, root_res,   AB_RIGHTS_DEFAULT_RESOURCE, &res_h);
    handle_alloc(userboot_proc, ub_vmar,    AB_RIGHTS_DEFAULT_VMAR,     &ub_vmar_h);
    handle_alloc(userboot_proc, bootfs_vmo, AB_RIGHTS_DEFAULT_VMO,      &bootfs_h);
    handle_alloc(userboot_proc, ch1,        AB_RIGHTS_DEFAULT_CHANNEL,  &ch1_h);

    ab_handle_t boot_h[5] = { job_h, log_h, res_h, ub_vmar_h, bootfs_h };
    uint64_t bootfs_sz_msg = (uint64_t)bootfs_sz;
    ab_channel_write(userboot_proc, ch0, &bootfs_sz_msg, sizeof(bootfs_sz_msg), boot_h, 5);

    ab_object_release(root_job);
    ab_object_release(root_log);
    ab_object_release(root_res);
    ab_object_release(ub_vmar);
    ab_object_release(bootfs_vmo);
    ab_object_release(ch0);
    ab_object_release(ch1);

    ab_process_start_user(NULL, userboot_proc, (ab_thread_dispatcher_t *)ub_thr,
                          USER_KVA_TO_UVA(userboot_entry), 0, ch1_h, 0);

    arch_enable_ints();
    while (((ab_thread_dispatcher_t *)ub_thr)->kthread->state != THREAD_DEATH) {
        thread_yield();
        __asm__ volatile("wfi");
    }

    dprintf(INFO, "[lk_main] ABOS Lesson 20 complete!\n");
    thread_become_idle();
}
