void sys_puts(const char *s) {
    asm volatile(
        "mov x0, %0\n"
        "svc #0\n"
        :
        : "r"(s)
        : "x0", "memory"
    );
}
