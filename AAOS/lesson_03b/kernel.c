#include "output.h"
#include "gic.h"
#include "timer.h"

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

void sleep(int cycles) {
    while(cycles-- > 0);
}

void init_other_cores() {
    // Wake up secondary cores using PSCI
    // We assume they are in a holding pen (QEMU default)
    safe_puts("Core 0: Waking up secondary cores...\n");
    psci_cpu_on(1, (unsigned long)_start);
    psci_cpu_on(2, (unsigned long)_start);
    psci_cpu_on(3, (unsigned long)_start);
}

void lock_uart() {
    // Spinlocks require MMU to be enabled for exclusive monitors to work reliably on some QEMU versions/configurations.
    // We will enable them in Lesson 04 (MMU). For now, we accept interleaved output.
    // lock_mutex(&uart_lock);
}

void unlock_uart() {
    // unlock_mutex(&uart_lock);
}

void safe_puts(const char *s) {
    // lock_uart();
    puts(s);
    // unlock_uart();
}

void safe_puthex(uint64_t v) {
    // lock_uart();
    puthex(v);
    // unlock_uart();
}

void kernel_main(void) {
    int core_id = get_core_id();
    
    if (core_id == 0) {
        // 1. Initialize UART (Core 0 only, or safe if multiple do it idempotent)
        init_uart();
        safe_puts("Kernel Started (Multicore)\n");

        // 2. Initialize GIC & Wake Secondary Cores
        safe_puts("Core 0: Initializing GIC Distributor...\n");
        gic_dist_init();
        
        // Wake up other cores NOW, after GICD is ready.
        init_other_cores();
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
        asm volatile("wfi");
    }
}
