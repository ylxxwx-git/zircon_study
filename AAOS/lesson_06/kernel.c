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

extern void lock_mutex(volatile int *lock);
extern void unlock_mutex(volatile int *lock);

volatile int gic_ready = 0;
volatile int uart_lock = 0;

extern void _start();

void sleep(volatile int cycles) {
    while(cycles-- > 0);
}

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
    // Spinlocks require MMU to be enabled for exclusive monitors to work reliably on some QEMU versions/configurations.
    // We will enable them in Lesson 04 (MMU). For now, we accept interleaved output.
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
    safe_puts("Test Thread Started\n");
    while (1) {
        // unsafe_puts("Task 1 Alive\n");
        // lock_uart();
        safe_puts("Core "); safe_puthex(core_id); safe_puts(" Thread "); safe_puthex(current->pid); safe_puts(": Running\n");
        // unlock_uart();
        sleep(100000);
        // schedule();
    }
}

void kernel_main(uint64_t dtb_ptr32, uint64_t x1, uint64_t x2, uint64_t x3) {
    int core_id = get_core_id();
    
    if (core_id == 0) {
        // 1. Initialize UART
        init_uart();
        safe_puts("Kernel Started (Multicore)\n");
        safe_puts("DTB Pointer: ");
        safe_puthex(dtb_ptr32);
        
        dtb_init(dtb_ptr32);
        
        // 1.5 Initialize PMM
        extern uint64_t _end;
        extern uint64_t memory_base;
        extern uint64_t memory_size;
        
        if (memory_size > 0) {
            pmm_init((uint64_t)&_end, memory_base, memory_size);
        }
        
        // 1.6 Initialize MMU (Done in boot.S)
        safe_puts("MMU Enabled in boot.S\n");
        safe_puts("Kernel Main Address: "); safe_puthex((uint64_t)&kernel_main); safe_puts("\n");
        
        // 1.7 Initialize Scheduler (Core 0)
        init_scheduler();
        
        // Create Test Thread for Core 0
        int pid = kthread_create(test_thread);
        safe_puts("Core 0 Created Thread PID: "); safe_puthex(pid); safe_puts("\n");
        
        // 2. Initialize GIC & Wake Secondary Cores
        safe_puts("Core 0: Initializing GIC Distributor...\n");
        gic_dist_init();
        
        // Wake up other cores NOW
        init_other_cores();
    } else {
        // Secondary Cores need to initialize their scheduler (current pointer)
        lock_uart();
        puts("Core "); puthex(core_id); puts(" Entered Kernel Main\n");
        unlock_uart();
        
        init_scheduler();
        
        // Create Test Thread for this Core
        int pid = kthread_create(test_thread);
        lock_uart();
        puts("Core "); puthex(core_id); puts(" Created Thread PID: "); puthex(pid); puts("\n");
        unlock_uart();
    }
    
    // 3. Initialize GIC CPU Interface (All Cores)
    gic_cpu_init();

    // 4. Initialize Timer (All Cores)
    timer_init();

    // 5. Enable Interrupts
    asm volatile("msr daifclr, #2");
    
    lock_uart();
    puts("Core ");
    puthex(core_id);
    puts(": Interrupts Enabled!\n");
    unlock_uart();

    // 6. Main Loop
    
    while (1) {
        if (core_id == 0) {
             // safe_puts("Core 0: Idle Loop\n");
        }
        safe_puts("Core "); safe_puthex(core_id); safe_puts(" Task 0: Running\n");
        sleep(100000);
        // schedule();
    }
}
