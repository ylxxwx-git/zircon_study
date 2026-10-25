/* =========================================================================
 * gic.h - ARM Generic Interrupt Controller v2 (GICv2) Driver
 * References:
 *   - zircon_org/kernel/dev/interrupt/arm_gic/v2/arm_gicv2m.cpp
 *   - AAOS/lesson_03b/gic.h
 * ========================================================================= */
#ifndef ABOS_GIC_H
#define ABOS_GIC_H

#include <stdint.h>

/* QEMU 'virt' GICv2 physical addresses */
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL

/* Distributor Registers */
#define GICD_CTLR       (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_TYPER      (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_ISENABLER  ((volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_ICENABLER  ((volatile uint32_t *)(GICD_BASE + 0x180))
#define GICD_IPRIORITYR ((volatile uint32_t *)(GICD_BASE + 0x400))
#define GICD_ITARGETSR  ((volatile uint32_t *)(GICD_BASE + 0x800))

/* CPU Interface Registers */
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
