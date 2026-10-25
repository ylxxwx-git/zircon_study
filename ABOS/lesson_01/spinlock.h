/* =========================================================================
 * spinlock.h - Zircon-style AArch64 Spinlock
 * Reference: zircon_org/kernel/arch/arm64/spinlock.cpp
 * ========================================================================= */
#ifndef ABOS_SPINLOCK_H
#define ABOS_SPINLOCK_H

#include <stdint.h>

#define SMP_MAX_CPUS 4

typedef struct {
    volatile uint64_t value;
} spin_lock_t;

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

/*
 * Stores (cpu_num + 1) into lock->value so 0 means unlocked and non-zero
 * identifies the owning CPU. Uses WFE/LDAXR/STXR like Zircon's arch_spin_lock.
 */
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

static inline int spin_lock_holder_cpu(const spin_lock_t *lock) {
    uint64_t v = lock->value;
    return (v == 0) ? -1 : (int)(v - 1);
}

#endif /* ABOS_SPINLOCK_H */
