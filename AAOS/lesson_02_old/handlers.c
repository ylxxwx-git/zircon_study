#include "output.h"

// Exception Syndrome Register (ESR_EL1) decoding
#define ESR_EC_SHIFT 26
#define ESR_EC_MASK  0x3F
#define EC_SVC_64    0x15

void handle_sync(unsigned long *regs) {
    unsigned long esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    
    unsigned long ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    
    if (ec == EC_SVC_64) {
        // It's a System Call!
        // x0 = regs[0] (First argument)
        // x8 = regs[8] (Syscall number - convention)
        
        // For now, we only support one syscall: puts
        // Argument is in x0 (string pointer)
        char *str = (char *)regs[0];
        puts(str);
    } else {
        puts("Unknown Exception!\n");
        while(1);
    }
}
