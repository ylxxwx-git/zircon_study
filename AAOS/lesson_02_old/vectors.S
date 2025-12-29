.section .text
.global vectors
.global handle_sync

// Macro to align handlers to 128 bytes (0x80)
.macro VECTOR handler
.align 7
    b \handler
.endm

.align 12 // Vector table must be aligned to 4096 bytes (2^12)
vectors:
    // Current EL with SP0
    .align 7
    mov x0, #0x09000000
    mov w1, #'0'
    str w1, [x0]
    b .
    .align 7
    VECTOR invalid_handler // IRQ
    VECTOR invalid_handler // FIQ
    VECTOR invalid_handler // SError

    // Current EL with SPx
    .align 7
    mov x0, #0x09000000
    mov w1, #'X'
    str w1, [x0]
    b .
    .align 7
    VECTOR invalid_handler // IRQ
    VECTOR invalid_handler // FIQ
    VECTOR invalid_handler // SError

    // Lower EL using AArch64 (This is what we need for EL0 -> EL1)
    .align 7
    // Inline debug handler
    mov x0, #0x09000000
    mov w1, #'S'
    str w1, [x0]
    mrs x0, elr_el1
    add x0, x0, #4
    msr elr_el1, x0
    eret
    .align 7 // Ensure next vector is aligned
    
    VECTOR invalid_handler // IRQ
    VECTOR invalid_handler // FIQ
    VECTOR invalid_handler // SError

    // Lower EL using AArch32
    VECTOR invalid_handler
    VECTOR invalid_handler
    VECTOR invalid_handler
    VECTOR invalid_handler

el0_sync:
    // Debug: Print 'S' directly to UART
    mov x0, #0x09000000
    mov w1, #'S'
    str w1, [x0]
    
    // Skip the SVC instruction to avoid infinite loop?
    // ELR_EL1 points to the svc instruction.
    // We need to add 4 to ELR_EL1 to skip it.
    mrs x0, elr_el1
    add x0, x0, #4
    msr elr_el1, x0
    
    eret

invalid_handler:
    // Debug: Print 'I' directly to UART
    mov x0, #0x09000000
    mov w1, #'I'
    str w1, [x0]
    b invalid_handler
