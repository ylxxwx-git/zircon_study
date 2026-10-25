/* =========================================================================
 * spinlock.h - Zircon-style AArch64 Spinlock & Interrupt Save/Restore
 * References:
 *   - zircon_org/kernel/arch/arm64/spinlock.cpp
 *   - zircon_org/kernel/arch/arm64/include/arch/arch_ops.h
 * ========================================================================= */
#ifndef ABOS_SPINLOCK_H
#define ABOS_SPINLOCK_H

#include <stdint.h>

#define SMP_MAX_CPUS 4

typedef struct {
    volatile uint64_t value;
} spin_lock_t;

typedef uint64_t spin_lock_saved_state_t;

#define SPIN_LOCK_INITIAL_VALUE { 0 }

static inline uint32_t arch_curr_cpu_num(void) {
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (uint32_t)(mpidr & 0xFF);
}

static inline uint32_t arch_boot_get_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (uint32_t)((el >> 2) & 0x3);
}

static inline void arch_enable_ints(void) {
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void arch_disable_ints(void) {
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

static inline void arch_interrupt_save(spin_lock_saved_state_t *statep) {
    uint64_t state;
    __asm__ volatile("mrs %0, daif" : "=r"(state));
    __asm__ volatile("msr daifset, #2" ::: "memory");
    *statep = state;
}

static inline void arch_interrupt_restore(spin_lock_saved_state_t old_state) {
    __asm__ volatile("msr daif, %0" :: "r"(old_state) : "memory");
}

static inline void arch_spin_lock(spin_lock_t *lock) {
    uint64_t val = (uint64_t)arch_curr_cpu_num() + 1;
    uint64_t temp;

    __asm__ volatile(
        "sevl\n"
        "1: wfe\n"
        "ldaxr %[temp], [%[lock]]\n"
        "cbnz  %[temp], 1b\n"
        "stxr  %w[temp], %[val], [%[lock]]\n"
        "cbnz  %w[temp], 1b\n"
        : [temp] "=&r"(temp)
        : [lock] "r"(&lock->value), [val] "r"(val)
        : "cc", "memory");
}

static inline void arch_spin_unlock(spin_lock_t *lock) {
    __asm__ volatile("stlr xzr, [%0]" :: "r"(&lock->value) : "memory");
}

static inline void spin_lock_irqsave(spin_lock_t *lock, spin_lock_saved_state_t *statep) {
    arch_interrupt_save(statep);
    arch_spin_lock(lock);
}

static inline void spin_lock_irqrestore(spin_lock_t *lock, spin_lock_saved_state_t old_state) {
    arch_spin_unlock(lock);
    arch_interrupt_restore(old_state);
}

#endif /* ABOS_SPINLOCK_H */
