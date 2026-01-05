#include "output.h"
#include "gic.h"
#include "timer.h"
#include "dtb.h"
#include "pmm.h"
#include "mmu.h"
#include "sched.h"

extern int get_core_id();
void safe_puts(const char *s);
void safe_puthex(uint64_t v);


// Always use HVC. If we are in EL2, our shim handles it.
void psci_cpu_on(unsigned long core_id, unsigned long entry_point) {
    register unsigned long r0 asm("x0") = 0xC4000003;
    register unsigned long r1 asm("x1") = core_id;
    register unsigned long r2 asm("x2") = entry_point;
    register unsigned long r3 asm("x3") = 0;
    
    asm volatile("hvc #0"
                 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                 :
                 : "memory");
}

extern int try_lock_mutex(volatile int *lock);
extern void unlock_mutex(volatile int *lock);

void lock_mutex(volatile int *lock) {
    while (try_lock_mutex(lock) == 0) {
        if (current) {
            schedule();
        } else {
            // Early boot, just spin-wait
            asm volatile("wfe");
        }
    }
}

volatile int gic_ready = 0;
volatile int uart_lock = 0;

extern void _start();
extern void sleep_ms(uint64_t ms);
extern void map_user_page(uint64_t pa);
extern void move_to_user_mode(uint64_t entry, uint64_t sp);
extern void *pmm_alloc_page();

void init_other_cores() {
    // Wake up secondary cores using PSCI
    // We assume they are in a holding pen (QEMU default)
    safe_puts("Core 0: Waking up secondary cores...\n");
    uint64_t entry_point = 0x40000000; // Physical address of _start
    psci_cpu_on(1, entry_point);
    psci_cpu_on(2, entry_point);
    psci_cpu_on(3, entry_point);
}

void lock_uart() {
    lock_mutex(&uart_lock);
}

void unlock_uart() {
    unlock_mutex(&uart_lock);
}

void safe_puts(const char *s) {
    lock_uart();
    puts(s);
    unlock_uart();
}

void safe_puthex(uint64_t v) {
    lock_uart();
    puthex(v);
    unlock_uart();
}

void unsafe_puts(const char *s) {
    puts(s);
}

void unsafe_puthex(uint64_t v) {
    puthex(v);
}

void test_thread() {
    int core_id = get_core_id();
    printf("Test Thread Started\n");
    while (1) {
        printf("Core %x Core, Task 1 Running\n", core_id);
        sleep_ms(100);
    }
}

// Stack frame structure matches stp order in boot.S
struct pt_regs {
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x30;
    uint64_t spsr, elr;
};

void svc_handler_c(struct pt_regs *regs) {
    uint64_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    
    // Check if it's an SVC instruction (EC = 0x15)
    if ((esr >> 26) == 0x15) {
        // System Call
        uint64_t syscall_num = regs->x8;
        safe_puts("SVC Handler Called!\n");
        printf("Syscall Num: %d, Arg0: %d\n", syscall_num, regs->x0);
        
        // Handle specific syscalls
        if (syscall_num == 64) { // sys_write
             // x0 = fd, x1 = buf, x2 = len
             // We ignore fd for now and just print to UART
             // x1 is Virtual Address in User Space (0x0 based).
             // Since we mapped 0x0 -> User Page PA, and we are in Kernel (Higher Half),
             // we can't directly access User VA 0x0 if TTBR0 is not active or if we use TTBR1.
             // BUT we are using TTBR0 for Identity Map (0x0-0x3FFFFFFF).
             // And we mapped User Page at 0x0.
             // So Kernel CAN access 0x0 directly!
             // Because Kernel runs in EL1, and 0x0 is mapped as User RW (AP=1).
             // EL1 can access User RW pages (unless PAN is set, which it isn't).
             
             char *str = (char *)regs->x1;
             int len = regs->x2;
             for (int i = 0; i < len; i++) {
                 // putc(str[i]); // We need safe_putc or just use printf for now
             }
             // safe_puts(str); // Unsafe if x1 is garbage
             // safe_puts("\n");
             printf("sys_write called with len=%d\n", regs->x2);
        }
        
        // Advance ELR to skip the SVC instruction
        regs->elr += 4;
        
        uint64_t daif;
        asm volatile("mrs %0, daif" : "=r"(daif));
        printf("SVC Return: ELR=%x, DAIF=%x\n", regs->elr, daif);
    } else {
        printf("Unexpected Sync Exception: ESR=%x\n", esr);
        while(1);
    }
}

void kernel_main(uint64_t dtb_ptr32, uint64_t x1, uint64_t x2, uint64_t x3) {
    int core_id = get_core_id();
    
    if (core_id == 0) {
        // 1. Initialize UART
        init_uart();
        
        dtb_init(dtb_ptr32);
        
        // 1.5 Initialize PMM
        extern uint64_t _end;
        extern uint64_t memory_base;
        extern uint64_t memory_size;
        
        if (memory_size > 0) {
            pmm_init((uint64_t)&_end, memory_base, memory_size);
        }
        
        // 1.7 Initialize Scheduler (Core 0)
        init_scheduler();
        
        // Create Test Thread for Core 0
        int pid = kthread_create(test_thread);
        printf("Core 0 Created Thread PID: %x\n", pid);
        
        // 2. Initialize GIC & Wake Secondary Cores
        printf("Core 0: Initializing GIC Distributor...\n");
        gic_dist_init();
        
        // Wake up other cores NOW
        init_other_cores();
    } else {
        // Secondary Cores need to initialize their scheduler (current pointer)
        printf("Core %x: Entered Kernel Main\n", core_id);
        
        init_scheduler();
        
        // Create Test Thread for this Core
        int pid = kthread_create(test_thread);
        printf("Core %x Created Thread PID: %x\n", core_id, pid);
    }
    
    // 3. Initialize GIC CPU Interface (All Cores)
    gic_cpu_init();

    // 4. Initialize Timer (All Cores)
    timer_init();

    // 5. Enable Interrupts
    asm volatile("msr daifclr, #2");
    
    printf("Core %x: Interrupts Enabled!\n", core_id);

    // 6. User Mode Test
    if (core_id == 0) {
        printf("Setting up User Mode...\n");
        
        // Allocate page for user code
        void *user_page = pmm_alloc_page();
        if (!user_page) {
            printf("Failed to allocate user page\n");
            while(1);
        }
        
        // Write a small assembly program
        // mov x0, #42      -> 0xD2800540
        // mov x8, #64      -> 0xD2800808 (sys_write)
        // svc #0           -> 0xD4000001
        // b .              -> 0x14000000
        
        uint32_t *code = (uint32_t *)user_page;
        code[0] = 0xD2800540; // mov x0, #42
        code[1] = 0xD2800808; // mov x8, #64
        code[2] = 0xD4000001; // svc #0
        code[3] = 0x14000000; // b .
        
        // Map it to 0x0
        // Convert VA to PA
        uint64_t va_offset = 0xFFFFFFFF40000000;
        uint64_t user_page_pa = (uint64_t)user_page - va_offset;
        map_user_page(user_page_pa);
        
        printf("Jumping to User Mode at 0x0...\n");
        move_to_user_mode(0x0, 0x1000); 
    }

    // 6. Task 0
    while (1) {
        printf("Core %x Core, Task 0 Running\n", core_id);
        sleep_ms(100);
    }
}
