/* =========================================================================
 * uspace_entry.S - Enter EL0 User Space from Kernel Thread
 * Reference: zircon_org/kernel/arch/arm64/uspace_entry.S
 * ========================================================================= */

.section .text
.global arm64_uspace_entry

/* void arm64_uspace_entry(uintptr_t arg1, uintptr_t arg2, uintptr_t pc,
 *                         uintptr_t user_sp, vaddr_t kstack_top);
 */
arm64_uspace_entry:
    mov     sp, x4              /* Set EL1 kernel stack pointer */
    msr     sp_el0, x3          /* Set EL0 user stack pointer */
    msr     elr_el1, x2         /* Set EL0 entry PC */
    mov     x9, #0              /* SPSR_EL1 = 0 (EL0t, IRQs enabled) */
    msr     spsr_el1, x9
    msr     mdscr_el1, xzr

    /* Zero all general-purpose registers (except arg1=x0, arg2=x1) to avoid leaking kernel state */
    mov     x2,  xzr
    mov     x3,  xzr
    mov     x4,  xzr
    mov     x5,  xzr
    mov     x6,  xzr
    mov     x7,  xzr
    mov     x8,  xzr
    mov     x9,  xzr
    mov     x10, xzr
    mov     x11, xzr
    mov     x12, xzr
    mov     x13, xzr
    mov     x14, xzr
    mov     x15, xzr
    mov     x16, xzr
    mov     x17, xzr
    mov     x18, xzr
    mov     x19, xzr
    mov     x20, xzr
    mov     x21, xzr
    mov     x22, xzr
    mov     x23, xzr
    mov     x24, xzr
    mov     x25, xzr
    mov     x26, xzr
    mov     x27, xzr
    mov     x28, xzr
    mov     x29, xzr
    mov     x30, xzr
    eret
