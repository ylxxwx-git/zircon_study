.section .text.boot
.global _start
.global el1_entry

_start:
    // Check Current Exception Level
    mrs x0, CurrentEL
    lsr x0, x0, #2
    and x0, x0, #3

    cmp x0, #3
    b.eq el3_entry

    cmp x0, #2
    b.eq el2_to_el1
    
    // If already in EL1, just continue
    b el1_entry

el3_entry:
    // Check if we are the primary core (Core 0)
    mrs x0, mpidr_el1
    and x0, x0, #0xFF
    cbnz x0, el3_park

    // Primary core continues to EL1
    b el3_to_el1

el3_park:
    // Wait for mailbox to be non-zero
    // Calculate mailbox address: mailbox + (core_id * 8)
    ldr x1, =mailbox
    lsl x2, x0, #3 // x2 = core_id * 8
    add x1, x1, x2 // x1 = address of mailbox entry

wait_loop:
    wfe
    ldr x2, [x1]
    cbz x2, wait_loop

    // Target address is in x2.
    // We need to jump to it, but we also need to drop to EL1.
    // We can reuse el3_to_el1 logic but set ELR_EL3 to x2.
    
    // Configure SCR_EL3 (Secure Configuration Register)
    mov x0, #(1 << 10) // RW=1 (AArch64)
    orr x0, x0, #(1 << 8) // Enable HVC (HCE)
    orr x0, x0, #(1 << 0) // NS=1 (Non-secure)
    msr scr_el3, x0

    // Set SPSR_EL3 to EL2h (1001)
    mov x0, #0x3C9
    msr spsr_el3, x0

    // Set ELR_EL3 to target address
    msr elr_el3, x2

    // We also need to initialize VBAR_EL3 for the secondary cores too
    ldr x0, =el3_vector_table
    msr vbar_el3, x0

    eret


el3_to_el1:
    // Set VBAR_EL3 (Vector Base Address Register)

    ldr x0, =el3_vector_table
    msr vbar_el3, x0


    // Configure SCR_EL3 (Secure Configuration Register)
    // RW [10]: 1 = Next lower level is AArch64
    // NS [0]:  1 = Non-secure state
    mov x0, #(1 << 10)
    orr x0, x0, #(1 << 8) // Enable HVC (HCE)
    orr x0, x0, #(1 << 0)
    msr scr_el3, x0
    
    // We also need to configure HCR_EL2 if we are skipping EL2, 
    // but usually EL3->EL2->EL1 is safer. 
    // However, for QEMU virt, we can often jump straight if we don't care about EL2.
    // BUT, if we skip EL2, HCR_EL2 might be 0, which means EL1 is AArch32? 
    // No, SCR_EL3.RW controls EL2/EL1 width if HCR_EL2 is not involved?
    // Actually, if we are in EL3, we should probably initialize HCR_EL2 too if we want EL1 to be 64-bit.
    // Let's go EL3 -> EL2 -> EL1 to be safe and standard.
    
    // Redo: EL3 -> EL2
    // Set SPSR_EL3 to EL2h (1001)
    mov x0, #0x3C9
    msr spsr_el3, x0
    adr x0, el2_to_el1
    msr elr_el3, x0
    eret

el2_to_el1:
    // Configure HCR_EL2 (Hypervisor Configuration Register)


    // Configure HCR_EL2 (Hypervisor Configuration Register)
    // RW [31]: 1 = EL1 is AArch64
    mov x0, #(1 << 31)
    // SWIO [1]: 1 = Set VMID to 0 (optional)
    msr hcr_el2, x0

    // Set VBAR_EL2 (Vector Base Address Register)
    ldr x0, =el2_vector_table
    msr vbar_el2, x0
    
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

// =============================================================
// EL2 Exception Vector Table
// =============================================================
.align 11 // Vector tables must be aligned to 2KB (2^11)
el2_vector_table:
    // Current EL with SP0
    .align 7; b . // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Current EL with SPx
    .align 7; b . // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Lower EL using AArch64 (This is where HVC from EL1 lands)
    .align 7; b el2_sync_handler // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Lower EL using AArch32
    .align 7; b . // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

// =============================================================
// EL2 Synchronous Exception Handler
// =============================================================
el2_sync_handler:
    // 1. Save x30 (LR) to TPIDR_EL2 (Scratch)
    msr tpidr_el2, x30

    // 2. Read Exception Syndrome Register (ESR_EL2)
    mrs x30, esr_el2
    
    // 3. Check EC (Exception Class) - Bits [31:26]
    lsr x30, x30, #26
    cmp x30, #0x16 // 0x16 = HVC instruction execution in AArch64 state
    b.ne . // If not HVC, hang (or handle other exceptions)
    
    // 4. Forward to EL3 via SMC
    // Arguments are already in x0-x7, which SMC uses.
    smc #0
    
    // 5. Restore x30
    mrs x30, tpidr_el2
    
    // 6. Return to EL1
    eret

// =============================================================
// EL3 Exception Vector Table
// =============================================================
.align 11 // Vector tables must be aligned to 2KB (2^11)
el3_vector_table:
    // Current EL with SP0
    .align 7; b . // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Current EL with SPx
    .align 7; b . // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Lower EL using AArch64 (Target for SMC from EL2/EL1)
    .align 7; b el3_sync_handler // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

    // Lower EL using AArch32
    .align 7; b . // Synchronous
    .align 7; b . // IRQ
    .align 7; b . // FIQ
    .align 7; b . // SError

// =============================================================
// EL3 Synchronous Exception Handler (SMC Handler)
// =============================================================
el3_sync_handler:
    // 1. Check if it's an SMC (ESR_EL3.EC == 0x17)
    mrs x30, esr_el3
    lsr x30, x30, #26
    cmp x30, #0x17 // SMC instruction
    b.ne . // Hang if not SMC

    // 2. Handle PSCI
    // x0 = Function ID
    // x1 = Target CPU (Affinity)
    // x2 = Entry Point
    // x3 = Context ID (Ignored)

    // Check for PSCI_CPU_ON (0xC4000003) or 0x84000003 (32-bit)
    ldr x30, =0xC4000003
    cmp x0, x30
    b.ne . // Only support CPU_ON for now

    // Store Entry Point (x2) into Mailbox for Target CPU (x1)
    ldr x30, =mailbox
    and x1, x1, #0xFF // Mask to get core ID (assuming affinity 0)
    lsl x1, x1, #3    // x1 * 8
    add x30, x30, x1
    str x2, [x30]

    // Wake up all cores (simple but effective)
    sev

    // Return SUCCESS (0)
    mov x0, #0 
    eret

// =============================================================
// Data Section
// =============================================================
.section .data
.balign 8
mailbox:
    .skip 8 * 4 // 4 cores, 8 bytes each


