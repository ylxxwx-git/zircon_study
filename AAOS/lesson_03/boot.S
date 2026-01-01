.section .text.boot
.global _start

_start:
    // We assume we are in EL1 (or we don't care about EL3/EL2 for now)
    // QEMU default without secure=on starts in EL1 (or EL2 if virtualization=on)
    // For simplicity, let's just check if we are in EL1.
    
    mrs x0, CurrentEL
    lsr x0, x0, #2
    and x0, x0, #3
    
    cmp x0, #1
    b.eq el1_entry
    
    // If not EL1, we might be in EL2 or EL3.
    // For this lesson, we assume the user runs QEMU correctly to land in EL1 or we just hang.
    // But to be nice, let's try to drop from EL2 if needed (minimal).
    cmp x0, #2
    b.eq el2_to_el1
    
    // If EL3, just hang for now (simplification)
    b .

el2_to_el1:
    // Minimal EL2 -> EL1
    mov x0, #(1 << 31) // HCR_EL2.RW = 1 (AArch64)
    msr hcr_el2, x0
    
    mov x0, #0x3C5 // SPSR_EL2 (EL1h, Masked)
    msr spsr_el2, x0
    
    adr x0, el1_entry
    msr elr_el2, x0
    
    eret

el1_entry:
    // 1. Setup Stack
    ldr x0, =_stack_top
    mov sp, x0
    
    // 2. Setup Vector Table
    ldr x0, =el1_vector_table
    msr vbar_el1, x0
    
    // 3. Clear BSS
    ldr x0, =_bss_start
    ldr x1, =_bss_end
bss_loop:
    cmp x0, x1
    b.ge bss_done
    str xzr, [x0], #8
    b bss_loop

bss_done:
    // 4. Jump to C
    bl kernel_main
    
halt:
    wfi
    b halt

// =============================================================
// EL1 Exception Vector Table
// =============================================================
.align 11
el1_vector_table:
    // Current EL with SP0
    .align 7; b .
    .align 7; b .
    .align 7; b .
    .align 7; b .

    // Current EL with SPx
    .align 7; b . // Sync
    .align 7; b irq_handler // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Lower EL using AArch64
    .align 7; b .
    .align 7; b irq_handler // IRQ
    .align 7; b .
    .align 7; b .

    // Lower EL using AArch32
    .align 7; b .
    .align 7; b .
    .align 7; b .
    .align 7; b .

// =============================================================
// IRQ Handler Wrapper
// =============================================================
irq_handler:
    // 1. Save Context
    // We must save x0-x18 (caller-saved), x29, x30 (LR)
    // For simplicity, let's save pairs
    sub sp, sp, #256 // Reserve space
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x30, [sp, #16 * 9] // Save LR with x18
    
    // 2. Call C Handler
    bl irq_handler_c
    
    // 3. Restore Context
    ldp x0, x1, [sp, #16 * 0]
    ldp x2, x3, [sp, #16 * 1]
    ldp x4, x5, [sp, #16 * 2]
    ldp x6, x7, [sp, #16 * 3]
    ldp x8, x9, [sp, #16 * 4]
    ldp x10, x11, [sp, #16 * 5]
    ldp x12, x13, [sp, #16 * 6]
    ldp x14, x15, [sp, #16 * 7]
    ldp x16, x17, [sp, #16 * 8]
    ldp x18, x30, [sp, #16 * 9]
    add sp, sp, #256
    
    // 4. Return
    // eret will restore state from SPSR_EL1 and PC from ELR_EL1.
    // - If SPSR_EL1 indicates EL0 (Mode=0), we return to User Space.
    // - If SPSR_EL1 indicates EL1 (Mode=5), we return to Kernel Space.
    // The hardware automatically handles the SP switch (SP_EL0 vs SP_EL1).
    eret
