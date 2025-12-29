.section .text.boot
.global _start


// The entry point for the kernel, defined by the linker script
_start:
    // Set SPSel to 1 (Use SP_ELx for Exception Levels)
    msr spsel, #1
    
    // Set VBAR_EL1 (Vector Base Address Register)
    ldr x0, =vectors
    msr vbar_el1, x0
    msr daifclr, #0xF
    ic iallu
    dsb sy
    isb
    
    // Debug: Print 'V'
    mov x0, #0x09000000
    mov w1, #'V'
    str w1, [x0]

    // Stack pointer = _stack_top - (CoreID * 4096)
    ldr x9, =_stack_top
    mrs x10, mpidr_el1
    and x10, x10, #0xFF      // x10 = Core ID
    sub x9, x9, x10, lsl #12 // x9 = x9 - (x10 << 12)
    mov sp, x9
    
    b secondary_core_entry

// Macro to align handlers to 128 bytes (0x80)
.macro VECTOR handler
.align 7
    b \handler
.endm

// .section .data
.align 11 // Vector table must be aligned to 2048 bytes (2^11)
.global vectors
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
    
    // Minimal handler: Skip SVC instruction and return
    // Use x30 (LR) to avoid clobbering x0 (arg)
    mrs x30, elr_el1
    add x30, x30, #4
    msr elr_el1, x30
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

invalid_handler:
    // Debug: Print 'I' directly to UART
    mov x0, #0x09000000
    mov w1, #'I'
    str w1, [x0]
    b invalid_handler

secondary_core_entry:
    // Secondary cores skip BSS clearing
    cbnz x10, bss_end

    // Clear BSS
    ldr x4, =_bss_start
    ldr x5, =_bss_end
bss_loop:
    cmp x4, x5
    b.ge bss_end
    str xzr, [x4], #8    // Store zero and increment address by 8
    b bss_loop

bss_end:
    // 3. Jump to the C entry function
    bl c_entry           // Branch and Link to the c_entry function in main.c

// Loop forever if c_entry ever returns (it shouldn't in this simple OS)
halt:
    wfi
    b halt

