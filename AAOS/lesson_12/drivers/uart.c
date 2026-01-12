#include <stdint.h>

// Define the base address of the PL011 UART for the QEMU 'virt' machine
// This is a fixed address QEMU maps to the virtual serial port.
// #define UART0_BASE 0x09000000
uint64_t uart_base = 0x09000000;

#include "uart.h"
#include "print.h"
#include "string.h"
#include "utils.h"

void uart_remap() {
    uart_base = 0xFFFFFFFFC9000000;
}

// Register offsets for the PL011 UART (32-bit access assumed)
// Data Register (send/receive)
#define UART0_DR     (*(volatile unsigned int *)((uint64_t)uart_base + 0x00))
#define UART0_FR     (*(volatile unsigned int *)((uint64_t)uart_base + 0x18))
#define UART0_IBRD   (*(volatile unsigned int *)((uint64_t)uart_base + 0x24))
#define UART0_FBRD   (*(volatile unsigned int *)((uint64_t)uart_base + 0x28))
#define UART0_LCR_H  (*(volatile unsigned int *)((uint64_t)uart_base + 0x2C))
#define UART0_CR     (*(volatile unsigned int *)((uint64_t)uart_base + 0x30))
#define UART0_IMSC   (*(volatile unsigned int *)((uint64_t)uart_base + 0x38))
#define UART0_ICR    (*(volatile unsigned int *)((uint64_t)uart_base + 0x44))

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
    // Read until empty
    while ((UART0_FR & UART_FR_RXFE) == 0) {
        char c = (char)(UART0_DR & 0xFF);
        
        // Add to buffer
        int next_head = (rx_head + 1) % RX_BUFFER_SIZE;
        if (next_head != rx_tail) {
            rx_buffer[rx_head] = c;
            rx_head = next_head;
        }
    }
    
    // Clear Interrupts
    UART0_ICR = 0x7FF; // Clear all
}

char uart_read_char(void) {
    // Blocking read (poll buffer)
    while (rx_head == rx_tail) {
        // Wait (no wfi for now to avoid race/hang issues)
    }
    
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUFFER_SIZE;
    return c;
}

/**
 * @brief Writes a single character to the serial port.
 * @param c The character to transmit.
 */
void uart_write_char(char c) {
    // Wait until the transmitter FIFO is NOT full.
    // We check the TXFF bit in the Flag Register (UART0_FR).
    int timeout = 1000000;
    while ((UART0_FR & UART_FR_TXFF) && timeout--) {
        // Spin lock: wait until ready
        asm volatile("nop");
    }
    // Write the character to the Data Register.
    UART0_DR = (unsigned int)c;
}

