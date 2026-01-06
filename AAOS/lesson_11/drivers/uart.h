#ifndef UART_H
#define UART_H

#include <stdint.h>

void init_uart(void);
void uart_write_char(char c);
char uart_read_char(void);
void uart_handler(void);
void uart_remap(void);

#endif
