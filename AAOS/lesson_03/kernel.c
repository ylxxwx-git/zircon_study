#include "output.h"


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

int get_core_id() {
    unsigned long mpidr;
    asm volatile("mrs %[mpidr], mpidr_el1" : [mpidr] "=r"(mpidr));
    return mpidr & 0xFF; // Extract Affinity Level 0 (core ID)
}
extern void _start();
extern void secondary_core_entry();

void sleep(int cycles) {
    while(cycles-- > 0) {
        // Simple delay loop
    }
}

// Inline assembly to get the current Exception Level
int get_el() {
    unsigned long el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3;
}

void init_other_cores() {
    // Wake up secondary cores using PSCI
    puts("Waking up secondary cores...\n");
    psci_cpu_on(1, (unsigned long)_start);
    sleep(1000000);
    psci_cpu_on(2, (unsigned long)_start);
    sleep(1000000);
    psci_cpu_on(3, (unsigned long)_start);
    puts("Secondary cores woken!\n");    
}

void secondary_core_entry() {
    puts("Secondary core starting...\n");
    int core_id = get_core_id();
    int loop = 1000;
    
    puts("Core ");
    puthex(core_id);
    puts(" is running!\n");
    
    // Infinite loop for secondary cores
    while (loop-- > 0) {
        // Secondary cores just wait here
    }
    return;
}

#include "gic.h"
#include "output.h"
#include "timer.h"

// ... keep existing functions ...

void kernel_main(void) {
    // 1. Initialize UART
    init_uart();
    puts("Kernel Started (EL1)\n");

    // 2. Initialize GIC
    puts("Initializing GIC...\n");
    gic_init();

    // 3. Initialize Timer
    puts("Initializing Timer...\n");
    timer_init();

    // 4. Enable Interrupts (DAIF)
    // Clear I bit (bit 7) to enable IRQ
    asm volatile("msr daifclr, #2");
    puts("Interrupts Enabled!\n");

    // 4. Main Loop
    while (1) {
        // Wait for interrupt
        asm volatile("wfi");
    }
}
