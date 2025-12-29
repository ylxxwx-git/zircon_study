#include "output.h"
#include "mutex.h"
#include "utils.h"

// Define the base address of the PL011 UART for the QEMU 'virt' machine
// This is a fixed address QEMU maps to the virtual serial port.
#define UART0_BASE 0x09000000

// Register offsets for the PL011 UART (32-bit access assumed)

// Data Register (send/receive)
#define UART0_DR (*(volatile unsigned int *)(UART0_BASE + 0x00))

// Flag Register (status checks)
#define UART0_FR (*(volatile unsigned int *)(UART0_BASE + 0x18))

// Flag Register constant: Transmitter FIFO Full (UART can't accept more data yet)
#define UART_FR_TXFF (1 << 5)
#define UART_FR_RXFE (1 << 4)

static mutex_t uart_mutex;

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
    // unsigned long flags = local_irq_save();
    // mutex_lock(&uart_mutex);
    while (*s != '\0') {
        putc(*s++);
    }
    // mutex_unlock(&uart_mutex);
    // local_irq_restore(flags);
}

/**
 * @brief Writes a 64-bit integer as a hexadecimal string.
 * @param n The integer to print.
 */
void puthex(unsigned long n) {
    // unsigned long flags = local_irq_save();
    // mutex_lock(&uart_mutex);
    // We can't call puts here because it locks again (non-recursive mutex)
    // So we implement the printing manually or use a helper
    const char *prefix = "0x";
    while (*prefix) putc(*prefix++);
    
    for (int i = 60; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        putc(d > 9 ? 'A' + d - 10 : '0' + d);
    }
    // mutex_unlock(&uart_mutex);
    // local_irq_restore(flags);
}

char getc() {
    while (UART0_FR & UART_FR_RXFE) {
        // Spin lock: wait until ready
    }
    return UART0_DR;
}


