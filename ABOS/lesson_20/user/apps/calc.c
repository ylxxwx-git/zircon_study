/* =========================================================================
 * user/apps/calc.c - Standalone EL0 User-Space Calculator Application
 *
 * Linked with user/ulibc/crt0.o and user/ulibc/libulibc.a at VA 0x20000000.
 * Uses standard printf() and u_gets() from ulibc!
 * ========================================================================= */
#include "ulibc.h"

int main(void) {
    dlog_printf("[calc.bin EL0 @ %p] crt0 initialized! Running main()...", (void *)main);

    printf("\n--- ABOS Calculator (loaded from disk.img:/calc.bin @ VA %p) ---\n", (void *)main);

    char num1_str[64];
    char op_str[32];
    char num2_str[64];

    printf("Enter first number: ");
    u_gets(num1_str, sizeof(num1_str));
    int ok1 = 0;
    int64_t a = u_atoi(num1_str, &ok1);
    if (!ok1) {
        printf("Error: '%s' is not a valid integer!\n\n", num1_str);
        return -1;
    }

    printf("Enter operation (+, -, *, /): ");
    u_gets(op_str, sizeof(op_str));
    char *op_p = op_str;
    while (*op_p == ' ' || *op_p == '\t') op_p++;
    char op = *op_p;
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        printf("Error: unknown operation '%c'! Use +, -, *, or /.\n\n", op ? op : '?');
        return -1;
    }

    printf("Enter second number: ");
    u_gets(num2_str, sizeof(num2_str));
    int ok2 = 0;
    int64_t b = u_atoi(num2_str, &ok2);
    if (!ok2) {
        printf("Error: '%s' is not a valid integer!\n\n", num2_str);
        return -1;
    }

    if (op == '/' && b == 0) {
        printf("Error: division by zero!\n\n");
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

    if (op == '/' && rem != 0) {
        printf("Result: %ld %c %ld = %ld (remainder %ld)\n\n", a, op, b, res, rem);
        dlog_printf("Result: %ld %c %ld = %ld (remainder %ld)", a, op, b, res, rem);
    } else {
        printf("Result: %ld %c %ld = %ld\n\n", a, op, b, res);
        dlog_printf("Result: %ld %c %ld = %ld", a, op, b, res);
        printf("My Result: %ld\n\n", res);
    }

    return 0;
}
