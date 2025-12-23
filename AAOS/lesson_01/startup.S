// =================================================================
// startup.s - AArch64 entry point.
// Initializes the stack and jumps to the C code.
// =================================================================

.section .text.boot
.global _start


// The entry point for the kernel, defined by the linker script
_start:
    // Stack pointer = _stack_top - (CoreID * 4096)
    ldr x9, =_stack_top
    mrs x10, mpidr_el1
    and x10, x10, #0xFF      // x10 = Core ID
    sub x9, x9, x10, lsl #12 // x9 = x9 - (x10 << 12)
    mov sp, x9
    
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
