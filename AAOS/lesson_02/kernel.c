#include "output.h"


int g_boot_el = 0;

static inline void psci_hvc(unsigned long core_id, unsigned long entry_point) {
    register unsigned long r0 asm("x0") = 0xC4000003;
    register unsigned long r1 asm("x1") = core_id;
    register unsigned long r2 asm("x2") = entry_point;
    register unsigned long r3 asm("x3") = 0;
    
    asm volatile("hvc #0"
                 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                 :
                 : "memory");
}

static inline void psci_smc(unsigned long core_id, unsigned long entry_point) {
    register unsigned long r0 asm("x0") = 0xC4000003;
    register unsigned long r1 asm("x1") = core_id;
    register unsigned long r2 asm("x2") = entry_point;
    register unsigned long r3 asm("x3") = 0;
    
    asm volatile("smc #0"
                 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                 :
                 : "memory");
}

void psci_cpu_on(unsigned long core_id, unsigned long entry_point) {
    if (g_boot_el == 2) {
        psci_smc(core_id, entry_point);
    } else {
        psci_hvc(core_id, entry_point);
    }
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

void kernel_main(int boot_el) {
    // Initialize UART (already done if we use the same output.c, but good to be sure)
    // In lesson_01 output.c, puts calls early_putc which writes to 0x09000000 directly.
    
    g_boot_el = boot_el;

    puts("\n[Lesson 02] Booting...\n");
    puts("Booted from EL");
    puthex(boot_el);
    puts("\n");
    int core_id = get_core_id();
    
    if (core_id != 0) {
        puts("Core ");
        puthex(core_id);
        puts(" is not the master core.\n");
        secondary_core_entry();
        return;
    }

    
    int el = get_el();
    puts("Current Exception Level: ");
    puthex(el);
    puts("\n");
    
    if (el == 1) {
        puts("Success: We are in EL1 (Kernel Mode)!\n");
    } else {
        puts("Warning: We are NOT in EL1.\n");
    }
     
    // Loop indefinitely to halt execution
    sleep(1000000);
    char input[1024];
    while (1) {
        // Infinite loop for the master core
        puts("$: ");
        gets(input);
        puts(input);
        puts("\n");

        if (strncmp(input, "init", 4) == 0) {
            init_other_cores();
        }
        memset(input, 0, 1024);
    }
}
