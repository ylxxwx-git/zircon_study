#ifndef GIC_H
#define GIC_H

#include <stdint.h>

// GICv2 Base Addresses for QEMU 'virt'
#define GICD_BASE       0x08000000
#define GICC_BASE       0x08010000

// Distributor Registers
#define GICD_CTLR       (*((volatile uint32_t *)(GICD_BASE + 0x000)))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_ITARGETSR  ((volatile uint32_t *)(GICD_BASE + 0x800))
#define GICD_ICFGR      ((volatile uint32_t *)(GICD_BASE + 0xC00))

// CPU Interface Registers
#define GICC_CTLR       (*((volatile uint32_t *)(GICC_BASE + 0x000)))
#define GICC_PMR        (*((volatile uint32_t *)(GICC_BASE + 0x004)))
#define GICC_IAR        (*((volatile uint32_t *)(GICC_BASE + 0x00C)))
#define GICC_EOIR       (*((volatile uint32_t *)(GICC_BASE + 0x010)))

void gic_dist_init(void);
void gic_cpu_init(void);
void gic_enable_interrupt(uint32_t int_id);
uint32_t gic_acknowledge_interrupt(void);
void gic_end_interrupt(uint32_t int_id);

#endif
