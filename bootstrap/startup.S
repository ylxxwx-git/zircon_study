// Assembly code must be compiled for the 64-bit ARM architecture (AArch64)

.section .text
.global _start

// The _start symbol is the entry point defined by our linker script
_start:
    // 1. Setup the Stack Pointer (SP)
    // We'll place the stack high up in the 256MB of RAM the QEMU 'virt' machine gives us (0x40000000)
    ldr x9, =0x40000000
    mov sp, x9

    // 2. Clear BSS section
    // The BSS section stores uninitialized global variables, which must be zeroed out.
    ldr x4, =_bss_start  // Load the start address of the BSS section (from linker script)
    ldr x5, =_bss_end    // Load the end address of the BSS section (from linker script)

bss_loop:
    cmp x4, x5           // Compare current address with end address
    b.ge bss_end         // If current >= end, exit loop
    str xzr, [x4]        // Store zero (xzr is always 0) at current address
    add x4, x4, #8       // Increment by 8 bytes (since we're storing a 64-bit zero)
    b bss_loop           // Loop again

bss_end:
    // 3. Jump to the C entry function
    bl c_entry           // Branch and Link (save return address) to the C entry function

// Loop forever if c_entry returns (which it shouldn't in a real OS)
halt:
    b halt