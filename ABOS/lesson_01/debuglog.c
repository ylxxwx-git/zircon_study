/* =========================================================================
 * debuglog.c - Early Kernel Debug Console (PL011 UART + serialized dprintf)
 * Reference: zircon_org/kernel/lib/debuglog/ & platform/generic-arm/
 * ========================================================================= */
#include "debuglog.h"
#include "spinlock.h"
#include <stdarg.h>

/* QEMU 'virt' PL011 UART0 base physical address */
#define PL011_UART0_BASE 0x09000000UL
#define UART_DR          (*(volatile uint32_t *)(PL011_UART0_BASE + 0x00))
#define UART_FR          (*(volatile uint32_t *)(PL011_UART0_BASE + 0x18))
#define UART_FR_TXFF     (1U << 5)

static spin_lock_t dlog_lock = SPIN_LOCK_INITIAL_VALUE;

void dlog_init_early(void) {
    /* QEMU's emulated PL011 works out of reset; ensure lock is initialized */
    dlog_lock.value = 0;
}

void uart_putc(char c) {
    if (c == '\n') {
        while (UART_FR & UART_FR_TXFF) { }
        UART_DR = '\r';
    }
    while (UART_FR & UART_FR_TXFF) { }
    UART_DR = (uint32_t)c;
}

void uart_puts(const char *s) {
    while (*s) {
        uart_putc(*s++);
    }
}

static void print_uint(uint64_t val, int base, int width, char pad) {
    char buf[24];
    const char *digits = "0123456789abcdef";
    int i = 0;

    if (val == 0) {
        buf[i++] = '0';
    } else {
        while (val > 0) {
            buf[i++] = digits[val % (uint64_t)base];
            val /= (uint64_t)base;
        }
    }
    while (i < width) {
        buf[i++] = pad;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

static void print_int(int64_t val) {
    if (val < 0) {
        uart_putc('-');
        print_uint((uint64_t)(-val), 10, 0, ' ');
    } else {
        print_uint((uint64_t)val, 10, 0, ' ');
    }
}

void dprintf(int level, const char *fmt, ...) {
    (void)level;
    va_list args;
    va_start(args, fmt);

    arch_spin_lock(&dlog_lock);

    while (*fmt) {
        if (*fmt != '%') {
            uart_putc(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Optional 'l' modifier */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
            case 's': {
                const char *s = va_arg(args, const char *);
                uart_puts(s ? s : "(null)");
                break;
            }
            case 'c': {
                char c = (char)va_arg(args, int);
                uart_putc(c);
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = is_long ? va_arg(args, int64_t) : va_arg(args, int);
                print_int(v);
                break;
            }
            case 'u': {
                uint64_t v = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                print_uint(v, 10, 0, ' ');
                break;
            }
            case 'x': {
                uint64_t v = is_long ? va_arg(args, uint64_t) : va_arg(args, unsigned int);
                print_uint(v, 16, 0, ' ');
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)va_arg(args, void *);
                uart_puts("0x");
                print_uint(v, 16, 16, '0');
                break;
            }
            case '%':
                uart_putc('%');
                break;
            default:
                uart_putc('%');
                uart_putc(*fmt);
                break;
        }
        fmt++;
    }

    arch_spin_unlock(&dlog_lock);
    va_end(args);
}
