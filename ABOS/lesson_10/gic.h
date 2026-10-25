/* =========================================================================
 * gic.h - ARM GICv2 Driver Using Higher-Half Periphmap Virtual Addresses
 * ========================================================================= */
#ifndef ABOS_GIC_H
#define ABOS_GIC_H

#include <stdint.h>
#include "mmu.h"

#define GICD_BASE (KERNEL_PERIPHMAP_OFFSET + 0x08000000ULL)
#define GICC_BASE (KERNEL_PERIPHMAP_OFFSET + 0x08010000ULL)

#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_TYPER      (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_ICENABLER  ((volatile uint32_t *)(GICD_BASE + 0x180))
#define GICD_IPRIORITYR ((volatile uint32_t *)(GICD_BASE + 0x400))
#define GICD_ITARGETSR  ((volatile uint32_t *)(GICD_BASE + 0x800))

#define GICC_CTLR       (*(volatile uint32_t *)(GICC_BASE + 0x0000))
#define GICC_PMR        (*(volatile uint32_t *)(GICC_BASE + 0x0004))
#define GICC_IAR        (*(volatile uint32_t *)(GICC_BASE + 0x000C))
#define GICC_EOIR       (*(volatile uint32_t *)(GICC_BASE + 0x0010))

void gic_dist_init(void);
void gic_cpu_init(void);
void gic_enable_interrupt(uint32_t int_id);
void gic_disable_interrupt(uint32_t int_id);
uint32_t gic_acknowledge_interrupt(void);
void gic_end_interrupt(uint32_t iar);

#endif /* ABOS_GIC_H */
