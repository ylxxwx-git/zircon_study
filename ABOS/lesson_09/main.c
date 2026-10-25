/* =========================================================================
 * main.c - ABOS Lesson 09: Asynchronous Event Notification with Port Objects
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

void user_program_entry(uint64_t arg0) {
    ab_handle_t log_h = (ab_handle_t)arg0;
    ulog(log_h, "EL0 started: testing PortDispatcher, WaitAsync, and User Port Packets");

    /* 1. Create a Port and two Event objects */
    ab_handle_t port_h = AB_HANDLE_INVALID;
    ab_handle_t ev1_h  = AB_HANDLE_INVALID;
    ab_handle_t ev2_h  = AB_HANDLE_INVALID;
    user_syscall4(SYS_PORT_CREATE, 0, (uintptr_t)&port_h, 0, 0);
    user_syscall4(SYS_EVENT_CREATE, 0, (uintptr_t)&ev1_h, 0, 0);
    user_syscall4(SYS_EVENT_CREATE, 0, (uintptr_t)&ev2_h, 0, 0);

    /* 2. Subscribe both Events to the Port using SYS_OBJECT_WAIT_ASYNC with distinct keys */
    user_syscall4(SYS_OBJECT_WAIT_ASYNC, ev1_h, port_h, 0x1001ULL, AB_SIGNAL_SIGNALED);
    user_syscall4(SYS_OBJECT_WAIT_ASYNC, ev2_h, port_h, 0x2002ULL, AB_USER_SIGNAL_0);

    /* 3. Queue an explicit user packet (key=0x3003) and signal both events */
    ab_port_packet_t user_pkt;
    user_pkt.key       = 0x3003ULL;
    user_pkt.type      = AB_PKT_TYPE_USER;
    user_pkt.status    = AB_OK;
    user_pkt.trigger   = 0;
    user_pkt.observed  = 0;
    user_pkt.timestamp = 0;
    user_pkt.user_val  = 0xCAFEBABEULL;
    user_syscall4(SYS_PORT_QUEUE, port_h, (uintptr_t)&user_pkt, 0, 0);

    user_syscall4(SYS_OBJECT_SIGNAL, ev2_h, 0, AB_USER_SIGNAL_0, 0);
    user_syscall4(SYS_OBJECT_SIGNAL, ev1_h, 0, AB_SIGNAL_SIGNALED, 0);

    /* 4. Multiplex all 3 events in a single PortWait event loop! */
    int received = 0;
    for (int i = 0; i < 3; i++) {
        ab_port_packet_t pkt;
        ab_status_t st = user_syscall4(SYS_PORT_WAIT, port_h, AB_TIME_INFINITE,
                                       (uintptr_t)&pkt, 0);
        if (st == AB_OK) {
            if (pkt.key == 0x3003ULL && pkt.type == AB_PKT_TYPE_USER && pkt.user_val == 0xCAFEBABEULL) {
                ulog(log_h, "[PortWait 1/3] Dequeued AB_PKT_TYPE_USER packet (key=0x3003, val=0xCAFEBABE)");
                received++;
            } else if (pkt.key == 0x2002ULL && pkt.type == AB_PKT_TYPE_SIGNAL_ONE &&
                       (pkt.observed & AB_USER_SIGNAL_0)) {
                ulog(log_h, "[PortWait 2/3] Dequeued async signal packet from ev2 (key=0x2002, AB_USER_SIGNAL_0)");
                received++;
            } else if (pkt.key == 0x1001ULL && pkt.type == AB_PKT_TYPE_SIGNAL_ONE &&
                       (pkt.observed & AB_SIGNAL_SIGNALED)) {
                ulog(log_h, "[PortWait 3/3] Dequeued async signal packet from ev1 (key=0x1001, AB_SIGNAL_SIGNALED)");
                received++;
            }
        }
    }

    user_syscall4(SYS_HANDLE_CLOSE, ev1_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, ev2_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, port_h, 0, 0, 0);
    user_syscall4(SYS_PROCESS_EXIT, (received == 3) ? 0 : 1, 0, 0, 0);
    for (;;) { }
}

static int run_user_process_thread(void *arg) {
    ab_process_t *proc = (ab_process_t *)arg;
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

    ab_object_t *log_obj = NULL;
    ab_log_dispatcher_create(0, &log_obj);
    ab_handle_t boot_log_h = AB_HANDLE_INVALID;
    handle_alloc(proc, log_obj, AB_RIGHTS_DEFAULT_LOG, &boot_log_h);
    ab_object_release(log_obj);

    vaddr_t kstack_top = (cur->stack_base + cur->stack_size) & ~15ULL;
    arm64_uspace_entry(boot_log_h, 0, USER_KVA_TO_UVA(user_program_entry),
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
    dprintf(INFO, "Lesson 09: Asynchronous Event Notification with Port Objects\n");
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

    ab_process_t *proc = ab_process_create_kernel("port-async-proc", 1);
    thread_t *ut = thread_create("user-main", run_user_process_thread, proc);
    thread_resume(ut);

    while (ut->state != THREAD_DEATH) {
        thread_sleep_ns(10000000ULL);
    }

    dprintf(INFO, "[lk_main] Process '%s' exited with code %d! Lesson 09 complete.\n",
            proc->name, proc->exit_code);
    thread_become_idle();
}
