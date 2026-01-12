#ifndef _LIB_H
#define _LIB_H

#include <stddef.h>

void sys_write(char * buf, int len);
int sys_read(char * buf, int len);
void sys_exit(int code);
void sys_yield(void);

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);

#endif
