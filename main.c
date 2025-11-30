// =================================================================
// main.c - C entry point for the bare-metal AArch64 kernel.
// Prints "Hello World" by writing directly to the QEMU virt UART.
// =================================================================

#include "output.h"

static inline void psci_cpu_on(unsigned long core_id, unsigned long entry_point) {
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
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xFF; // Extract Affinity Level 0 (core ID)
}
extern void _start();
extern void secondary_core_entry();

void sleep(int cycles) {
    while(cycles-- > 0) {
        // Simple delay loop
    }
}

/**
 * @brief The main entry function for the C kernel.
 * @param None
 */
void c_entry() {
    // The core functionality: print the message
   
    int core_id = get_core_id();
    
    if (core_id != 0) {
        puts("Core ");
        puthex(core_id);
        puts(" is not the master core.\n");
        secondary_core_entry();
        return;
    }

    puts("Hello World from a 64-bit ARM (AArch64) bare-metal OS!\n");
    
    // Wake up secondary cores using PSCI
    puts("Waking up secondary cores...\n");
    psci_cpu_on(1, (unsigned long)_start);
    sleep(1000000);
    psci_cpu_on(2, (unsigned long)_start);
    sleep(1000000);
    psci_cpu_on(3, (unsigned long)_start);
    puts("Secondary cores woken!\n");

    // Loop indefinitely to halt execution
    sleep(1000000);
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