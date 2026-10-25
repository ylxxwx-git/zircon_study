/* =========================================================================
 * main.c - ABOS Lesson 11: Shared Memory IPC (VMO, VMAR & Demand Paging)
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
 * EL0 Process A (Producer):
 * arg0 = vmar_h, arg1 = ch0_h. Also has log_h via Channel or initial setup.
 * ========================================================================= */
void process_a_entry(uint64_t arg0, uint64_t arg1) {
    ab_handle_t vmar_h = (ab_handle_t)(arg0 & 0xFFFFFFFFULL);
    ab_handle_t log_h  = (ab_handle_t)(arg0 >> 32);
    ab_handle_t ch0_h  = (ab_handle_t)arg1;

    /* 1. Create a 2-page (8KB) VMO */
    ab_handle_t vmo_h = AB_HANDLE_INVALID;
    user_syscall4(SYS_VMO_CREATE, 8192, 0, (uintptr_t)&vmo_h, 0);

    /* 2. Map the VMO into Process A's VMAR at offset 0 (VA 0x10000000) */
    vaddr_t mapped_va = 0;
    ab_vmar_map_args_t margs;
    margs.vmo_handle    = vmo_h;
    margs.vmo_offset    = 0;
    margs.len           = 8192;
    margs.map_flags     = AB_VM_PERM_READ | AB_VM_PERM_WRITE;
    margs.mapped_va_out = &mapped_va;
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0, (uintptr_t)&margs, 0);

    ulog(log_h, "[Proc A] Mapped VMO at VA 0x10000000; writing directly to trigger demand paging...");

    /* 3. Write directly to the unallocated page at mapped_va -> triggers EL0 Data Abort! */
    const char *secret = "Zero-copy shared VMO payload written by Process A at VA 0x10000000!";
    char *shm_ptr = (char *)mapped_va;
    for (size_t i = 0; i <= user_strlen(secret); i++) {
        shm_ptr[i] = secret[i];
    }

    /* 4. Duplicate vmo_h WITHOUT AB_RIGHT_WRITE (Read-Only + Map + Transfer) */
    ab_handle_t ro_vmo_h = AB_HANDLE_INVALID;
    ab_handle_t dup_log_h = AB_HANDLE_INVALID;
    user_syscall4(SYS_HANDLE_DUPLICATE, vmo_h,
                  AB_RIGHT_READ | AB_RIGHT_MAP | AB_RIGHT_TRANSFER | AB_RIGHT_INSPECT,
                  (uintptr_t)&ro_vmo_h, 0);
    user_syscall4(SYS_HANDLE_DUPLICATE, log_h, AB_RIGHT_SAME_RIGHTS,
                  (uintptr_t)&dup_log_h, 0);

    /* 5. Send ro_vmo_h and dup_log_h over Channel to Process B */
    ab_handle_t send_h[2] = { dup_log_h, ro_vmo_h };
    uint32_t tag = 0x564D4F21U; /* "VMO!" */
    ab_channel_write_args_t wargs = { &tag, sizeof(tag), send_h, 2 };
    user_syscall4(SYS_CHANNEL_WRITE, ch0_h, 0, (uintptr_t)&wargs, 0);

    /* Wait for Process B to close ch1_h when done */
    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, ch0_h, AB_SIGNAL_PEER_CLOSED, AB_TIME_INFINITE, (uintptr_t)&obs);
    ulog(log_h, "[Proc A] Process B finished reading shared VMO! Exiting.");

    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}

/* =========================================================================
 * EL0 Process B (Consumer):
 * arg0 = vmar_h, arg1 = ch1_h.
 * Receives read-only VMO handle from Process A, verifies write-mapping is
 * denied, maps read-only at VA 0x20000000, and reads zero-copy!
 * ========================================================================= */
void process_b_entry(uint64_t arg0, uint64_t arg1) {
    ab_handle_t vmar_h = (ab_handle_t)arg0;
    ab_handle_t ch1_h  = (ab_handle_t)arg1;

    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, ch1_h, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t recv_h[2];
    ab_channel_read_args_t rargs = { &tag, recv_h, sizeof(tag), 2, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, ch1_h, 0, (uintptr_t)&rargs, 0);

    ab_handle_t log_h    = recv_h[0];
    ab_handle_t ro_vmo_h = recv_h[1];

    /* 1. Attempt to map ro_vmo_h with AB_VM_PERM_WRITE -> MUST fail with AB_ERR_ACCESS_DENIED! */
    vaddr_t bad_va = 0;
    ab_vmar_map_args_t bad_map = {
        ro_vmo_h, 0, 8192, AB_VM_PERM_READ | AB_VM_PERM_WRITE, &bad_va
    };
    ab_status_t denied = user_syscall4(SYS_VMAR_MAP, vmar_h, 0x10000000ULL, (uintptr_t)&bad_map, 0);
    if (denied == AB_ERR_ACCESS_DENIED) {
        ulog(log_h, "[Proc B] Verified capability protection: writable VMAR_MAP on RO VMO handle denied!");
    }

    /* 2. Map ro_vmo_h Read-Only at VA 0x20000000 (vmar_offset = 0x10000000) */
    vaddr_t shm_va = 0;
    ab_vmar_map_args_t ok_map = {
        ro_vmo_h, 0, 8192, AB_VM_PERM_READ, &shm_va
    };
    user_syscall4(SYS_VMAR_MAP, vmar_h, 0x10000000ULL, (uintptr_t)&ok_map, 0);

    /* 3. Read the shared physical page directly from VA 0x20000000! */
    const char *shared_str = (const char *)shm_va;
    ulog(log_h, "[Proc B] Reading shared VMO mapped at VA 0x20000000:");
    ulog(log_h, shared_str);

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
    dprintf(INFO, "Lesson 11: Shared Memory IPC (VMO, VMAR & Demand Paging)\n");
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

    ab_process_t *proc_a = ab_process_create_kernel("proc-A-producer", 1);
    ab_process_t *proc_b = ab_process_create_kernel("proc-B-consumer", 2);

    ab_object_t *vmar_a_obj = NULL, *vmar_b_obj = NULL;
    ab_vmar_dispatcher_create(proc_a, &vmar_a_obj);
    ab_vmar_dispatcher_create(proc_b, &vmar_b_obj);
    ab_handle_t vmar_a_h = AB_HANDLE_INVALID, vmar_b_h = AB_HANDLE_INVALID;
    handle_alloc(proc_a, vmar_a_obj, AB_RIGHTS_DEFAULT_VMAR, &vmar_a_h);
    handle_alloc(proc_b, vmar_b_obj, AB_RIGHTS_DEFAULT_VMAR, &vmar_b_h);
    ab_object_release(vmar_a_obj);
    ab_object_release(vmar_b_obj);

    ab_object_t *log_obj = NULL;
    ab_log_dispatcher_create(0, &log_obj);
    ab_handle_t log_a_h = AB_HANDLE_INVALID;
    handle_alloc(proc_a, log_obj, AB_RIGHTS_DEFAULT_LOG, &log_a_h);
    ab_object_release(log_obj);

    ab_object_t *ch0_obj = NULL, *ch1_obj = NULL;
    ab_channel_dispatcher_create(&ch0_obj, &ch1_obj);
    ab_handle_t ch0_a_h = AB_HANDLE_INVALID, ch1_b_h = AB_HANDLE_INVALID;
    handle_alloc(proc_a, ch0_obj, AB_RIGHTS_DEFAULT_CHANNEL, &ch0_a_h);
    handle_alloc(proc_b, ch1_obj, AB_RIGHTS_DEFAULT_CHANNEL, &ch1_b_h);
    ab_object_release(ch0_obj);
    ab_object_release(ch1_obj);

    static user_launch_args_t args_a, args_b;
    args_a.proc     = proc_a;
    args_a.entry_fn = (uintptr_t)process_a_entry;
    args_a.arg0     = ((uint64_t)log_a_h << 32) | (uint64_t)vmar_a_h;
    args_a.arg1     = ch0_a_h;

    args_b.proc     = proc_b;
    args_b.entry_fn = (uintptr_t)process_b_entry;
    args_b.arg0     = vmar_b_h;
    args_b.arg1     = ch1_b_h;

    thread_t *ta = thread_create("proc-a-main", run_user_process_thread, &args_a);
    thread_t *tb = thread_create("proc-b-main", run_user_process_thread, &args_b);
    thread_resume(tb);
    thread_resume(ta);

    while (ta->state != THREAD_DEATH || tb->state != THREAD_DEATH) {
        thread_sleep_ns(10000000ULL);
    }

    dprintf(INFO, "[lk_main] Shared-memory VMO test completed (A=%d, B=%d)! Lesson 11 complete.\n",
            proc_a->exit_code, proc_b->exit_code);
    thread_become_idle();
}
