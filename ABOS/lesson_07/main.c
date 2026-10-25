/* =========================================================================
 * main.c - ABOS Lesson 07: Kernel Objects, KOIDs, Handles, Rights & EL0
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

/*
 * EL0 User Program Entry Point:
 * Receives an initial bootstrap LogDispatcher handle in x0 (arg0)!
 * Note: Because user_program_entry executes at low user VA (0x00084xxx),
 * AArch64 PC-relative addressing (adrp) automatically resolves string literals
 * in .rodata to their low user VA (0x0008bxxx).
 */
void user_program_entry(uint64_t arg0) {
    ab_handle_t log_h = (ab_handle_t)arg0;

    /* 1. Write to the kernel debuglog using the capability handle passed in x0 */
    const char *msg1 = "Hello from EL0 using bootstrap LogDispatcher handle!";
    user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0, (uintptr_t)msg1, user_strlen(msg1));

    /* 2. Inspect handle info (KOID, type, rights, handle_count) */
    ab_handle_basic_info_t info;
    user_syscall4(SYS_OBJECT_GET_INFO, log_h, (uintptr_t)&info, 0, 0);

    /* 3. Duplicate log_h WITHOUT AB_RIGHT_WRITE (only INSPECT | TRANSFER) */
    ab_handle_t ro_log_h = AB_HANDLE_INVALID;
    user_syscall4(SYS_HANDLE_DUPLICATE, log_h, AB_RIGHT_INSPECT | AB_RIGHT_TRANSFER,
                  (uintptr_t)&ro_log_h, 0);

    /* 4. Attempt to write using ro_log_h -> MUST fail with AB_ERR_ACCESS_DENIED (-30)! */
    const char *msg2 = "This write should be denied!";
    ab_status_t denied_st = user_syscall4(SYS_DEBUGLOG_WRITE, ro_log_h, 0,
                                          (uintptr_t)msg2, user_strlen(msg2));
    if (denied_st == AB_ERR_ACCESS_DENIED) {
        const char *ok_msg =
            "Verified capability check: write via reduced-right handle returned AB_ERR_ACCESS_DENIED!";
        user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0, (uintptr_t)ok_msg, user_strlen(ok_msg));
    }

    /* 5. Close both handles -> triggers LogDispatcher on_zero_handles + destruction! */
    user_syscall4(SYS_HANDLE_CLOSE, ro_log_h, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);

    /* 6. Attempt to use log_h after closing -> MUST fail with AB_ERR_BAD_HANDLE (-11)! */
    ab_status_t closed_st = user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0,
                                          (uintptr_t)msg1, user_strlen(msg1));

    /* Exit process with status 0 if closed_st == AB_ERR_BAD_HANDLE */
    user_syscall4(SYS_PROCESS_EXIT, (closed_st == AB_ERR_BAD_HANDLE) ? 0 : 1, 0, 0, 0);
    for (;;) { }
}

static int run_user_process_thread(void *arg) {
    ab_process_t *proc = (ab_process_t *)arg;
    thread_t *cur = get_current_thread();
    cur->process_obj = proc;
    cur->aspace      = proc->aspace;
    vmm_switch_aspace(proc->aspace);

    /* Map .text .. .rodata at USER_KVA_TO_UVA so PC-relative calls and literals work in EL0 */
    for (uintptr_t kva = (uintptr_t)_text_start; kva < (uintptr_t)_rodata_end; kva += PAGE_SIZE) {
        paddr_t pa = physmap_to_paddr((const void *)kva);
        vaddr_t uva = USER_KVA_TO_UVA(kva);
        vmm_map_page(proc->aspace, uva, pa,
                     MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_EXECUTE);
    }

    /* Allocate and map EL0 user stack */
    paddr_t stack_pa = 0;
    pmm_alloc_page(VM_PAGE_STATE_OBJECT, &stack_pa);
    vaddr_t user_stack_va = 0x00800000ULL;
    vmm_map_page(proc->aspace, user_stack_va, stack_pa,
                 MMU_FLAG_PERM_USER | MMU_FLAG_PERM_READ | MMU_FLAG_PERM_WRITE);

    /* Create a bootstrap LogDispatcher object and allocate Handle #0 in proc's handle table */
    ab_object_t *log_obj = NULL;
    ab_log_dispatcher_create(0, &log_obj);
    ab_handle_t boot_log_h = AB_HANDLE_INVALID;
    handle_alloc(proc, log_obj, AB_RIGHTS_DEFAULT_LOG, &boot_log_h);
    ab_object_release(log_obj);

    dprintf(INFO, "[kernel] Launching EL0 process '%s' (koid=%lu) with bootstrap handle=0x%x (koid=%lu)\n",
            proc->name, proc->base.koid, boot_log_h, log_obj->koid);

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
    dprintf(INFO, "Lesson 07: Kernel Objects (Dispatcher), Handles, Rights & EL0\n");
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

    ab_process_t *proc = ab_process_create_kernel("test-cap-proc", 1);
    thread_t *ut = thread_create("user-main", run_user_process_thread, proc);
    thread_resume(ut);

    while (ut->state != THREAD_DEATH) {
        thread_sleep_ns(10000000ULL);
    }

    dprintf(INFO, "[lk_main] Process '%s' exited with code %d! Lesson 07 complete.\n",
            proc->name, proc->exit_code);
    thread_become_idle();
}
