/* =========================================================================
 * main.c - ABOS Lesson 12: Tasks (Job, Process, Thread) & userboot
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

extern char _text_start[];
extern char _rodata_end[];
extern char _end[];
extern uint16_t ab_alloc_asid(void);

#define USER_KVA_TO_UVA(kva) ((vaddr_t)((uintptr_t)(kva) - 0xFFFFFFFF80000000ULL))

/* Bootfs Directory Entry stored inside the Bootfs VMO */
typedef struct {
    uint32_t magic;        /* 0x424F4F54 ("BOOT") */
    char     app_name[32]; /* "hello-app" */
    uint64_t entry_uva;    /* Entry point UVA of child app */
    char     banner[64];   /* Payload banner from bootfs */
} bootfs_header_t;

#define BOOTFS_MAGIC 0x424F4F54U

static inline __attribute__((always_inline))
ab_status_t user_syscall4(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    register uint64_t x8 __asm__("x8") = num;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "r"(x8)
                     : "memory");
    return (ab_status_t)x0;
}

static size_t user_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void ulog(ab_handle_t log_h, const char *msg) {
    user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0, (uintptr_t)msg, user_strlen(msg));
}

/* =========================================================================
 * Child Application ('hello-app') spawned from EL0 by userboot!
 * Receives its bootstrap Channel handle in x0 (arg0).
 * ========================================================================= */
void hello_app_entry(uint64_t arg0) {
    ab_handle_t app_ch = (ab_handle_t)arg0;

    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, app_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);

    char banner[64];
    ab_handle_t recv_h[1];
    uint32_t act_b = 0, act_h = 0;
    ab_channel_read_args_t rargs = { banner, recv_h, sizeof(banner), 1, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, app_ch, 0, (uintptr_t)&rargs, 0);

    ab_handle_t log_h = recv_h[0];
    ulog(log_h, "[hello-app] Running in EL0! Spawned by userboot with dedicated Channel + Log capability!");
    ulog(log_h, banner);

    const char *done_msg = "[userboot] Received completion IPC from hello-app over Channel!";
    ab_channel_write_args_t wargs = { done_msg, (uint32_t)(user_strlen(done_msg) + 1), NULL, 0 };
    user_syscall4(SYS_CHANNEL_WRITE, app_ch, 0, (uintptr_t)&wargs, 0);

    user_syscall4(SYS_HANDLE_CLOSE, app_ch, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * Zircon-Style 'userboot' First User Process (running in EL0):
 * Receives ONLY a single bootstrap Channel handle in x0 (arg0)!
 * Reads Root Job, Root Log, and Bootfs VMO capabilities from the Channel,
 * parses the Bootfs VMO, and spawns 'hello-app' using SYS_PROCESS_CREATE,
 * SYS_THREAD_CREATE, and SYS_PROCESS_START!
 * ========================================================================= */
void userboot_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    /* 1. Read the kernel's bootstrap message containing 3 root capabilities */
    uint32_t boot_tag = 0, act_b = 0, act_h = 0;
    ab_handle_t boot_handles[3];
    ab_channel_read_args_t rargs = { &boot_tag, boot_handles, sizeof(boot_tag), 3, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);

    ab_handle_t root_job_h   = boot_handles[0];
    ab_handle_t root_log_h   = boot_handles[1];
    ab_handle_t bootfs_vmo_h = boot_handles[2];

    ulog(root_log_h, "[userboot] Dequeued bootstrap packet from kernel: RootJob, RootLog, BootfsVMO!");

    /* 2. Read the bootfs_header_t from the Bootfs VMO */
    bootfs_header_t hdr;
    user_syscall4(SYS_VMO_READ, bootfs_vmo_h, (uintptr_t)&hdr, 0, sizeof(hdr));
    if (hdr.magic == BOOTFS_MAGIC) {
        ulog(root_log_h, "[userboot] Parsed Bootfs VMO: found child executable 'hello-app'");
    }

    /* 3. Create child Process + Root VMAR under Root Job via SYS_PROCESS_CREATE */
    ab_handle_t child_proc_h = AB_HANDLE_INVALID;
    ab_handle_t child_vmar_h = AB_HANDLE_INVALID;
    ab_process_create_args_t pc_args = {
        hdr.app_name, user_strlen(hdr.app_name), &child_proc_h, &child_vmar_h
    };
    user_syscall4(SYS_PROCESS_CREATE, root_job_h, (uintptr_t)&pc_args, 0, 0);

    /* 4. Create initial Thread inside child Process via SYS_THREAD_CREATE */
    ab_handle_t child_thr_h = AB_HANDLE_INVALID;
    const char *thr_name = "hello-main";
    user_syscall4(SYS_THREAD_CREATE, child_proc_h, (uintptr_t)thr_name,
                  user_strlen(thr_name), (uintptr_t)&child_thr_h);

    /* 5. Create IPC Channel to pass to 'hello-app' and enqueue initial message + Log handle */
    ab_handle_t parent_ch = AB_HANDLE_INVALID, child_ch = AB_HANDLE_INVALID;
    user_syscall4(SYS_CHANNEL_CREATE, 0, (uintptr_t)&parent_ch, (uintptr_t)&child_ch, 0);

    ab_handle_t dup_log_h = AB_HANDLE_INVALID;
    user_syscall4(SYS_HANDLE_DUPLICATE, root_log_h, AB_RIGHT_SAME_RIGHTS,
                  (uintptr_t)&dup_log_h, 0);
    ab_channel_write_args_t wargs = {
        hdr.banner, (uint32_t)(user_strlen(hdr.banner) + 1), &dup_log_h, 1
    };
    user_syscall4(SYS_CHANNEL_WRITE, parent_ch, 0, (uintptr_t)&wargs, 0);

    /* 6. Start 'hello-app' in EL0 via SYS_PROCESS_START, transferring child_ch in x0! */
    ulog(root_log_h, "[userboot] Calling SYS_PROCESS_START to launch 'hello-app' in EL0...");
    ab_process_start_args_t start_args = {
        child_thr_h, hdr.entry_uva, 0, child_ch, 0
    };
    user_syscall4(SYS_PROCESS_START, child_proc_h, (uintptr_t)&start_args, 0, 0);

    /* 7. Wait for 'hello-app' reply on parent_ch and wait for process termination signal! */
    arch_enable_ints();
    user_syscall4(SYS_OBJECT_WAIT_ONE, parent_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
    char reply[128];
    ab_channel_read_args_t rep_rargs = { reply, NULL, sizeof(reply), 0, &act_b, &act_h };
    if (user_syscall4(SYS_CHANNEL_READ, parent_ch, 0, (uintptr_t)&rep_rargs, 0) == AB_OK) {
        ulog(root_log_h, reply);
    }

    user_syscall4(SYS_OBJECT_WAIT_ONE, child_proc_h, AB_SIGNAL_TASK_TERMINATED,
                  AB_TIME_INFINITE, (uintptr_t)&obs);
    ulog(root_log_h, "[userboot] Observed AB_SIGNAL_TASK_TERMINATED on 'hello-app' ProcessDispatcher!");

    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

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
    dprintf(INFO, "Lesson 12: Tasks (Job, Process, Thread) & userboot Bootstrapping\n");
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

    /* 1. Create Root Job, Root Debuglog, and Bootfs VMO */
    ab_object_t *root_job = NULL, *root_log = NULL, *bootfs_vmo = NULL;
    ab_job_dispatcher_create(&root_job);
    ab_log_dispatcher_create(0, &root_log);
    ab_vmo_dispatcher_create(PAGE_SIZE, 0, &bootfs_vmo);

    bootfs_header_t hdr;
    hdr.magic     = BOOTFS_MAGIC;
    const char *aname = "hello-app";
    for (int i = 0; i < 10; i++) hdr.app_name[i] = aname[i];
    hdr.entry_uva = USER_KVA_TO_UVA(hello_app_entry);
    const char *bmsg  = "[hello-app] Payload loaded from Bootfs VMO via userboot!";
    for (size_t i = 0; i <= user_strlen(bmsg); i++) hdr.banner[i] = bmsg[i];
    ab_vmo_write(bootfs_vmo, &hdr, 0, sizeof(hdr));

    /* 2. Create userboot process + thread and bootstrap Channel */
    ab_process_t *userboot_proc = ab_process_create_kernel("userboot", ab_alloc_asid());
    ab_object_t *ub_thr_obj = NULL;
    ab_thread_dispatcher_create(userboot_proc, "userboot-main", &ub_thr_obj);

    ab_object_t *boot_ch0 = NULL, *boot_ch1 = NULL;
    ab_channel_dispatcher_create(&boot_ch0, &boot_ch1);

    /* Allocate temporary handles in userboot_proc or kernel staging to write bootstrap message */
    ab_handle_t job_h = 0, log_h = 0, vmo_h = 0, ch1_h = 0;
    handle_alloc(userboot_proc, root_job,   AB_RIGHTS_DEFAULT_JOB, &job_h);
    handle_alloc(userboot_proc, root_log,   AB_RIGHTS_DEFAULT_LOG, &log_h);
    handle_alloc(userboot_proc, bootfs_vmo, AB_RIGHTS_DEFAULT_VMO, &vmo_h);
    handle_alloc(userboot_proc, boot_ch1,   AB_RIGHTS_DEFAULT_CHANNEL, &ch1_h);

    ab_handle_t boot_handles[3] = { job_h, log_h, vmo_h };
    uint32_t boot_tag = BOOTFS_MAGIC;
    ab_channel_write(userboot_proc, boot_ch0, &boot_tag, sizeof(boot_tag), boot_handles, 3);

    ab_object_release(root_job);
    ab_object_release(root_log);
    ab_object_release(bootfs_vmo);
    ab_object_release(boot_ch0);
    ab_object_release(boot_ch1);

    dprintf(INFO, "[kernel] Starting 'userboot' (koid=%lu) in EL0 with bootstrap channel handle=0x%x\n",
            userboot_proc->base.koid, ch1_h);
    ab_process_start_user(NULL, userboot_proc, (ab_thread_dispatcher_t *)ub_thr_obj,
                          USER_KVA_TO_UVA(userboot_entry), 0, ch1_h, 0);

    /* Wait for userboot to finish */
    arch_enable_ints();
    while (((ab_thread_dispatcher_t *)ub_thr)->kthread->state != THREAD_DEATH) { thread_yield(); __asm__ volatile("wfi"); } //

    dprintf(INFO, "[lk_main] 'userboot' finished with exit code %d! Lesson 12 complete.\n",
            userboot_proc->exit_code);
    thread_become_idle();
}
