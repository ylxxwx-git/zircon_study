#include "uart.h"
#include "print.h"
#include "string.h"
#include "gic.h"
#include "timer.h"
#include "dtb.h"
#include "pmm.h"
#include "mmu.h"
#include "sched.h"
#include "process.h"
#include "virtio.h"
#include "tar.h"

extern int get_core_id();


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

extern void _start();
extern void sleep_ms(uint64_t ms);
extern void map_user_page(uint64_t pa);
extern void move_to_user_mode(uint64_t entry, uint64_t sp);
extern void *pmm_alloc_page();

void init_other_cores() {
    // Wake up secondary cores using PSCI
    // We assume they are in a holding pen (QEMU default)
    printk("Core 0: Waking up secondary cores...\n");
    uint64_t entry_point = 0x40000000; // Physical address of _start
    psci_cpu_on(1, entry_point);
    psci_cpu_on(2, entry_point);
    psci_cpu_on(3, entry_point);
}

void test_thread() {
    int core_id = get_core_id();
    printk("Test Thread Started\n");
    while (1) {
        // printk("Core %x Core, Task 1 Running\n", core_id);
        sleep_ms(100);
    }
}

#include "irq.h"



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
        printk("Core 0 Created Thread PID: %x\n", pid);
        
        // 2. Initialize GIC & Wake Secondary Cores
        printk("Core 0: Initializing GIC Distributor...\n");
        gic_dist_init();
        
        // Enable UART RX Interrupt (ID 33)
        gic_enable_interrupt(33);
        
        // Wake up other cores NOW
        init_other_cores();
    } else {
        // Secondary Cores need to initialize their scheduler (current pointer)
        printk("Core %x: Entered Kernel Main\n", core_id);
        
        init_scheduler();
        
        // Create Test Thread for this Core
        int pid = kthread_create(test_thread);
        printk("Core %x Created Thread PID: %x\n", core_id, pid);
    }
    
    // 3. Initialize GIC CPU Interface (All Cores)
    gic_cpu_init();

    // 4. Initialize Timer (All Cores)
    timer_init();

    // 5. Enable Interrupts
    asm volatile("msr daifclr, #2");
    
    printk("Core %x: Interrupts Enabled!\n", core_id);

    // 6. User Mode Test
    // Remap UART to Kernel VA (Higher Half)
    // This allows us to remove the Identity Map for Device Memory in User Page Tables
    if (core_id == 0) {
        uart_remap();
        gic_remap();
        printk("UART & GIC Remapped to Kernel VA\n");
    }
    
    if (core_id == 0) {
        printk("Core 0: Initializing VirtIO...\n");
        virtio_init();
        printk("Core 0: Initializing TAR FS...\n");
        tar_init();
        
        uint64_t current_el;
    asm volatile("mrs %0, CurrentEL" : "=r"(current_el));
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    printk("CurrentEL: %x, SCTLR_EL1: %x\n", current_el >> 2, sctlr);
    
    create_user_process("./shell.bin");
    }

    // 6. Task 0
    while (1) {
        // printk("Core %x Core, Task 0 Running\n", core_id);
        sleep_ms(100);
    }
}
