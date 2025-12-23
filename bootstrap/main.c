// Define the base address of the PL011 UART for the QEMU 'virt' machine
// This is where QEMU expects serial data to be written
#define UART0_BASE 0x9000000

// Register offsets for the PL011 UART
// Data Register (send/receive) - R/W
#define UART0_DR (*(volatile unsigned int *)(UART0_BASE + 0x00))

// Flag Register (status checks) - R/O
#define UART0_FR (*(volatile unsigned int *)(UART0_BASE + 0x18))

// Constant to check if the UART is busy (transmitter FIFO full)
#define UART_FR_TXFF (1 << 5)

/**
 * @brief Sends a single character to the UART.
 * @param c The character to transmit.
 */
void putc(char c) {
    // Wait until the transmitter FIFO is not full (UART_FR_TXFF is 0)
    while (UART0_FR & UART_FR_TXFF) {
        // Spin lock
    }
    // Write the character to the Data Register
    UART0_DR = (unsigned int)c;
}

/**
 * @brief Sends a null-terminated string to the UART.
 * @param s The string to transmit.
 */
void puts(const char *s) {
    while (*s != '\0') {
        putc(*s++);
    }
}

/**
 * @brief The entry point for the C code.
 */
void c_entry() {
    // Print the message
    puts("Hello World from a bare-metal ARM kernel!\n");

    // Loop indefinitely to keep the kernel running
    while (1) {
        // Do nothing
    }
}