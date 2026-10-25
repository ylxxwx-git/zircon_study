/* =========================================================================
 * main.c - ABOS Lesson 10: Message-Passing IPC (Channel & Handle Transfer)
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
extern void arm64_uspace_entry(uintptr_t arg1, uintptr_t arg2, uintptr_t pc,
                               uintptr_t user_sp, vaddr_t kstack_top);

#define USER_KVA_TO_UVA(kva) ((vaddr_t)((uintptr_t)(kva) - 0xFFFFFFFF80000000ULL))

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
 * EL0 Process A (Sender / Client):
 * Starts with (arg0 = log_h, arg1 = ch0_h).
 * Creates an Event, duplicates log_h and ev_h, and transfers BOTH handles
 * over ch0_h to Process B!
 * ========================================================================= */
void process_a_entry(uint64_t arg0, uint64_t arg1) {
    ab_handle_t log_h = (ab_handle_t)arg0;
    ab_handle_t ch0_h = (ab_handle_t)arg1;

    ulog(log_h, "[Proc A] Creating Event and duplicating Log + Event capabilities for Process B");

    ab_handle_t ev_h = AB_HANDLE_INVALID;
    user_syscall4(SYS_EVENT_CREATE, 0, (uintptr_t)&ev_h, 0, 0);

    ab_handle_t dup_log_h = AB_HANDLE_INVALID;
    ab_handle_t dup_ev_h  = AB_HANDLE_INVALID;
    user_syscall4(SYS_HANDLE_DUPLICATE, log_h, AB_RIGHT_SAME_RIGHTS, (uintptr_t)&dup_log_h, 0);
    user_syscall4(SYS_HANDLE_DUPLICATE, ev_h,  AB_RIGHT_SAME_RIGHTS, (uintptr_t)&dup_ev_h, 0);

    const char *ping_msg = "PING from Process A (carrying Log + Event handles)";
    ab_handle_t send_handles[2];
    send_handles[0] = dup_log_h;
    send_handles[1] = dup_ev_h;

    ab_channel_write_args_t wargs;
    wargs.bytes       = ping_msg;
    wargs.num_bytes   = (uint32_t)(user_strlen(ping_msg) + 1);
    wargs.handles     = send_handles;
    wargs.num_handles = 2;

    user_syscall4(SYS_CHANNEL_WRITE, ch0_h, 0, (uintptr_t)&wargs, 0);
    ulog(log_h, "[Proc A] Sent message + 2 capabilities over Channel; waiting on Event + Reply...");

    /* Wait for Process B to signal the transferred Event capability! */
    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, ev_h, AB_SIGNAL_SIGNALED, AB_TIME_INFINITE, (uintptr_t)&obs);
    ulog(log_h, "[Proc A] Observed AB_SIGNAL_SIGNALED on Event triggered by Process B!");

    /* Read Process B's reply message from ch0_h */
    user_syscall4(SYS_OBJECT_WAIT_ONE, ch0_h, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
    char reply_buf[128];
    uint32_t actual_b = 0, actual_h = 0;
    ab_channel_read_args_t rargs;
    rargs.bytes          = reply_buf;
    rargs.handles        = NULL;
    rargs.max_bytes      = sizeof(reply_buf);
    rargs.max_handles    = 0;
    rargs.actual_bytes   = &actual_b;
    rargs.actual_handles = &actual_h;

    if (user_syscall4(SYS_CHANNEL_READ, ch0_h, 0, (uintptr_t)&rargs, 0) == AB_OK) {
        ulog(log_h, reply_buf);
    }

    user_syscall4(SYS_HANDLE_CLOSE, ev_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, ch0_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * EL0 Process B (Receiver / Server):
 * Starts ONLY with (arg0 = ch1_h) -- NO initial LogDispatcher capability!
 * Receives LogDispatcher and EventDispatcher handles over ch1_h from Process A!
 * ========================================================================= */
void process_b_entry(uint64_t arg0) {
    ab_handle_t ch1_h = (ab_handle_t)arg0;

    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, ch1_h, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);

    char msg_buf[128];
    ab_handle_t recv_handles[2];
    uint32_t actual_b = 0, actual_h = 0;

    ab_channel_read_args_t rargs;
    rargs.bytes          = msg_buf;
    rargs.handles        = recv_handles;
    rargs.max_bytes      = sizeof(msg_buf);
    rargs.max_handles    = 2;
    rargs.actual_bytes   = &actual_b;
    rargs.actual_handles = &actual_h;

    ab_status_t st = user_syscall4(SYS_CHANNEL_READ, ch1_h, 0, (uintptr_t)&rargs, 0);
    if (st != AB_OK || actual_h != 2) {
        user_syscall4(SYS_PROCESS_EXIT, 1, 0, 0, 0);
    }

    ab_handle_t received_log_h = recv_handles[0];
    ab_handle_t received_ev_h  = recv_handles[1];

    /* Use the newly received LogDispatcher capability in Process B! */
    ulog(received_log_h, "[Proc B] Received Log + Event capabilities over Channel from Process A!");
    ulog(received_log_h, msg_buf);

    /* Send reply message over ch1_h and signal the received Event capability */
    const char *pong_msg = "[Proc A] Dequeued reply from Channel: PONG from Process B!";
    ab_channel_write_args_t wargs;
    wargs.bytes       = pong_msg;
    wargs.num_bytes   = (uint32_t)(user_strlen(pong_msg) + 1);
    wargs.handles     = NULL;
    wargs.num_handles = 0;
    user_syscall4(SYS_CHANNEL_WRITE, ch1_h, 0, (uintptr_t)&wargs, 0);

    user_syscall4(SYS_OBJECT_SIGNAL, received_ev_h, 0, AB_SIGNAL_SIGNALED, 0);

    user_syscall4(SYS_HANDLE_CLOSE, received_ev_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, received_log_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, ch1_h, 0, 0, 0);
    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

typedef struct {
    ab_process_t *proc;
    uintptr_t     entry_fn;
    uint64_t      arg0;
    uint64_t      arg1;
} user_launch_args_t;

static int run_user_process_thread(void *arg) {
    user_launch_args_t *largs = (user_launch_args_t *)arg;
    ab_process_t *proc = largs->proc;
    thread_t *cur = get_current_thread();
    cur->process_obj = proc;
    cur->aspace      = proc->aspace;
    vmm_switch_aspace(proc->aspace);

    for (uintptr_t kva = (uintptr_t)_text_start; kva < (uintptr_t)_rodata_end; kva += PAGE_SIZE) {
        paddr_t pa = physmap_to_paddr((const void *)kva);
        vaddr_t uva = USER_KVA_TO_UVA(kva);
        vmm_map_page(proc->aspace, uva, pa,
                     MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_EXECUTE);
    }

    paddr_t stack_pa = 0;
    pmm_alloc_page(VM_PAGE_STATE_OBJECT, &stack_pa);
    vaddr_t user_stack_va = 0x00800000ULL;
    vmm_map_page(proc->aspace, user_stack_va, stack_pa,
                 MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_WRITE);

    vaddr_t kstack_top = (cur->stack_base + cur->stack_size) & ~15ULL;
    arm64_uspace_entry(largs->arg0, largs->arg1,
                       USER_KVA_TO_UVA(largs->entry_fn),
                       user_stack_va + PAGE_SIZE, kstack_top);
    return 0;
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
    dprintf(INFO, "Lesson 10: Message-Passing IPC (Channel & Capability Transfer)\n");
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

    /* Create two isolated EL0 processes (ASID 1 and ASID 2) */
    ab_process_t *proc_a = ab_process_create_kernel("proc-A-client", 1);
    ab_process_t *proc_b = ab_process_create_kernel("proc-B-server", 2);

    /* Create bootstrap LogDispatcher (given ONLY to Process A!) and Channel pair */
    ab_object_t *log_obj = NULL;
    ab_log_dispatcher_create(0, &log_obj);
    ab_handle_t log_h_a = AB_HANDLE_INVALID;
    handle_alloc(proc_a, log_obj, AB_RIGHTS_DEFAULT_LOG, &log_h_a);
    ab_object_release(log_obj);

    ab_object_t *ch0_obj = NULL, *ch1_obj = NULL;
    ab_channel_dispatcher_create(&ch0_obj, &ch1_obj);
    ab_handle_t ch0_h_a = AB_HANDLE_INVALID;
    ab_handle_t ch1_h_b = AB_HANDLE_INVALID;
    handle_alloc(proc_a, ch0_obj, AB_RIGHTS_DEFAULT_CHANNEL, &ch0_h_a);
    handle_alloc(proc_b, ch1_obj, AB_RIGHTS_DEFAULT_CHANNEL, &ch1_h_b);
    ab_object_release(ch0_obj);
    ab_object_release(ch1_obj);

    static user_launch_args_t args_a, args_b;
    args_a.proc     = proc_a;
    args_a.entry_fn = (uintptr_t)process_a_entry;
    args_a.arg0     = log_h_a;
    args_a.arg1     = ch0_h_a;

    args_b.proc     = proc_b;
    args_b.entry_fn = (uintptr_t)process_b_entry;
    args_b.arg0     = ch1_h_b;
    args_b.arg1     = 0;

    thread_t *ta = thread_create("proc-a-main", run_user_process_thread, &args_a);
    thread_t *tb = thread_create("proc-b-main", run_user_process_thread, &args_b);
    thread_resume(tb);
    thread_resume(ta);

    while (ta->state != THREAD_DEATH || tb->state != THREAD_DEATH) {
        thread_sleep_ns(10000000ULL);
    }

    dprintf(INFO, "[lk_main] Both isolated EL0 processes exited (A=%d, B=%d)! Lesson 10 complete.\n",
            proc_a->exit_code, proc_b->exit_code);
    thread_become_idle();
}
