#ifndef OUTPUT_H
#define OUTPUT_H

#include <stdint.h>
#include <stddef.h>

void putc(char c);
void puts(const char *s);
void puthex(unsigned long n);

char getc();
char *gets(char *s);

void memset(void *s, int c, size_t n);
#endif
