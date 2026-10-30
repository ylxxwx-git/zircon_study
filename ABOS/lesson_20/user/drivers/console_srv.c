/* =========================================================================
 * drivers/console_srv.c - EL0 Multi-Terminal Console Server ('console-srv')
 *
 * Lesson 20: Standalone BOOTFS system server linked at VA 0x20000000.
 * Multiplexes 4 Virtual Terminals (tty0..tty3) and PL011 UART RX IRQ 33
 * using a Zircon PortDispatcher (sys_port_create + sys_object_wait_async).
 *
 * Supports switching between terminals 0..3 at any time via:
 *   - Ctrl+0 / Ctrl+1 / Ctrl+2 / Ctrl+3 (CSI modifyOtherKeys / kitty / NUL)
 *   - Alt+0 / Alt+1 / Alt+2 / Alt+3 (ESC + 0..3)
 *   - Ctrl+T followed by 0..3, or ^0 / ^1 / ^2 / ^3
 *   - Shell command: 'vt 0' .. 'vt 3'
 * ========================================================================= */
#include "ulibc.h"

#define PL011_PHYS_BASE   0x09000000ULL
#define PL011_IRQ_NUM     33U
#define UART_DR           0x000
#define UART_FR           0x018
#define UART_LCR_H        0x02C
#define UART_IMSC         0x038
#define UART_ICR          0x044
#define UART_FR_RXFE      (1U << 4)
#define UART_FR_TXFF      (1U << 5)
#define UART_LCR_H_FEN    (1U << 4)
#define UART_LCR_H_WLEN8  (3U << 5)
#define UART_IMSC_RXIM    (1U << 4)
#define UART_IMSC_RTIM    (1U << 6)

#define KEY_UART_IRQ      100ULL
#define VT_SCREEN_CAP     1024U
#define VT_LINE_CAP       128U
#define VT_RX_RING_CAP    512U
#define ESC_TIMEOUT_NS    25000000ULL /* 25ms standalone ESC timeout */

typedef struct {
    ab_handle_t ch;
    int         waiting_read;
    int         last_was_cr;
    char        rx_ring[VT_RX_RING_CAP];
    uint16_t    rx_head;
    uint16_t    rx_tail;
    uint16_t    rx_count;
    char        line_buf[VT_LINE_CAP];
    size_t      line_len;
    char        screen_buf[VT_SCREEN_CAP];
    size_t      screen_len;
} vt_state_t;

typedef struct {
    int       prefix_mode; /* 0=normal, 1=after Ctrl+T/caret, 2=after ESC, 3=in CSI */
    ab_time_t esc_deadline;
    char      csi_buf[16];
    size_t    csi_len;
} rx_parser_t;

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

static void vt_append_screen(vt_state_t *vt, const char *text) {
    while (*text) {
        char c = *text++;
        if (vt->screen_len + 1 >= VT_SCREEN_CAP) {
            size_t drop = VT_SCREEN_CAP / 4;
            for (size_t i = drop; i < vt->screen_len; i++) {
                vt->screen_buf[i - drop] = vt->screen_buf[i];
            }
            vt->screen_len -= drop;
        }
        vt->screen_buf[vt->screen_len++] = c;
    }
    vt->screen_buf[vt->screen_len] = '\0';
}

static void vt_push_rx(vt_state_t *vt, char c) {
    if (vt->rx_count < VT_RX_RING_CAP) {
        vt->rx_ring[vt->rx_tail] = c;
        vt->rx_tail = (uint16_t)((vt->rx_tail + 1U) % VT_RX_RING_CAP);
        vt->rx_count++;
    }
}

static int vt_pop_rx(vt_state_t *vt, char *out_c) {
    if (vt->rx_count == 0) return 0;
    *out_c = vt->rx_ring[vt->rx_head];
    vt->rx_head = (uint16_t)((vt->rx_head + 1U) % VT_RX_RING_CAP);
    vt->rx_count--;
    return 1;
}

static void deliver_line_to_vt(vt_state_t *vt) {
    console_msg_t rep;
    rep.op = CON_OP_READ_LINE;
    vt->line_buf[vt->line_len] = '\0';
    u_strcpy(rep.text, vt->line_buf);
    vt->line_len     = 0;
    vt->line_buf[0]  = '\0';
    vt->waiting_read = 0;
    sys_channel_write(vt->ch, &rep, sizeof(rep), NULL, 0);
}

static void process_vt_rx(vaddr_t uart_va, vt_state_t *vt, int is_active) {
    while (vt->waiting_read && vt->rx_count > 0) {
        char c = 0;
        vt_pop_rx(vt, &c);

        if (c == '\n' && vt->last_was_cr) {
            vt->last_was_cr = 0;
            continue;
        }
        vt->last_was_cr = (c == '\r');

        if (c == '\r' || c == '\n') {
            if (is_active) el0_uart_putc(uart_va, '\n');
            vt->line_buf[vt->line_len] = '\0';
            vt_append_screen(vt, vt->line_buf);
            vt_append_screen(vt, "\n");
            deliver_line_to_vt(vt);
            break;
        } else if (c == '\b' || c == 127) {
            if (vt->line_len > 0) {
                vt->line_len--;
                if (is_active) el0_uart_puts(uart_va, "\b \b");
            }
        } else if (c >= 32 && c < 127) {
            if (vt->line_len + 1 < VT_LINE_CAP) {
                vt->line_buf[vt->line_len++] = c;
                if (is_active) el0_uart_putc(uart_va, c);
            }
        }
    }
}

static void switch_active_vt(vaddr_t uart_va, vt_state_t *vts,
                             uint32_t *active_vt, uint32_t target_vt) {
    if (target_vt >= NUM_VTS) return;
    *active_vt = target_vt;

    char hdr[256];
    u_snprintf(hdr, sizeof(hdr),
               "\n============================================================\n"
               " >>> Switched to Virtual Terminal [tty%u] (abos-shell-%u) <<<\n"
               " [Switch: Alt+0..3 | Ctrl+2..5 | Ctrl+N (next) | vt 0..3]\n"
               "============================================================\n",
               target_vt, target_vt);
    el0_uart_puts(uart_va, hdr);

    if (vts[target_vt].screen_len > 0) {
        el0_uart_puts(uart_va, vts[target_vt].screen_buf);
    }
    if (vts[target_vt].line_len > 0) {
        vts[target_vt].line_buf[vts[target_vt].line_len] = '\0';
        el0_uart_puts(uart_va, vts[target_vt].line_buf);
    }
    process_vt_rx(uart_va, &vts[target_vt], 1);
}

/* Parse CSI sequences for Ctrl+0..Ctrl+4:
 *   - "48;5u" .. "52;5u" (xterm modifyOtherKeys / kitty)
 *   - "27;5;48~" .. "27;5;52~" (xterm modifyOtherKeys format 0)
 */
static int parse_csi_vt_hotkey(const char *csi, size_t len) {
    if (len < 4) return -1;
    char final_ch = csi[len - 1];
    int keycode = -1;

    if (final_ch == 'u') {
        int val = 0;
        size_t i = 0;
        while (i < len && csi[i] >= '0' && csi[i] <= '9') {
            val = val * 10 + (csi[i] - '0');
            i++;
        }
        if (i > 0 && csi[i] == ';') keycode = val;
    } else if (final_ch == '~' && len >= 7 &&
               csi[0] == '2' && csi[1] == '7' && csi[2] == ';') {
        size_t i = 3;
        while (i < len && csi[i] != ';') i++;
        if (i < len && csi[i] == ';') {
            i++;
            int val = 0;
            size_t digits = 0;
            while (i < len && csi[i] >= '0' && csi[i] <= '9') {
                val = val * 10 + (csi[i] - '0');
                i++;
                digits++;
            }
            if (digits > 0 && csi[i] == '~') keycode = val;
        }
    }

    if (keycode >= 48 && keycode <= 51) return keycode - 48; /* Ctrl+0..3 -> tty0..3 */
    if (keycode == 52) return 0;                             /* Ctrl+4    -> tty0    */
    return -1;
}

static void drain_uart_rx(vaddr_t uart_va, vt_state_t *vts,
                          uint32_t *active_vt, rx_parser_t *rx) {
    /* Clear ICR before draining so any byte arriving after RXFE==1 re-asserts IRQ 33 */
    while (!(mmio_r32(uart_va, UART_FR) & UART_FR_RXFE)) {
        mmio_w32(uart_va, UART_ICR, UART_IMSC_RXIM | UART_IMSC_RTIM);
        while (!(mmio_r32(uart_va, UART_FR) & UART_FR_RXFE)) {
            char c = (char)(mmio_r32(uart_va, UART_DR) & 0xFFU);

        if (rx->prefix_mode == 1) {
            rx->prefix_mode = 0;
            if (c >= '0' && c <= '3') {
                switch_active_vt(uart_va, vts, active_vt, (uint32_t)(c - '0'));
                continue;
            }
        } else if (rx->prefix_mode == 2) {
            if (c == '[') {
                rx->prefix_mode = 3;
                rx->csi_len = 0;
                continue;
            }
            rx->prefix_mode = 0;
            if (c >= '0' && c <= '3') {
                /* Alt+0 .. Alt+3 (ESC followed by 0..3) */
                switch_active_vt(uart_va, vts, active_vt, (uint32_t)(c - '0'));
                continue;
            }
        } else if (rx->prefix_mode == 3) {
            if (rx->csi_len + 1 < sizeof(rx->csi_buf)) {
                rx->csi_buf[rx->csi_len++] = c;
                rx->csi_buf[rx->csi_len]   = '\0';
            }
            if (c >= 0x40 && c <= 0x7E) {
                rx->prefix_mode = 0;
                int target = parse_csi_vt_hotkey(rx->csi_buf, rx->csi_len);
                if (target >= 0) {
                    switch_active_vt(uart_va, vts, active_vt, (uint32_t)target);
                }
            }
            continue;
        }

        /* Hotkey triggers over serial/VTE */
        if (c == 0x00) {
            switch_active_vt(uart_va, vts, active_vt, 2U);
            continue;
        } else if (c == 0x1C) {
            switch_active_vt(uart_va, vts, active_vt, 0U);
            continue;
        } else if (c == 0x1D) {
            switch_active_vt(uart_va, vts, active_vt, 1U);
            continue;
        } else if (c == 0x0E || c == 0x1E) {
            switch_active_vt(uart_va, vts, active_vt, (*active_vt + 1U) % NUM_VTS);
            continue;
        } else if (c == 0x14 || c == '^') {
            rx->prefix_mode = 1;
            continue;
        } else if (c == 0x1B) {
            rx->prefix_mode  = 2;
            rx->esc_deadline = ulibc_clock_get_monotonic() + ESC_TIMEOUT_NS;
            continue;
        }

        vt_push_rx(&vts[*active_vt], c);
        process_vt_rx(uart_va, &vts[*active_vt], 1);
        }
    }
}

int main(void) {
    uint32_t act_b = 0, act_h = 0;

    ab_handle_t log_h  = ulibc_get_startup_handle(0);
    ab_handle_t vmar_h = ulibc_get_startup_handle(1);
    ab_handle_t res_h  = ulibc_get_startup_handle(2);

    vt_state_t vts[NUM_VTS];
    memset(vts, 0, sizeof(vts));
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        vts[i].ch = ulibc_get_startup_handle(3 + i);
    }

    /* Map PL011 MMIO physical page (0x09000000) into EL0 and bind IRQ 33 */
    ab_handle_t uart_vmo_h = 0, uart_irq_h = 0;
    sys_vmo_create_physical(res_h, PL011_PHYS_BASE, PAGE_SIZE, &uart_vmo_h);
    sys_object_set_name(uart_vmo_h, "pl011-uart-mmio");

    vaddr_t uart_va = 0;
    sys_vmar_map(vmar_h, 0, uart_vmo_h, 0, PAGE_SIZE,
                 AB_VM_PERM_READ | AB_VM_PERM_WRITE, &uart_va);

    sys_interrupt_create(res_h, PL011_IRQ_NUM, &uart_irq_h);
    mmio_w32(uart_va, UART_LCR_H, UART_LCR_H_WLEN8 | UART_LCR_H_FEN);
    mmio_w32(uart_va, UART_ICR, 0x7FFU);
    mmio_w32(uart_va, UART_IMSC, UART_IMSC_RXIM | UART_IMSC_RTIM);

    /* Enable xterm modifyOtherKeys (mode 2) + kitty keyboard protocol */
    el0_uart_puts(uart_va, "\x1b[>4;2m\x1b[>1u");

    ulog(log_h, "[console-srv EL0] Multiplexing 4 Virtual Terminals (tty0..tty3) + UART IRQ 33 on Port!");

    ab_handle_t port_h = AB_HANDLE_INVALID;
    sys_port_create(0, &port_h);

    sys_object_wait_async(uart_irq_h, port_h, KEY_UART_IRQ, AB_SIGNAL_SIGNALED);
    for (uint32_t i = 0; i < NUM_VTS; i++) {
        sys_object_wait_async(vts[i].ch, port_h, (uint64_t)i,
                              AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
    }

    uint32_t active_vt = 0;
    rx_parser_t rx;
    memset(&rx, 0, sizeof(rx));
    int running = 1;

    while (running) {
        ab_port_packet_t pkt;
        ab_time_t deadline = (rx.prefix_mode == 2) ? rx.esc_deadline : AB_TIME_INFINITE;
        ab_status_t wst = sys_port_wait(port_h, deadline, &pkt);
        if (wst == AB_ERR_TIMED_OUT) {
            if (rx.prefix_mode == 2) {
                /* Standalone 0x1B (Ctrl+3 / ESC) -> switch to tty3 */
                rx.prefix_mode = 0;
                switch_active_vt(uart_va, vts, &active_vt, 3U);
            }
            continue;
        }
        if (wst != AB_OK) {
            continue;
        }

        if (pkt.key == KEY_UART_IRQ) {
            drain_uart_rx(uart_va, vts, &active_vt, &rx);
            sys_interrupt_ack(uart_irq_h);
            sys_object_wait_async(uart_irq_h, port_h, KEY_UART_IRQ, AB_SIGNAL_SIGNALED);
            continue;
        }

        uint32_t vt_idx = (uint32_t)pkt.key;
        if (vt_idx >= NUM_VTS || vts[vt_idx].ch == AB_HANDLE_INVALID) {
            continue;
        }

        vt_state_t *vt = &vts[vt_idx];
        console_msg_t req;
        while (sys_channel_read(vt->ch, &req, sizeof(req), NULL, 0, &act_b, &act_h) == AB_OK) {
            if (req.op == CON_OP_WRITE) {
                vt_append_screen(vt, req.text);
                if (vt_idx == active_vt) {
                    el0_uart_puts(uart_va, req.text);
                }
                console_msg_t ack = { CON_OP_WRITE, "" };
                sys_channel_write(vt->ch, &ack, sizeof(uint32_t), NULL, 0);
            } else if (req.op == CON_OP_READ_LINE) {
                vt->waiting_read = 1;
                if (vt_idx == active_vt) {
                    drain_uart_rx(uart_va, vts, &active_vt, &rx);
                }
                process_vt_rx(uart_va, vt, vt_idx == active_vt);
            } else if (req.op == CON_OP_SWITCH_VT) {
                uint32_t target = (uint32_t)(req.text[0] - '0');
                console_msg_t ack = { CON_OP_SWITCH_VT, "" };
                sys_channel_write(vt->ch, &ack, sizeof(uint32_t), NULL, 0);
                if (target < NUM_VTS) {
                    switch_active_vt(uart_va, vts, &active_vt, target);
                    drain_uart_rx(uart_va, vts, &active_vt, &rx);
                }
            } else if (req.op == CON_OP_SHUTDOWN) {
                running = 0;
                break;
            }
        }

        if (!running) break;

        if (pkt.observed & AB_SIGNAL_PEER_CLOSED) {
            sys_handle_close(vt->ch);
            vt->ch = AB_HANDLE_INVALID;
        } else {
            sys_object_wait_async(vt->ch, port_h, (uint64_t)vt_idx,
                                  AB_SIGNAL_READABLE | AB_SIGNAL_PEER_CLOSED);
        }
    }

    el0_uart_puts(uart_va, "\x1b[<u\x1b[>4;0m");
    return 0;
}
