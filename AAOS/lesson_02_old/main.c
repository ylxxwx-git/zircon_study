// =================================================================
// main.c - C entry point for the bare-metal AArch64 kernel.
// Prints "Hello World" by writing directly to the QEMU virt UART.
// =================================================================

#include "output.h"
#include "utils.h"


extern void _start();
extern void secondary_core_entry();
void user_main(int core_id);

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
    puts("Current EL: ");
    puthex(get_el());
    puts("\n");
    
    extern void vectors();
    puts("Vectors at: ");
    puthex((unsigned long)vectors);
    puts("\n");
    
    extern unsigned long _stack_top;
    puts("Stack Top: ");
    puthex((unsigned long)&_stack_top);
    puts("\n");
    
    extern unsigned long _bss_start, _bss_end;
    puts("BSS Start: "); puthex((unsigned long)&_bss_start); puts("\n");
    puts("BSS End: "); puthex((unsigned long)&_bss_end); puts("\n");

    // Wake up secondary cores using PSCI
    //puts("Waking up secondary cores...\n");
    //psci_cpu_on(1, (unsigned long)_start);
    //sleep(1000000);
    //psci_cpu_on(2, (unsigned long)_start);
    //sleep(1000000);
    //psci_cpu_on(3, (unsigned long)_start);
    //puts("Secondary cores woken!\n");

    // Debug: Check VBAR_EL1 and CurrentEL before switch
    unsigned long vbar, el;
    asm volatile("mrs %0, vbar_el1" : "=r"(vbar));
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    puts("Pre-switch VBAR: "); puthex(vbar); puts("\n");
    puts("Pre-switch EL: "); puthex(el >> 2); puts("\n");
    
    extern void _start();
    puts("_start content:\n");
    unsigned int *s = (unsigned int *)_start;
    for (int i = 0; i < 8; i++) {
        puthex(s[i]);
        puts(" ");
    }
    puts("\n");

    puts("Vectors content:\n");
    unsigned int *v = (unsigned int *)vectors;
    for (int i = 0; i < 8; i++) {
        puthex(v[i]);
        puts(" ");
    }
    puts("\n");

    // Switch Core 0 to EL0 as well
    unsigned long current_sp;
    asm volatile("mov %0, sp" : "=r"(current_sp));
    extern void vectors();
    switch_to_el0(user_main, (void*)(current_sp - 2048), 0, vectors);
    
    // Loop indefinitely to halt execution
    while (1) {
        char c = getc();
        puts("\n You pressed: ");
        putc(c);
    }
}

#include "syscalls.h"

void user_main(int core_id) {
    puts("U");
    
    unsigned long sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    puts(" SP: ");
    puthex(sp);
    puts("\n");

    sys_puts(" is running in User Mode via SVC!\n");
    
    while(1) {}
}

void secondary_core_entry() {
    puts("Secondary core starting...\n");
    int core_id = get_core_id();
    
    // Calculate a separate stack for EL0 (just below the current stack)
    // In a real OS, we would allocate a new page.
    // Current stack is at _stack_top - (core_id * 4096)
    // Let's just use the same stack pointer for simplicity in this demo, 
    // effectively "downgrading" the stack to user mode.
    // Or better, subtract 2048 to give some space.
    unsigned long current_sp;
    asm volatile("mov %0, sp" : "=r"(current_sp));
    
    extern void vectors();
    switch_to_el0(user_main, (void*)(current_sp - 2048), core_id, vectors);
}