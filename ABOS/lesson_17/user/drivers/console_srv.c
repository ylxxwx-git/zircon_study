/* =========================================================================
 * drivers/console_srv.c - EL0 User-Space Console Driver ('console-srv')
 *
 * Maps PL011 UART0 MMIO (0x09000000) into EL0 via sys_vmo_create_physical()
 * and binds GIC SPI 33 via sys_interrupt_create().
 * ========================================================================= */
#include "ulibc.h"

#define PL011_PHYS_BASE   0x09000000ULL
#define PL011_IRQ_NUM     33U
#define UART_DR           0x000
#define UART_FR           0x018
#define UART_IMSC         0x038
#define UART_ICR          0x044
#define UART_FR_RXFE      (1U << 4)
#define UART_FR_TXFF      (1U << 5)
#define UART_IMSC_RXIM    (1U << 4)

static inline uint32_t mmio_r32(vaddr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_w32(vaddr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

static void el0_uart_putc(vaddr_t uart_va, char c) {
    if (c == '\n') {
        while (mmio_r32(uart_va, UART_FR) & UART_FR_TXFF) { }
        mmio_w32(uart_va, UART_DR, '\r');
    }
    while (mmio_r32(uart_va, UART_FR) & UART_FR_TXFF) { }
    mmio_w32(uart_va, UART_DR, (uint32_t)(unsigned char)c);
}

static void el0_uart_puts(vaddr_t uart_va, const char *s) {
    while (*s) el0_uart_putc(uart_va, *s++);
}

static char el0_uart_getc(vaddr_t uart_va, ab_handle_t uart_irq_h) {
    while (mmio_r32(uart_va, UART_FR) & UART_FR_RXFE) {
        sys_interrupt_wait(uart_irq_h);
        mmio_w32(uart_va, UART_ICR, UART_IMSC_RXIM);
        sys_interrupt_ack(uart_irq_h);
    }
    return (char)(mmio_r32(uart_va, UART_DR) & 0xFFU);
}

void console_server_entry(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[4];
    sys_channel_read(boot_ch, &tag, sizeof(tag), h, 4, &act_b, &act_h);
    sys_handle_close(boot_ch);

    ab_handle_t log_h    = h[0];
    ab_handle_t vmar_h   = h[1];
    ab_handle_t res_h    = h[2];
    ab_handle_t srv_ch_h = h[3];

    /* Map PL011 MMIO physical page (0x09000000) into EL0 and bind IRQ 33 */
    ab_handle_t uart_vmo_h = 0, uart_irq_h = 0;
    sys_vmo_create_physical(res_h, PL011_PHYS_BASE, PAGE_SIZE, &uart_vmo_h);
    sys_object_set_name(uart_vmo_h, "pl011-uart-mmio");

    vaddr_t uart_va = 0;
    sys_vmar_map(vmar_h, 0, uart_vmo_h, 0, PAGE_SIZE,
                AB_VM_PERM_READ | AB_VM_PERM_WRITE, &uart_va);

    sys_interrupt_create(res_h, PL011_IRQ_NUM, &uart_irq_h);
    mmio_w32(uart_va, UART_ICR, 0x7FFU);
    mmio_w32(uart_va, UART_IMSC, UART_IMSC_RXIM);

    ulog(log_h, "[console-srv EL0] Mapped PL011 MMIO (0x09000000) & bound IRQ 33! Ready.");

    for (;;) {
        ab_signals_t obs = 0;
        sys_object_wait_one(srv_ch_h, AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED,
                           AB_TIME_INFINITE, &obs);
        if (obs & AB_SIGNAL_PEER_CLOSED) break;

        console_msg_t req;
        if (sys_channel_read(srv_ch_h, &req, sizeof(req), NULL, 0, &act_b, &act_h) != AB_OK) {
            continue;
        }

        if (req.op == CON_OP_WRITE) {
            el0_uart_puts(uart_va, req.text);
            console_msg_t ack = { CON_OP_WRITE, "" };
            sys_channel_write(srv_ch_h, &ack, sizeof(uint32_t), NULL, 0);
        } else if (req.op == CON_OP_READ_LINE) {
            console_msg_t rep;
            rep.op = CON_OP_READ_LINE;
            size_t pos = 0;
            while (pos + 1 < sizeof(rep.text)) {
                char c = el0_uart_getc(uart_va, uart_irq_h);
                if (c == '\r' || c == '\n') {
                    el0_uart_putc(uart_va, '\n');
                    break;
                } else if (c == '\b' || c == 127) {
                    if (pos > 0) {
                        pos--;
                        el0_uart_puts(uart_va, "\b \b");
                    }
                } else if (c >= 32 && c < 127) {
                    rep.text[pos++] = c;
                    el0_uart_putc(uart_va, c);
                }
            }
            rep.text[pos] = '\0';
            sys_channel_write(srv_ch_h, &rep, sizeof(rep), NULL, 0);
        } else if (req.op == CON_OP_SHUTDOWN) {
            break;
        }
    }

    sys_process_exit(0);
}
