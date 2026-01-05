// Define the base address of the PL011 UART for the QEMU 'virt' machine
// This is a fixed address QEMU maps to the virtual serial port.
#define UART0_BASE 0x09000000
#include "output.h"

// Register offsets for the PL011 UART (32-bit access assumed)
// Data Register (send/receive)
#define UART0_DR     (*(volatile unsigned int *)(UART0_BASE + 0x00))
#define UART0_FR     (*(volatile unsigned int *)(UART0_BASE + 0x18))
#define UART0_IBRD   (*(volatile unsigned int *)(UART0_BASE + 0x24))
#define UART0_FBRD   (*(volatile unsigned int *)(UART0_BASE + 0x28))
#define UART0_LCR_H  (*(volatile unsigned int *)(UART0_BASE + 0x2C))
#define UART0_CR     (*(volatile unsigned int *)(UART0_BASE + 0x30))
#define UART0_IMSC   (*(volatile unsigned int *)(UART0_BASE + 0x38))
#define UART0_ICR    (*(volatile unsigned int *)(UART0_BASE + 0x44))

// Flag Register constant: Transmitter FIFO Full
#define UART_FR_TXFF (1 << 5)
// Flag Register constant: Receiver FIFO Empty
#define UART_FR_RXFE (1 << 4)

// Ring Buffer
#define RX_BUFFER_SIZE 128
static char rx_buffer[RX_BUFFER_SIZE];
static volatile int rx_head = 0;
static volatile int rx_tail = 0;

void init_uart(void) {
    // Disable UART
    UART0_CR = 0;
    
    // Clear all interrupts
    UART0_ICR = 0x7FF;
    
    // Setup for 115200 baud (assuming 24MHz clock)
    // IBRD = 13, FBRD = 1
    UART0_IBRD = 13;
    UART0_FBRD = 1;
    
    // 8 bit word length, Enable FIFO
    UART0_LCR_H = (1 << 4) | (1 << 5) | (1 << 6);
    
    // Enable RX Interrupt (Bit 4) and RX Timeout Interrupt (Bit 6)
    UART0_IMSC = (1 << 4) | (1 << 6);
    
    // Enable UART, TX, RX
    UART0_CR = (1 << 0) | (1 << 8) | (1 << 9);
}





void uart_handler(void) {
    // Check if it's RX Interrupt (Masked Status would be better, but we only enabled RX)
    unsafe_puts("UART IRQ\n");
    // Read until empty
    while ((UART0_FR & UART_FR_RXFE) == 0) {
        char c = (char)(UART0_DR & 0xFF);
        
        // Add to buffer
        int next_head = (rx_head + 1) % RX_BUFFER_SIZE;
        if (next_head != rx_tail) {
            rx_buffer[rx_head] = c;
            rx_head = next_head;
        }
        
        // Echo back for now (so we can see what we type)
        putc(c);
    }
    
    // Clear Interrupts
    UART0_ICR = 0x7FF; // Clear all
}

char uart_read(void) {
    // Blocking read
    while (rx_head == rx_tail) {
        // Wait for interrupt
        asm volatile("wfi");
    }
    
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUFFER_SIZE;
    return c;
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
