#ifndef _UTILS_H
#define _UTILS_H

#include <stddef.h>
#include <stdint.h>

void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
int strcmp(const char *s1, const char *s2);
size_t strlen(const char *s);

#endif
