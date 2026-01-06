#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void *memset(void *s, int c, size_t n);
int strncmp(const char *s1, const char *s2, size_t n);
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);

#endif
