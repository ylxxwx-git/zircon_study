/* =========================================================================
 * calc.c - Standalone EL0 User-Space Calculator Binary ('calc.bin')
 *
 * Compiled separately from the kernel, linked at user VA 0x20000000,
 * stored inside the USTAR TAR archive on disk.img, and loaded at runtime
 * by abos-shell via VMO + SYS_VMAR_MAP + SYS_PROCESS_START!
 * ========================================================================= */
#include <stdint.h>
#include <stddef.h>
#include "object.h"
#include "syscalls.h"

#define CON_OP_WRITE      1U
#define CON_OP_READ_LINE  2U

typedef struct {
    uint32_t op;
    char     text[240];
} console_msg_t;

static inline __attribute__((always_inline))
ab_status_t user_syscall4(uint64_t num, uint64_t a0, uint64_t a1,
                          uint64_t a2, uint64_t a3) {
    register uint64_t x8 __asm__("x8") = num;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    register uint64_t x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : "r"(x8)
                     : "memory");
    return (ab_status_t)x0;
}

static size_t u_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void u_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++) != '\0') { }
}

static void u_append(char *dst, const char *src) {
    while (*dst) dst++;
    while ((*dst++ = *src++) != '\0') { }
}

static void ulog(ab_handle_t log_h, const char *msg) {
    user_syscall4(SYS_DEBUGLOG_WRITE, log_h, 0, (uintptr_t)msg, u_strlen(msg));
}

static void con_print(ab_handle_t con_ch, const char *text) {
    size_t total = u_strlen(text);
    size_t off = 0;
    while (off < total) {
        console_msg_t msg;
        msg.op = CON_OP_WRITE;
        size_t chunk = total - off;
        if (chunk > 200) chunk = 200;
        for (size_t i = 0; i < chunk; i++) msg.text[i] = text[off + i];
        msg.text[chunk] = '\0';
        off += chunk;

        ab_channel_write_args_t cw = { &msg, sizeof(msg), NULL, 0 };
        user_syscall4(SYS_CHANNEL_WRITE, con_ch, 0, (uintptr_t)&cw, 0);

        ab_signals_t obs = 0;
        user_syscall4(SYS_OBJECT_WAIT_ONE, con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
        uint32_t act_b = 0, act_h = 0;
        console_msg_t ack;
        ab_channel_read_args_t cr = { &ack, NULL, sizeof(ack), 0, &act_b, &act_h };
        user_syscall4(SYS_CHANNEL_READ, con_ch, 0, (uintptr_t)&cr, 0);
    }
}

static void con_readline(ab_handle_t con_ch, char *out_buf) {
    console_msg_t req;
    req.op = CON_OP_READ_LINE;
    req.text[0] = '\0';
    ab_channel_write_args_t cw = { &req, sizeof(uint32_t), NULL, 0 };
    user_syscall4(SYS_CHANNEL_WRITE, con_ch, 0, (uintptr_t)&cw, 0);

    ab_signals_t obs = 0;
    user_syscall4(SYS_OBJECT_WAIT_ONE, con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, (uintptr_t)&obs);
    uint32_t act_b = 0, act_h = 0;
    console_msg_t rep;
    ab_channel_read_args_t cr = { &rep, NULL, sizeof(rep), 0, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, con_ch, 0, (uintptr_t)&cr, 0);
    u_strcpy(out_buf, rep.text);
}

static int u_parse_int64(const char *str, int64_t *out_val) {
    if (!str) return -1;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == '\0') return -1;

    int neg = 0;
    if (*str == '-') {
        neg = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    if (*str < '0' || *str > '9') return -1;

    int64_t val = 0;
    while (*str >= '0' && *str <= '9') {
        val = val * 10 + (*str - '0');
        str++;
    }
    while (*str == ' ' || *str == '\t') str++;
    if (*str != '\0') return -1;

    *out_val = neg ? -val : val;
    return 0;
}

static void u_int64_to_str(int64_t val, char *buf) {
    char tmp[32];
    int i = 0;
    uint64_t uval;

    if (val < 0) {
        *buf++ = '-';
        uval = (uint64_t)(-val);
    } else {
        uval = (uint64_t)val;
    }

    if (uval == 0) {
        *buf++ = '0';
        *buf = '\0';
        return;
    }

    while (uval > 0) {
        tmp[i++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    while (i > 0) {
        *buf++ = tmp[--i];
    }
    *buf = '\0';
}

__attribute__((section(".text.entry")))
void _start(uint64_t arg0) {
    ab_handle_t boot_ch = (ab_handle_t)arg0;

    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[2];
    ab_channel_read_args_t rargs = { &tag, h, sizeof(tag), 2, &act_b, &act_h };
    user_syscall4(SYS_CHANNEL_READ, boot_ch, 0, (uintptr_t)&rargs, 0);
    user_syscall4(SYS_HANDLE_CLOSE, boot_ch, 0, 0, 0);

    ab_handle_t log_h  = h[0];
    ab_handle_t con_ch = h[1];

    ulog(log_h, "[calc.bin EL0 @ 0x20000000] Loaded from TAR disk VMO! Running calculator...");
    con_print(con_ch, "\n--- ABOS Calculator (loaded from disk.img:/calc.bin @ VA 0x20000000) ---\n");

    char num1_str[240];
    char op_str[240];
    char num2_str[240];

    con_print(con_ch, "Enter first number: ");
    con_readline(con_ch, num1_str);

    int64_t a = 0;
    if (u_parse_int64(num1_str, &a) != 0) {
        con_print(con_ch, "Error: Invalid first number!\n");
        user_syscall4(SYS_HANDLE_CLOSE, con_ch, 0, 0, 0);
        user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
        user_syscall4(SYS_PROCESS_EXIT, 1, 0, 0, 0);
    }

    con_print(con_ch, "Enter operation (+, -, *, /): ");
    con_readline(con_ch, op_str);

    char op = op_str[0];
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        con_print(con_ch, "Error: Unsupported operation (use +, -, *, or /)!\n");
        user_syscall4(SYS_HANDLE_CLOSE, con_ch, 0, 0, 0);
        user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
        user_syscall4(SYS_PROCESS_EXIT, 1, 0, 0, 0);
    }

    con_print(con_ch, "Enter second number: ");
    con_readline(con_ch, num2_str);

    int64_t b = 0;
    if (u_parse_int64(num2_str, &b) != 0) {
        con_print(con_ch, "Error: Invalid second number!\n");
        user_syscall4(SYS_HANDLE_CLOSE, con_ch, 0, 0, 0);
        user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
        user_syscall4(SYS_PROCESS_EXIT, 1, 0, 0, 0);
    }

    if (op == '/' && b == 0) {
        con_print(con_ch, "Error: Division by zero is undefined!\n");
        user_syscall4(SYS_HANDLE_CLOSE, con_ch, 0, 0, 0);
        user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
        user_syscall4(SYS_PROCESS_EXIT, 1, 0, 0, 0);
    }

    int64_t res = 0;
    int64_t rem = 0;
    if (op == '+')      res = a + b;
    else if (op == '-') res = a - b;
    else if (op == '*') res = a * b;
    else if (op == '/') {
        res = a / b;
        rem = a % b;
    }

    char out_line[200];
    char s_a[32], s_b[32], s_res[32], s_rem[32];
    char op_buf[4] = { ' ', op, ' ', '\0' };
    u_int64_to_str(a, s_a);
    u_int64_to_str(b, s_b);
    u_int64_to_str(res, s_res);

    out_line[0] = '\0';
    u_append(out_line, "Result: ");
    u_append(out_line, s_a);
    u_append(out_line, op_buf);
    u_append(out_line, s_b);
    u_append(out_line, " = ");
    u_append(out_line, s_res);

    if (op == '/' && rem != 0) {
        u_int64_to_str(rem, s_rem);
        u_append(out_line, " (remainder ");
        u_append(out_line, s_rem);
        u_append(out_line, ")");
    }
    u_append(out_line, "\n");

    con_print(con_ch, out_line);
    ulog(log_h, out_line);

    user_syscall4(SYS_HANDLE_CLOSE, con_ch, 0, 0, 0);
    user_syscall4(SYS_HANDLE_CLOSE, log_h, 0, 0, 0);
    user_syscall4(SYS_PROCESS_EXIT, 0, 0, 0, 0);
    for (;;) { }
}
