#include "output.h"

// Inline assembly to get the current Exception Level
int get_el() {
    unsigned long el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3;
}

void kernel_main() {
    // Initialize UART (already done if we use the same output.c, but good to be sure)
    // In lesson_01 output.c, puts calls early_putc which writes to 0x09000000 directly.
    
    puts("\n[Lesson 02] Booting...\n");
    
    int el = get_el();
    puts("Current Exception Level: ");
    puthex(el);
    puts("\n");
    
    if (el == 1) {
        puts("Success: We are in EL1 (Kernel Mode)!\n");
    } else {
        puts("Warning: We are NOT in EL1.\n");
    }
    
    // Halt
    while(1) {}
}
