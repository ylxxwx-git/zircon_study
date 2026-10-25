/* =========================================================================
 * debuglog.h - Early Kernel Debug Console (PL011 UART + Zircon-style dprintf)
 * Reference: zircon_org/kernel/lib/debuglog/debuglog.cpp
 * ========================================================================= */
#ifndef ABOS_DEBUGLOG_H
#define ABOS_DEBUGLOG_H

#include <stdint.h>

/* Zircon debug verbosity levels */
#define CRITICAL 0
#define ALWAYS   0
#define INFO     1
#define SPEW     2

void dlog_init_early(void);
void uart_putc(char c);
void uart_puts(const char *s);
void dprintf(int level, const char *fmt, ...);

#endif /* ABOS_DEBUGLOG_H */
