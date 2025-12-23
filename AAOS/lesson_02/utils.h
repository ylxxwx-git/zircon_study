#ifndef UTILS_H
#define UTILS_H

void psci_cpu_on(unsigned long core_id, unsigned long entry_point);
int get_core_id();
int get_el();

static inline unsigned long local_irq_save() {
    unsigned long flags;
    asm volatile(
        "mrs %0, daif\n"
        "msr daifset, #2" // Disable IRQ (bit 1 of DAIF)
        : "=r" (flags)
        :
        : "memory"
    );
    return flags;
}

static inline void local_irq_restore(unsigned long flags) {
    asm volatile(
        "msr daif, %0"
        :
        : "r" (flags)
        : "memory"
    );
}

void switch_to_el0(void (*entry_point)(), void *stack_top, unsigned long arg0, void *vectors);

#endif
