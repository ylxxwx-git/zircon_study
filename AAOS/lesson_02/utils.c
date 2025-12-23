#include "utils.h"

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
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return mpidr & 0xFF; // Extract Affinity Level 0 (core ID)
}

int get_el() {
    unsigned long el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 0x3;
}

void switch_to_el0(void (*entry_point)(), void *stack_top, unsigned long arg0, void *vectors) {
    asm volatile(
        "msr vbar_el1, %4\n"  // Set VBAR_EL1
        "isb\n"
        "msr sp_el0, %1\n"    // Set User Stack Pointer
        "msr elr_el1, %0\n"   // Set Exception Link Register (Return Address)
        "msr spsr_el1, %2\n"  // Set Saved Program Status Register (EL0t, IRQ masked)
        "mov x0, %3\n"        // Set x0 (Argument 1 for entry_point)
        "eret\n"              // Return from Exception (Jump to EL0)
        :
        : "r"(entry_point), "r"(stack_top), "r"(0x3C0), "r"(arg0), "r"(vectors)
        : "x0", "memory"
    );
}
