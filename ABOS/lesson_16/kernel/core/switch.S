/* =========================================================================
 * switch.S - Zircon-Style AArch64 Context Switch (arm64_context_switch)
 * References:
 *   - zircon_org/kernel/arch/arm64/asm.S (arm64_context_switch)
 * ========================================================================= */

.section .text
.global arm64_context_switch

/* void arm64_context_switch(vaddr_t *old_sp, vaddr_t new_sp); */
arm64_context_switch:
    /* Save callee-saved registers + tpidr_el0/tpidrro_el0 (14 * 8 = 112 bytes) */
    sub     sp, sp, #112
    mrs     x9,  tpidr_el0
    mrs     x10, tpidrro_el0
    stp     x9,  x10, [sp, #(16 * 0)]
    stp     x19, x20, [sp, #(16 * 1)]
    stp     x21, x22, [sp, #(16 * 2)]
    stp     x23, x24, [sp, #(16 * 3)]
    stp     x25, x26, [sp, #(16 * 4)]
    stp     x27, x28, [sp, #(16 * 5)]
    stp     x29, x30, [sp, #(16 * 6)]

    /* Store old stack pointer into *old_sp (x0) */
    mov     x9, sp
    str     x9, [x0]

    /* Load new stack pointer from new_sp (x1) */
    mov     sp, x1

    /* Restore callee-saved registers + tpidr_el0/tpidrro_el0 */
    ldp     x9,  x10, [sp, #(16 * 0)]
    msr     tpidr_el0,   x9
    msr     tpidrro_el0, x10
    ldp     x19, x20, [sp, #(16 * 1)]
    ldp     x21, x22, [sp, #(16 * 2)]
    ldp     x23, x24, [sp, #(16 * 3)]
    ldp     x25, x26, [sp, #(16 * 4)]
    ldp     x27, x28, [sp, #(16 * 5)]
    ldp     x29, x30, [sp, #(16 * 6)]
    add     sp, sp, #112
    ret
