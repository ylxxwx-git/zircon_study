// Define the base address of the PL011 UART for the QEMU 'virt' machine
// This is a fixed address QEMU maps to the virtual serial port.
#define UART0_BASE 0x09000000
#include "output.h"

// Register offsets for the PL011 UART (32-bit access assumed)

// Data Register (send/receive)
#define UART0_DR (*(volatile unsigned int *)(UART0_BASE + 0x00))

// Flag Register (status checks)
#define UART0_FR (*(volatile unsigned int *)(UART0_BASE + 0x18))

// Flag Register constant: Transmitter FIFO Full (UART can't accept more data yet)
#define UART_FR_TXFF (1 << 5)

void init_uart(void) {
    // On QEMU, UART is already initialized.
    // We could set baud rate here if on real hardware.
}


/**
 * @brief Writes a single character to the serial port.
 * @param c The character to transmit.
 */
void putc(char c) {
    // Wait until the transmitter FIFO is NOT full.
    // We check the TXFF bit in the Flag Register (UART0_FR).
    while (UART0_FR & UART_FR_TXFF) {
        // Spin lock: wait until ready
    }
    // Write the character to the Data Register.
    UART0_DR = (unsigned int)c;
}

/**
 * @brief Writes a null-terminated string to the serial port.
 * @param s The string to transmit.
 */
void puts(const char *s) {
    while (*s != '\0') {
        putc(*s++);
    }
}

/**
 * @brief Writes a 64-bit integer as a hexadecimal string.
 * @param n The integer to print.
 */
void puthex(unsigned long n) {
    puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        putc(d > 9 ? 'A' + d - 10 : '0' + d);
    }
}

char getc() {
    while (UART0_FR & (1 << 4)) {
        // Wait until the receiver FIFO is NOT empty.
    }
    return UART0_DR;
}

char *gets(char *s) {
    char *original = s;
    char c;
    while (1) {
        c = getc();
        if (c == '\r' || c == '\n') {
            putc('\n'); // Echo newline
            break;
        }
        putc(c); // Echo character
        *s++ = c;
    }
    *s = '\0';
    return original;
}

void memset(void *s, int c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ((char *)s)[i] = c;
    }
}

int strncmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i]) {
            return s1[i] - s2[i];
        }
    }
    return 0;
}

#include <stdarg.h>

extern void lock_uart(void);
extern void unlock_uart(void);

static void buffer_append(char *buf, int *idx, char c) {
    if (*idx < 127) {
        buf[(*idx)++] = c;
    }
}

static void buffer_append_str(char *buf, int *idx, const char *s) {
    while (*s) {
        buffer_append(buf, idx, *s++);
    }
}

static void buffer_append_dec(char *buf, int *idx, long n) {
    if (n < 0) {
        buffer_append(buf, idx, '-');
        n = -n;
    }
    
    if (n == 0) {
        buffer_append(buf, idx, '0');
        return;
    }
    
    char tmp[32];
    int i = 0;
    while (n > 0) {
        tmp[i++] = (n % 10) + '0';
        n /= 10;
    }
    
    while (i > 0) {
        buffer_append(buf, idx, tmp[--i]);
    }
}

static void buffer_append_hex(char *buf, int *idx, unsigned long n) {
    buffer_append_str(buf, idx, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        buffer_append(buf, idx, d > 9 ? 'A' + d - 10 : '0' + d);
    }
}

void printf(const char *fmt, ...) {
    char buf[128];
    int idx = 0;
    
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'c') {
                int c = va_arg(args, int);
                buffer_append(buf, &idx, (char)c);
            } else if (*fmt == 's') {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                buffer_append_str(buf, &idx, s);
            } else if (*fmt == 'd') {
                long n = va_arg(args, long);
                buffer_append_dec(buf, &idx, n);
            } else if (*fmt == 'x') {
                unsigned long n = va_arg(args, unsigned long);
                buffer_append_hex(buf, &idx, n);
            } else if (*fmt == 'p') {
                unsigned long n = va_arg(args, unsigned long);
                buffer_append_hex(buf, &idx, n);
            } else if (*fmt == '%') {
                buffer_append(buf, &idx, '%');
            } else {
                buffer_append(buf, &idx, '%');
                buffer_append(buf, &idx, *fmt);
            }
        } else {
            buffer_append(buf, &idx, *fmt);
        }
        fmt++;
    }
    
    va_end(args);
    
    buf[idx] = '\0';
    lock_uart();
    puts(buf);
    unlock_uart();
}

