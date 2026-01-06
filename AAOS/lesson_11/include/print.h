#ifndef _PRINT_H
#define _PRINT_H

#include <stdint.h>
#include <stdarg.h>

void printk(const char *fmt, ...);
void safe_output(const char *s);
void unsafe_output(const char *s);

// Kernel put functions (safe versions by default or specific?)
// Let's keep kputs/kputhex as safe versions
void kputs(const char *s);
void kputhex(uint64_t v);

#endif
