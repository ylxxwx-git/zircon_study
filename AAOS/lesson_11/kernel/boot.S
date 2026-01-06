.section .idmap.text, "ax"
.global _start

// Constants for Page Table Attributes
.equ MAIR_VALUE,    0x000000000000FF00 // Attr0=Device, Attr1=Normal
.equ TCR_VALUE,     0x00000000B5210021 // T0SZ=33, T1SZ=33 (2GB), TG0=4K, TG1=4K, IPS=32bit, SH1=3, ORGN1=1, IRGN1=1

.equ PTE_BLOCK,     0x401 // Valid, Table/Block, Access
.equ PTE_DEVICE,    0x401 | (0 << 2) | (1 << 10) | (1 << 53) | (1 << 54) // Attr0, AF, PXN, UXN
.equ PTE_RAM,       0x401 | (1 << 2) | (1 << 10) | (3 << 8) // Attr1, AF, ISH

_start:
    // Save DTB Pointer
    mov x20, x0

    // 1. Check Core ID
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, master_core
    
    // Secondary Cores: Wait for MMU ready flag
    adr x1, mmu_ready_flag
wait_for_mmu:
    ldr w2, [x1]
    cbz w2, wait_for_mmu
    
    // Secondary Cores: Enable MMU
    b enable_mmu_secondary

master_core:
    // Setup Early Vector Table
    ldr x0, =early_vector_table
    msr vbar_el1, x0
    isb
    
    // 2. Setup Page Tables
    // Setup idmap_table (TTBR0)
    ldr x0, =idmap_table
    
    // Entry 0: 0x00000000 (Device) - For UART
    mov x1, #0
    ldr x2, =PTE_DEVICE
    orr x2, x1, x2
    str x2, [x0, #0]
    
    // Entry 1: 0x40000000 (RAM) - For Boot Code (Covers 0x40000000-0x7FFFFFFF)
    ldr x1, =0x40000000
    ldr x2, =PTE_RAM
    orr x2, x1, x2
    str x2, [x0, #8]
    
    // Setup kernel_table (TTBR1)
    ldr x0, =kernel_table
    
    // Index 0: 0xFFFFFFFF80000000 -> 0x40000000 (RAM)
    ldr x1, =0x40000000
    ldr x2, =PTE_RAM
    orr x2, x1, x2
    str x2, [x0, #0]
    
    // Index 1: 0xFFFFFFFFC0000000 -> 0x00000000 (Device)
    mov x1, #0
    ldr x2, =PTE_DEVICE
    orr x2, x1, x2
    str x2, [x0, #8]
    
    // 3. Setup MAIR
    ldr x0, =MAIR_VALUE
    msr mair_el1, x0
    
    // 4. Setup TCR
    ldr x0, =TCR_VALUE
    msr tcr_el1, x0
    
    // 5. Setup TTBR0 and TTBR1
    ldr x0, =idmap_table
    msr ttbr0_el1, x0
    ldr x0, =kernel_table
    msr ttbr1_el1, x0
    isb
    
    // 5.5 Invalidate TLB and Caches
    tlbi vmalle1
    ic iallu
    dsb ish
    isb
    
    // 6. Enable MMU (No Caches for debugging)
    mov x0, #0
    msr sctlr_el1, x0  // Clear SCTLR first
    orr x0, x0, #1     // M bit
    // orr x0, x0, #4     // C bit (Cache)
    // orr x0, x0, #0x1000 // I bit (I-Cache)
    msr sctlr_el1, x0
    isb
    
    // 7. Jump to High Kernel (el1_entry)
    ldr x0, =el1_entry
    
    // Signal secondary cores that MMU tables are ready
    adr x1, mmu_ready_flag
    mov w2, #1
    str w2, [x1]
    
    // Ensure visibility
    dsb ish
    sev
    
    // Jump
    br x0

enable_mmu_secondary:
    // 3. Setup MAIR (Same as master)
    ldr x0, =MAIR_VALUE
    msr mair_el1, x0
    
    // 4. Setup TCR (Same as master)
    ldr x0, =TCR_VALUE
    msr tcr_el1, x0
    
    // 5. Setup TTBR0 and TTBR1 (Point to same tables)
    ldr x0, =idmap_table
    msr ttbr0_el1, x0
    ldr x0, =kernel_table
    msr ttbr1_el1, x0
    isb
    
    // 5.5 Invalidate TLB
    tlbi vmalle1
    ic iallu
    dsb ish
    isb
    
    // 6. Enable MMU
    mov x0, #0
    msr sctlr_el1, x0
    orr x0, x0, #1     // M bit
    msr sctlr_el1, x0
    isb
    
    // 7. Jump to High Kernel
    ldr x0, =el1_entry
    br x0

bad_target:
    b .

// Early Exception Vector
.align 11
early_vector_table:
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler
    .align 7; b early_handler

early_handler:
    mov x1, #0x09000000
    mov w2, #'!'
    str w2, [x1]
    b .

.section .idmap.data, "aw"
.align 4
mmu_ready_flag: .word 0

.align 12
.global idmap_table
idmap_table:
    .space 4096
kernel_table:
    .space 4096

.section .text
.global el1_entry
el1_entry:
    // 1. Setup Stack (Per Core)
    mrs x0, mpidr_el1
    and x0, x0, #3      // x0 = Core ID
    
    ldr x1, =_stack_top
    mov x2, #4096       // Stack size per core
    mul x2, x2, x0      // Offset = CoreID * 4096
    sub x1, x1, x2      // SP = _stack_top - Offset
    mov sp, x1

    // 2. Setup Vector Table
    ldr x0, =el1_vector_table
    msr vbar_el1, x0
    
    // 3. Clear BSS (Only Core 0 should do this)
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbz x0, init_bss
    b skip_bss

init_bss:
    ldr x0, =_bss_start
    ldr x1, =_bss_end
bss_loop:
    cmp x0, x1
    b.ge bss_done
    str xzr, [x0], #8
    b bss_loop

bss_done:
    // Ensure BSS clear is visible to other cores
    dsb ish

skip_bss:
    // 4. Jump to C
    // Pass DTB pointer (saved in x20) as first argument (x0)
    mov x0, x20
    bl kernel_main
    b .
    
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
    .align 7; b svc_handler // Sync (was b .)
    .align 7; b irq_handler // IRQ
    .align 7; b . // FIQ
    .align 7; b svc_handler // SError (was b .)

    // Lower EL using AArch64
    .align 7; b svc_handler // Sync (SVC)
    .align 7; b irq_handler // IRQ
    .align 7; b .
    .align 7; b .

    // Lower EL using AArch32
    .align 7; b .
    .align 7; b .
    .align 7; b .
    .align 7; b .

// =============================================================
// SVC Handler Wrapper
// =============================================================
svc_handler:
    // 1. Save Context (x0-x30, SPSR, ELR)
    // We reuse the same stack frame structure as IRQ for simplicity
    sub sp, sp, #288
    
    stp x0, x1, [sp, #16 * 0]
    stp x2, x3, [sp, #16 * 1]
    stp x4, x5, [sp, #16 * 2]
    stp x6, x7, [sp, #16 * 3]
    stp x8, x9, [sp, #16 * 4]
    stp x10, x11, [sp, #16 * 5]
    stp x12, x13, [sp, #16 * 6]
    stp x14, x15, [sp, #16 * 7]
    stp x16, x17, [sp, #16 * 8]
    stp x18, x30, [sp, #16 * 9]
    
    mrs x0, spsr_el1
    mrs x1, elr_el1
    stp x0, x1, [sp, #16 * 10]
    
    // 2. Call C Handler
    // Arguments are already in x0-x7 (saved on stack, but also still in registers)
    // We pass the stack pointer to the handler so it can access/modify saved registers if needed
    mov x0, sp 
    
    // Read ESR_EL1 BEFORE enabling interrupts (IRQ clobbers it)
    mrs x1, esr_el1
    
    // Enable Interrupts (IRQ) so we can handle UART/Timer during syscalls
    msr daifclr, #2
    
    bl svc_handler_c
    
    // Disable Interrupts again before restoring context
    msr daifset, #2
    
    // 3. Restore Context
    ldp x0, x1, [sp, #16 * 10]
    msr spsr_el1, x0
    msr elr_el1, x1

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
    add sp, sp, #288
    
    eret

// =============================================================
// IRQ Handler Wrapper
// =============================================================
irq_handler:
    // 1. Save Context
    sub sp, sp, #288 // Reserve space (16 * 10 + 16 for SPSR/ELR + padding)
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
    
    // Save SPSR and ELR
    mrs x0, spsr_el1
    mrs x1, elr_el1
    stp x0, x1, [sp, #16 * 10]

    // 2. Call C Handler
    bl irq_handler_c
    
    // 3. Restore Context
    // Restore SPSR and ELR
    ldp x0, x1, [sp, #16 * 10]
    msr spsr_el1, x0
    msr elr_el1, x1

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
    add sp, sp, #288
    
    // 4. Return
    eret

// void move_to_user_mode(uint64_t entry_point, uint64_t stack_ptr);
.global move_to_user_mode
move_to_user_mode:
    // x0 = entry_point
    // x1 = stack_ptr
    
    // 1. Set SPSR_EL1 (Saved Program Status Register)
    // M[3:0] = 0000 (EL0t)
    // DAIF = 0 (Unmasked) or masked? Let's unmask IRQ/FIQ (0).
    mov x2, #0
    msr spsr_el1, x2
    
    // 2. Set ELR_EL1 (Exception Link Register) -> Return Address
    msr elr_el1, x0
    
    // 3. Set SP_EL0 (Stack Pointer for EL0)
    msr sp_el0, x1
    
    // 4. Return to EL0
    eret
