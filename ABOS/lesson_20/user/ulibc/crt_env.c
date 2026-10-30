/* =========================================================================
 * user/ulibc/crt_env.c - C Runtime Startup Environment for Standalone EL0 Binaries
 *
 * Unpacks the bootstrap Channel message passed in x0 to _start (crt0.S),
 * stores up to 8 startup capability handles with AB_PA_* type tags (modeling
 * Zircon's processargs / zx_take_startup_handle), and provides standard
 * printf(), dlog_printf(), u_puts(), u_gets(), u_dlog().
 * ========================================================================= */
#include "ulibc.h"

#define ULIBC_MAX_STARTUP_HANDLES 8U

static ab_handle_t __ulibc_handles[ULIBC_MAX_STARTUP_HANDLES];
static uint8_t     __ulibc_handle_types[ULIBC_MAX_STARTUP_HANDLES];
static uint32_t    __ulibc_num_handles = 0;
static uint64_t    __ulibc_arg1        = 0;
static ab_handle_t __ulibc_log_h       = AB_HANDLE_INVALID;
static ab_handle_t __ulibc_con_ch      = AB_HANDLE_INVALID;

void __ulibc_init(ab_handle_t boot_ch, uint64_t arg1) {
    __ulibc_arg1 = arg1;
    for (uint32_t i = 0; i < ULIBC_MAX_STARTUP_HANDLES; i++) {
        __ulibc_handles[i]      = AB_HANDLE_INVALID;
        __ulibc_handle_types[i] = AB_PA_NONE;
    }

    if (boot_ch != AB_HANDLE_INVALID) {
        ab_proc_args_t pargs;
        memset(&pargs, 0, sizeof(pargs));
        uint32_t act_b = 0, act_h = 0;
        sys_channel_read(boot_ch, &pargs, sizeof(pargs),
                         __ulibc_handles, ULIBC_MAX_STARTUP_HANDLES,
                         &act_b, &act_h);
        sys_handle_close(boot_ch);
        __ulibc_num_handles = act_h;

        if (act_b >= sizeof(ab_proc_args_t) && pargs.magic == AB_PROC_ARGS_MAGIC) {
            for (uint32_t i = 0; i < act_h && i < ULIBC_MAX_STARTUP_HANDLES; i++) {
                __ulibc_handle_types[i] = pargs.handle_types[i];
                if (pargs.handle_types[i] == AB_PA_LOG && __ulibc_log_h == AB_HANDLE_INVALID) {
                    __ulibc_log_h = __ulibc_handles[i];
                } else if (pargs.handle_types[i] == AB_PA_CONSOLE && __ulibc_con_ch == AB_HANDLE_INVALID) {
                    __ulibc_con_ch = __ulibc_handles[i];
                }
            }
        } else {
            /* Legacy fallback if no ab_proc_args_t header was sent */
            __ulibc_log_h  = __ulibc_handles[0];
            __ulibc_con_ch = __ulibc_handles[1];
        }
    }
}

ab_handle_t ulibc_get_startup_handle(uint32_t index) {
    if (index < __ulibc_num_handles) {
        return __ulibc_handles[index];
    }
    return AB_HANDLE_INVALID;
}

ab_handle_t ulibc_get_startup_handle_by_type(uint8_t pa_type) {
    for (uint32_t i = 0; i < __ulibc_num_handles; i++) {
        if (__ulibc_handle_types[i] == pa_type) {
            return __ulibc_handles[i];
        }
    }
    return AB_HANDLE_INVALID;
}

uint64_t ulibc_get_startup_arg1(void) {
    return __ulibc_arg1;
}

ab_handle_t ulibc_get_log_handle(void) {
    return __ulibc_log_h;
}

ab_handle_t ulibc_get_console_handle(void) {
    return __ulibc_con_ch;
}

void u_puts(const char *s) {
    if (__ulibc_con_ch != AB_HANDLE_INVALID) {
        shell_print(__ulibc_con_ch, s);
    } else if (__ulibc_log_h != AB_HANDLE_INVALID) {
        ulog(__ulibc_log_h, s);
    }
}

void u_gets(char *out_buf, size_t max_len) {
    if (__ulibc_con_ch != AB_HANDLE_INVALID) {
        shell_readline(__ulibc_con_ch, out_buf, max_len);
    } else if (max_len > 0) {
        out_buf[0] = '\0';
    }
}

void u_dlog(const char *msg) {
    if (__ulibc_log_h != AB_HANDLE_INVALID) {
        ulog(__ulibc_log_h, msg);
    }
}

int printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    u_puts(buf);
    return n;
}

int dlog_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    u_dlog(buf);
    return n;
}
