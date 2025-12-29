.section .text.boot
.global _start

_start:
    // Check Current Exception Level
    mrs x0, CurrentEL
    lsr x0, x0, #2
    and x0, x0, #3
    
    cmp x0, #3
    b.eq el3_to_el1
    
    cmp x0, #2
    b.eq el2_to_el1
    
    // If already in EL1, just continue
    b el1_entry

el3_to_el1:
    // Configure SCR_EL3 (Secure Configuration Register)
    // RW [10]: 1 = Next lower level is AArch64
    // NS [0]:  1 = Non-secure state
    mov x0, #(1 << 10)
    orr x0, x0, #(1 << 0)
    msr scr_el3, x0
    
    // Set SPSR_EL3 (Saved Program Status Register)
    // M [3:0]: 0101 (EL1h) - Return to EL1 using SP_EL1
    // DAIF [9:6]: 1111 (Mask all interrupts)
    ldr x0, =0x3C5
    msr spsr_el3, x0
    
    // Set ELR_EL3 (Exception Link Register) to el1_entry
    adr x0, el1_entry
    msr elr_el3, x0
    
    // We also need to configure HCR_EL2 if we are skipping EL2, 
    // but usually EL3->EL2->EL1 is safer. 
    // However, for QEMU virt, we can often jump straight if we don't care about EL2.
    // BUT, if we skip EL2, HCR_EL2 might be 0, which means EL1 is AArch32? 
    // No, SCR_EL3.RW controls EL2/EL1 width if HCR_EL2 is not involved?
    // Actually, if we are in EL3, we should probably initialize HCR_EL2 too if we want EL1 to be 64-bit.
    // Let's go EL3 -> EL2 -> EL1 to be safe and standard.
    
    // Redo: EL3 -> EL2
    // Set SPSR_EL3 to EL2h (1001)
    ldr x0, =0x3C9
    msr spsr_el3, x0
    adr x0, el2_to_el1
    msr elr_el3, x0
    eret

el2_to_el1:
    // Configure HCR_EL2 (Hypervisor Configuration Register)
    // RW [31]: 1 = EL1 is AArch64
    mov x0, #(1 << 31)
    // SWIO [1]: 1 = Set VMID to 0 (optional)
    msr hcr_el2, x0
    
    // Configure SCTLR_EL1 (System Control Register) - Optional but good practice
    // Little Endian, etc. Reset value is usually unknown.
    // For now, we can leave it or set to 0 to be safe (MMU disabled).
    ldr x0, =0x30d00800 // Some default value or just 0
    // actually, just ensuring MMU is off is enough for now.
    mov x0, #0x0800 // I-Cache disabled, C-Cache disabled, MMU disabled
    mov x0, #0 // Use x0
    msr sctlr_el1, x0
    
    // Set SPSR_EL2
    // M [3:0]: 0101 (EL1h)
    // DAIF [9:6]: 1111 (Mask all interrupts)
    ldr x0, =0x3C5
    msr spsr_el2, x0
    
    // Set ELR_EL2
    adr x0, el1_entry
    msr elr_el2, x0
    
    eret

el1_entry:
    // =============================================================
    // We are now in EL1
    // =============================================================
    
    // 1. Setup Stack
    // Stack pointer = _stack_top - (CoreID * 4096)
    ldr x9, =_stack_top
    mrs x10, mpidr_el1
    and x10, x10, #0xFF      // x10 = Core ID
    sub x9, x9, x10, lsl #12 // x9 = x9 - (x10 << 12)
    mov sp, x9
    
    // 2. Clear BSS (Only Main Core 0)
    cbnz x10, bss_end
    
    ldr x4, =_bss_start
    ldr x5, =_bss_end
bss_loop:
    cmp x4, x5
    b.ge bss_end
    str xzr, [x4], #8
    b bss_loop

bss_end:
    // 3. Jump to C
    bl kernel_main

halt:
    wfi
    b halt
