/* =========================================================================
 * user/ulibc/crt_env.c - C Runtime Startup Environment for Standalone Apps
 *
 * Unpacks the bootstrap Channel handle passed in x0 to _start (crt0.S),
 * stores the process's DebugLog and Console Channel capabilities, and
 * provides standard printf(), dlog_printf(), u_puts(), u_gets(), u_dlog().
 * ========================================================================= */
#include "ulibc.h"

static ab_handle_t __ulibc_log_h  = AB_HANDLE_INVALID;
static ab_handle_t __ulibc_con_ch = AB_HANDLE_INVALID;

void __ulibc_init(ab_handle_t boot_ch) {
    uint32_t tag = 0, act_b = 0, act_h = 0;
    ab_handle_t h[2] = { AB_HANDLE_INVALID, AB_HANDLE_INVALID };
    sys_channel_read(boot_ch, &tag, sizeof(tag), h, 2, &act_b, &act_h);
    sys_handle_close(boot_ch);

    __ulibc_log_h  = h[0];
    __ulibc_con_ch = h[1];
}

ab_handle_t ulibc_get_log_handle(void) {
    return __ulibc_log_h;
}

ab_handle_t ulibc_get_console_handle(void) {
    return __ulibc_con_ch;
}

void u_puts(const char *s) {
    shell_print(__ulibc_con_ch, s);
}

void u_gets(char *out_buf, size_t max_len) {
    shell_readline(__ulibc_con_ch, out_buf, max_len);
}

void u_dlog(const char *msg) {
    ulog(__ulibc_log_h, msg);
}

int printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shell_print(__ulibc_con_ch, buf);
    return n;
}

int dlog_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ulog(__ulibc_log_h, buf);
    return n;
}
