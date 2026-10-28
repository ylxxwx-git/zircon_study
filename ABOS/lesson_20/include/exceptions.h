/* =========================================================================
 * exceptions.h - Zircon-Style AArch64 Exception Frame (iframe_t)
 * References:
 *   - zircon_org/kernel/arch/arm64/include/arch/arm64.h
 *   - zircon_org/kernel/arch/arm64/include/arch/arm64/exceptions.h
 * ========================================================================= */
#ifndef ABOS_EXCEPTIONS_H
#define ABOS_EXCEPTIONS_H

#include <stdint.h>

#define ARM64_EXCEPTION_FLAG_LOWER_EL 0x1

/*
 * Matches Zircon's arm64_iframe_long (288 bytes = 18 * 16B aligned).
 * Pushed onto the current CPU/thread kernel stack on synchronous exceptions
 * and IRQs from either EL1 or EL0.
 */
typedef struct arm64_iframe {
    uint64_t r[30];     /* x0 .. x29 (offset 0..232) */
    uint64_t lr;        /* x30       (offset 240) */
    uint64_t usp;       /* sp_el0    (offset 248) */
    uint64_t elr;       /* elr_el1   (offset 256) */
    uint64_t spsr;      /* spsr_el1  (offset 264) */
    uint64_t mdscr;     /* mdscr_el1 (offset 272) */
    uint64_t pad;       /* 16-byte stack alignment padding (offset 280) */
} iframe_t;

extern void arm64_el1_exception_base(void);
void dump_iframe(const iframe_t *iframe);
void arm64_sync_exception(iframe_t *iframe, uint32_t exception_flags, uint32_t esr);
void arm64_irq_exception(iframe_t *iframe, uint32_t exception_flags);

#endif /* ABOS_EXCEPTIONS_H */
