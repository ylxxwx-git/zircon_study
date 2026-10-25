/* =========================================================================
 * exceptions.S - EL1 Exception Vector Table & iframe_t Save/Restore Macros
 * References:
 *   - zircon_org/kernel/arch/arm64/exceptions.S
 * ========================================================================= */

#define ARM64_EXCEPTION_FLAG_LOWER_EL 0x1
#define IFRAME_SIZE 288

.macro regsave, flags
    sub     sp, sp, #IFRAME_SIZE
    stp     x0,  x1,  [sp, #(16 * 0)]
    stp     x2,  x3,  [sp, #(16 * 1)]
    stp     x4,  x5,  [sp, #(16 * 2)]
    stp     x6,  x7,  [sp, #(16 * 3)]
    stp     x8,  x9,  [sp, #(16 * 4)]
    stp     x10, x11, [sp, #(16 * 5)]
    stp     x12, x13, [sp, #(16 * 6)]
    stp     x14, x15, [sp, #(16 * 7)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(16 * 9)]
    stp     x20, x21, [sp, #(16 * 10)]
    stp     x22, x23, [sp, #(16 * 11)]
    stp     x24, x25, [sp, #(16 * 12)]
    stp     x26, x27, [sp, #(16 * 13)]
    stp     x28, x29, [sp, #(16 * 14)]

    mrs     x9,  sp_el0
    mrs     x10, elr_el1
    mrs     x11, spsr_el1
    mrs     x12, mdscr_el1
    stp     x30, x9,  [sp, #(16 * 15)]
    stp     x10, x11, [sp, #(16 * 16)]
    stp     x12, xzr, [sp, #(16 * 17)]
.endm

.macro regrestore
    ldp     x30, x9,  [sp, #(16 * 15)]
    ldp     x10, x11, [sp, #(16 * 16)]
    ldp     x12, xzr, [sp, #(16 * 17)]
    msr     sp_el0,    x9
    msr     elr_el1,   x10
    msr     spsr_el1,  x11
    msr     mdscr_el1, x12

    ldp     x0,  x1,  [sp, #(16 * 0)]
    ldp     x2,  x3,  [sp, #(16 * 1)]
    ldp     x4,  x5,  [sp, #(16 * 2)]
    ldp     x6,  x7,  [sp, #(16 * 3)]
    ldp     x8,  x9,  [sp, #(16 * 4)]
    ldp     x10, x11, [sp, #(16 * 5)]
    ldp     x12, x13, [sp, #(16 * 6)]
    ldp     x14, x15, [sp, #(16 * 7)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(16 * 9)]
    ldp     x20, x21, [sp, #(16 * 10)]
    ldp     x22, x23, [sp, #(16 * 11)]
    ldp     x24, x25, [sp, #(16 * 12)]
    ldp     x26, x27, [sp, #(16 * 13)]
    ldp     x28, x29, [sp, #(16 * 14)]
    add     sp, sp, #IFRAME_SIZE
    eret
.endm

.section .text
.align 11
.global arm64_el1_exception_base
arm64_el1_exception_base:
    /* Current EL with SP_EL0 */
    .align 7; b .Lel1_sync
    .align 7; b .Lel1_irq
    .align 7; b .Lel1_unhandled
    .align 7; b .Lel1_unhandled

    /* Current EL with SP_ELx (Kernel mode exceptions & IRQs) */
    .align 7; b .Lel1_sync
    .align 7; b .Lel1_irq
    .align 7; b .Lel1_unhandled
    .align 7; b .Lel1_unhandled

    /* Lower EL using AArch64 (User mode EL0 syscalls, faults & IRQs) */
    .align 7; b .Lel0_sync
    .align 7; b .Lel0_irq
    .align 7; b .Lel1_unhandled
    .align 7; b .Lel1_unhandled

    /* Lower EL using AArch32 */
    .align 7; b .Lel1_unhandled
    .align 7; b .Lel1_unhandled
    .align 7; b .Lel1_unhandled
    .align 7; b .Lel1_unhandled

.Lel1_sync:
    regsave 0
    mov     x0, sp
    mov     w1, #0
    mrs     x2, esr_el1
    bl      arm64_sync_exception
    regrestore

.Lel1_irq:
    regsave 0
    mov     x0, sp
    mov     w1, #0
    bl      arm64_irq_exception
    regrestore

.Lel0_sync:
    regsave ARM64_EXCEPTION_FLAG_LOWER_EL
    mov     x0, sp
    mov     w1, #ARM64_EXCEPTION_FLAG_LOWER_EL
    mrs     x2, esr_el1
    bl      arm64_sync_exception
    regrestore

.Lel0_irq:
    regsave ARM64_EXCEPTION_FLAG_LOWER_EL
    mov     x0, sp
    mov     w1, #ARM64_EXCEPTION_FLAG_LOWER_EL
    bl      arm64_irq_exception
    regrestore

.Lel1_unhandled:
    regsave 0
    mov     x0, sp
    mov     w1, #0
    mrs     x2, esr_el1
    bl      arm64_sync_exception
1:  wfe
    b       1b
