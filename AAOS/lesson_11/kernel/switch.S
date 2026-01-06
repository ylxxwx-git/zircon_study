#include "sched.h"

.global cpu_switch_to

// void cpu_switch_to(struct task_struct* prev, struct task_struct* next);
// x0 = prev, x1 = next
cpu_switch_to:
    // 1. Save Callee-Saved Registers to prev->cpu_context
    mov x10, #THREAD_CPU_CONTEXT
    add x8, x0, x10
    
    stp x19, x20, [x8], #16
    stp x21, x22, [x8], #16
    stp x23, x24, [x8], #16
    stp x25, x26, [x8], #16
    stp x27, x28, [x8], #16
    
    // Save FP and SP
    mov x9, sp
    stp x29, x9, [x8], #16
    
    // Save LR as PC
    str x30, [x8]
    
    // 2. Restore Callee-Saved Registers from next->cpu_context
    add x8, x1, x10
    
    ldp x19, x20, [x8], #16
    ldp x21, x22, [x8], #16
    ldp x23, x24, [x8], #16
    ldp x25, x26, [x8], #16
    ldp x27, x28, [x8], #16
    
    // Restore FP and SP
    ldp x29, x9, [x8], #16
    
    // Restore LR (PC)
    ldr x30, [x8]
    
    // Restore SP
    mov sp, x9
    
    ret

.global ret_from_fork
ret_from_fork:
    // 1. Enable Interrupts
    msr daifclr, #2
    
    // 2. Call Thread Function (stored in x19)
    blr x19
    
    // 3. Should never return, but if it does, loop
    b .
