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
