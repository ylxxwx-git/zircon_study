/* =========================================================================
 * user/apps/calc.c - Standalone EL0 User-Space Calculator Application
 *
 * Linked with user/ulibc/crt0.o and user/ulibc/libulibc.a at VA 0x20000000.
 * Entry point _start in crt0.S initializes the ulibc runtime environment
 * from the bootstrap Channel and calls int main(void) below!
 * ========================================================================= */
#include "ulibc.h"

int main(void) {
    u_dlog("[calc.bin EL0 @ 0x20000000] crt0 initialized! Running main()...");

    u_puts("\n--- ABOS Calculator Userspace App (loaded from disk.img:/calc.bin @ VA 0x20000000) ---\n");

    char num1_str[64];
    char op_str[32];
    char num2_str[64];

    u_puts("Enter first number: ");
    u_gets(num1_str, sizeof(num1_str));
    int ok1 = 0;
    int64_t a = u_atoi(num1_str, &ok1);
    if (!ok1) {
        u_puts("Error: invalid first number!\n\n");
        return -1;
    }

    u_puts("Enter operation (+, -, *, /): ");
    u_gets(op_str, sizeof(op_str));
    char *op_p = op_str;
    while (*op_p == ' ' || *op_p == '\t') op_p++;
    char op = *op_p;
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        u_puts("Error: unknown operation! Use +, -, *, or /.\n\n");
        return -1;
    }

    u_puts("Enter second number: ");
    u_gets(num2_str, sizeof(num2_str));
    int ok2 = 0;
    int64_t b = u_atoi(num2_str, &ok2);
    if (!ok2) {
        u_puts("Error: invalid second number!\n\n");
        return -1;
    }

    if (op == '/' && b == 0) {
        u_puts("Error: division by zero!\n\n");
        return -1;
    }

    int64_t res = 0;
    int64_t rem = 0;
    if (op == '+') res = a + b;
    else if (op == '-') res = a - b;
    else if (op == '*') res = a * b;
    else if (op == '/') {
        res = a / b;
        rem = a % b;
    }

    char a_buf[32], b_buf[32], r_buf[32], rem_buf[32], out_line[160];
    u_itoa(a, a_buf);
    u_itoa(b, b_buf);
    u_itoa(res, r_buf);

    size_t pos = 0;
    const char *prefix = "Result: ";
    for (size_t i = 0; prefix[i]; i++) out_line[pos++] = prefix[i];
    for (size_t i = 0; a_buf[i]; i++)  out_line[pos++] = a_buf[i];
    out_line[pos++] = ' ';
    out_line[pos++] = op;
    out_line[pos++] = ' ';
    for (size_t i = 0; b_buf[i]; i++)  out_line[pos++] = b_buf[i];
    out_line[pos++] = ' ';
    out_line[pos++] = '=';
    out_line[pos++] = ' ';
    for (size_t i = 0; r_buf[i]; i++)  out_line[pos++] = r_buf[i];

    if (op == '/' && rem != 0) {
        const char *rp = " (remainder ";
        for (size_t i = 0; rp[i]; i++) out_line[pos++] = rp[i];
        u_itoa(rem, rem_buf);
        for (size_t i = 0; rem_buf[i]; i++) out_line[pos++] = rem_buf[i];
        out_line[pos++] = ')';
    }
    out_line[pos++] = '\n';
    out_line[pos]   = '\0';

    u_puts(out_line);
    u_puts("\n");
    u_dlog(out_line);

    return 0;
}
