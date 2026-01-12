#include "print.h"
#include "uart.h"
#include "print.h"
#include "string.h"
#include "sched.h" // For schedule()

// Locking logic
static volatile int print_lock = 0;

extern int try_lock_mutex(volatile int *lock);
extern void unlock_mutex(volatile int *lock);

static void lock_print(void) {
    while (try_lock_mutex(&print_lock) == 0) {
        // Simple spin for now, or yield if we had a way to check 'current'
        // We can use the same logic as kernel.c's lock_mutex if we have access to it
        // For now, let's assume we can just spin or use a simplified version
        // If we are in interrupt context, we shouldn't yield.
        // But printk is often called from everywhere.
        // Let's just spin-wait for simplicity in this step, or copy lock_mutex logic.
        // Since we included sched.h, we might have access to 'current' and 'schedule'
        // But 'current' might be defined in sched.c/kernel.c and not exposed in header?
        // Let's check sched.h later. For now, spin.
        asm volatile("nop");
    }
}

static void unlock_print(void) {
    unlock_mutex(&print_lock);
}

void unsafe_output(const char *s) {
    while (*s) {
        uart_write_char(*s++);
    }
}

void safe_output(const char *s) {
    lock_print();
    unsafe_output(s);
    unlock_print();
}

void kputs(const char *s) {
    safe_output(s);
}

void kputhex(uint64_t v) {
    char buf[32];
    int idx = 0;
    
    // Manual hex conversion
    buf[idx++] = '0';
    buf[idx++] = 'x';
    
    for (int i = 60; i >= 0; i -= 4) {
        int d = (v >> i) & 0xF;
        buf[idx++] = (d > 9 ? 'A' + d - 10 : '0' + d);
    }
    buf[idx] = '\0';
    
    safe_output(buf);
}

// Helper for printk
static void buffer_append(char *buf, int *idx, char c) {
    if (*idx < 255) {
        buf[(*idx)++] = c;
    }
}

static void buffer_append_str(char *buf, int *idx, const char *s) {
    while (*s) {
        buffer_append(buf, idx, *s++);
    }
}

static void buffer_append_dec(char *buf, int *idx, long n) {
    if (n < 0) {
        buffer_append(buf, idx, '-');
        n = -n;
    }
    
    if (n == 0) {
        buffer_append(buf, idx, '0');
        return;
    }
    
    char tmp[32];
    int i = 0;
    while (n > 0) {
        tmp[i++] = (n % 10) + '0';
        n /= 10;
    }
    
    while (i > 0) {
        buffer_append(buf, idx, tmp[--i]);
    }
}

static void buffer_append_hex(char *buf, int *idx, unsigned long n) {
    buffer_append_str(buf, idx, "0x");
    for (int i = 60; i >= 0; i -= 4) {
        int d = (n >> i) & 0xF;
        buffer_append(buf, idx, d > 9 ? 'A' + d - 10 : '0' + d);
    }
}

void printk(const char *fmt, ...) {
    char buf[256];
    int idx = 0;
    
    va_list args;
    va_start(args, fmt);
    
    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'c') {
                int c = va_arg(args, int);
                buffer_append(buf, &idx, (char)c);
            } else if (*fmt == 's') {
                char *s = va_arg(args, char *);
                if (!s) s = "(null)";
                buffer_append_str(buf, &idx, s);
            } else if (*fmt == 'd') {
                long n = va_arg(args, long);
                buffer_append_dec(buf, &idx, n);
            } else if (*fmt == 'x') {
                unsigned long n = va_arg(args, unsigned long);
                buffer_append_hex(buf, &idx, n);
            } else if (*fmt == 'p') {
                unsigned long n = va_arg(args, unsigned long);
                buffer_append_hex(buf, &idx, n);
            } else if (*fmt == '%') {
                buffer_append(buf, &idx, '%');
            } else {
                buffer_append(buf, &idx, '%');
                buffer_append(buf, &idx, *fmt);
            }
        } else {
            buffer_append(buf, &idx, *fmt);
        }
        fmt++;
    }
    
    va_end(args);
    
    buf[idx] = '\0';
    safe_output(buf);
}
