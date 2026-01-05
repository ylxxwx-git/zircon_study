#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdint.h>
#include <stddef.h>

void init_uart(void);
void putc(char c);
void puts(const char *s);
void puthex(unsigned long n);
void safe_puts(const char *s);
void safe_puthex(uint64_t v);
void unsafe_puts(const char *s);
void unsafe_puthex(uint64_t v);

char getc();
char *gets(char *s);

void memset(void *s, int c, size_t n);

int strncmp(const char *s1, const char *s2, size_t n);

void puthex(uint64_t v);
void printf(const char *fmt, ...);

#endif
