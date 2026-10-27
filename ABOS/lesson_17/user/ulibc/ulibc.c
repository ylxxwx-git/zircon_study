/* =========================================================================
 * user/ulibc/ulibc.c - ABOS Wrapped System Calls & Standard Utility Library
 * ========================================================================= */
#include "ulibc.h"
ab_status_t __ab_syscall4(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t syscall_num) {
    register uint64_t x8 __asm__("x8") = syscall_num;
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

/* =========================================================================
 * 1. Wrapped ABOS System Calls (calling __ab_syscall4 in crt0.S)
 * ========================================================================= */
ab_status_t sys_debuglog_write(ab_handle_t log_h, uint32_t flags, const char *msg, size_t len) {
    return __ab_syscall4(log_h, flags, (uintptr_t)msg, len, SYS_DEBUGLOG_WRITE);
}

ab_status_t sys_nanosleep(uint64_t ticks) {
    return __ab_syscall4(ticks, 0, 0, 0, SYS_NANOSLEEP);
}

void sys_process_exit(int64_t retcode) {
    (void)__ab_syscall4((uint64_t)retcode, 0, 0, 0, SYS_PROCESS_EXIT);
    for (;;) { }
}

ab_status_t sys_handle_close(ab_handle_t h) {
    return __ab_syscall4(h, 0, 0, 0, SYS_HANDLE_CLOSE);
}

ab_status_t sys_handle_duplicate(ab_handle_t h, ab_rights_t rights, ab_handle_t *out_h) {
    return __ab_syscall4(h, rights, (uintptr_t)out_h, 0, SYS_HANDLE_DUPLICATE);
}

ab_status_t sys_object_get_info(ab_handle_t h, ab_handle_basic_info_t *out_info) {
    return __ab_syscall4(h, (uintptr_t)out_info, 0, 0, SYS_OBJECT_GET_INFO);
}

ab_status_t sys_event_create(uint32_t options, ab_handle_t *out_h) {
    return __ab_syscall4(options, (uintptr_t)out_h, 0, 0, SYS_EVENT_CREATE);
}

ab_status_t sys_eventpair_create(uint32_t options, ab_handle_t *out_h0, ab_handle_t *out_h1) {
    return __ab_syscall4(options, (uintptr_t)out_h0, (uintptr_t)out_h1, 0, SYS_EVENTPAIR_CREATE);
}

ab_status_t sys_object_signal(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask) {
    return __ab_syscall4(h, clear_mask, set_mask, 0, SYS_OBJECT_SIGNAL);
}

ab_status_t sys_object_signal_peer(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask) {
    return __ab_syscall4(h, clear_mask, set_mask, 0, SYS_OBJECT_SIGNAL_PEER);
}

ab_status_t sys_object_wait_one(ab_handle_t h, ab_signals_t signals,
                                uint64_t deadline, ab_signals_t *out_observed) {
    return __ab_syscall4(h, signals, deadline, (uintptr_t)out_observed, SYS_OBJECT_WAIT_ONE);
}

ab_status_t sys_port_create(uint32_t options, ab_handle_t *out_port) {
    return __ab_syscall4(options, (uintptr_t)out_port, 0, 0, SYS_PORT_CREATE);
}

ab_status_t sys_port_queue(ab_handle_t port_h, const ab_port_packet_t *pkt) {
    return __ab_syscall4(port_h, (uintptr_t)pkt, 0, 0, SYS_PORT_QUEUE);
}

ab_status_t sys_port_wait(ab_handle_t port_h, uint64_t deadline, ab_port_packet_t *out_pkt) {
    return __ab_syscall4(port_h, deadline, (uintptr_t)out_pkt, 0, SYS_PORT_WAIT);
}

ab_status_t sys_object_wait_async(ab_handle_t obj_h, ab_handle_t port_h,
                                  uint64_t key, ab_signals_t signals) {
    return __ab_syscall4(obj_h, port_h, key, signals, SYS_OBJECT_WAIT_ASYNC);
}

ab_status_t sys_channel_create(uint32_t options, ab_handle_t *out_ch0, ab_handle_t *out_ch1) {
    return __ab_syscall4(options, (uintptr_t)out_ch0, (uintptr_t)out_ch1, 0, SYS_CHANNEL_CREATE);
}

ab_status_t sys_channel_write(ab_handle_t ch_h, const void *bytes, uint32_t num_bytes,
                              const ab_handle_t *handles, uint32_t num_handles) {
    ab_channel_write_args_t args = { bytes, num_bytes, handles, num_handles };
    return __ab_syscall4(ch_h, 0, (uintptr_t)&args, 0, SYS_CHANNEL_WRITE);
}

ab_status_t sys_channel_read(ab_handle_t ch_h, void *bytes, uint32_t buf_bytes,
                             ab_handle_t *handles, uint32_t buf_handles,
                             uint32_t *actual_bytes, uint32_t *actual_handles) {
    ab_channel_read_args_t args = {
        bytes, handles, buf_bytes, buf_handles, actual_bytes, actual_handles
    };
    return __ab_syscall4(ch_h, 0, (uintptr_t)&args, 0, SYS_CHANNEL_READ);
}

ab_status_t sys_vmo_create(uint64_t size, uint32_t options, ab_handle_t *out_vmo) {
    return __ab_syscall4(size, options, (uintptr_t)out_vmo, 0, SYS_VMO_CREATE);
}

ab_status_t sys_vmo_read(ab_handle_t vmo_h, void *buf, uint64_t offset, size_t len) {
    return __ab_syscall4(vmo_h, (uintptr_t)buf, offset, len, SYS_VMO_READ);
}

ab_status_t sys_vmo_write(ab_handle_t vmo_h, const void *buf, uint64_t offset, size_t len) {
    return __ab_syscall4(vmo_h, (uintptr_t)buf, offset, len, SYS_VMO_WRITE);
}

ab_status_t sys_vmar_map(ab_handle_t vmar_h, uint64_t vmar_offset,
                         ab_handle_t vmo_h, uint64_t vmo_offset,
                         uint64_t len, uint32_t perms, vaddr_t *out_va) {
    ab_vmar_map_args_t args = { vmo_h, vmo_offset, len, perms, out_va };
    return __ab_syscall4(vmar_h, vmar_offset, (uintptr_t)&args, 0, SYS_VMAR_MAP);
}

ab_status_t sys_process_create(ab_handle_t job_h, const char *name,
                               ab_handle_t *out_proc, ab_handle_t *out_vmar) {
    ab_process_create_args_t args = { name, u_strlen(name), out_proc, out_vmar };
    return __ab_syscall4(job_h, (uintptr_t)&args, 0, 0, SYS_PROCESS_CREATE);
}

ab_status_t sys_thread_create(ab_handle_t proc_h, const char *name, ab_handle_t *out_thr) {
    return __ab_syscall4(proc_h, (uintptr_t)name, u_strlen(name), (uintptr_t)out_thr, SYS_THREAD_CREATE);
}

ab_status_t sys_process_start(ab_handle_t proc_h, ab_handle_t thr_h, uintptr_t entry,
                              uintptr_t sp, ab_handle_t arg_h, uintptr_t arg2) {
    ab_process_start_args_t args = { thr_h, entry, sp, arg_h, arg2 };
    return __ab_syscall4(proc_h, (uintptr_t)&args, 0, 0, SYS_PROCESS_START);
}

ab_status_t sys_vmo_create_physical(ab_handle_t res_h, paddr_t pa, uint64_t size,
                                    ab_handle_t *out_vmo) {
    ab_vmo_create_phys_args_t args = { pa, size, out_vmo };
    return __ab_syscall4(res_h, (uintptr_t)&args, 0, 0, SYS_VMO_CREATE_PHYSICAL);
}

ab_status_t sys_vmo_create_contiguous(ab_handle_t res_h, uint64_t size,
                                      ab_handle_t *out_vmo, paddr_t *out_pa) {
    ab_vmo_create_contig_args_t args = { size, out_vmo, out_pa };
    return __ab_syscall4(res_h, (uintptr_t)&args, 0, 0, SYS_VMO_CREATE_CONTIG);
}

ab_status_t sys_interrupt_create(ab_handle_t res_h, uint32_t vector, ab_handle_t *out_irq) {
    return __ab_syscall4(res_h, vector, (uintptr_t)out_irq, 0, SYS_INTERRUPT_CREATE);
}

ab_status_t sys_interrupt_wait(ab_handle_t irq_h) {
    return __ab_syscall4(irq_h, 0, 0, 0, SYS_INTERRUPT_WAIT);
}

ab_status_t sys_interrupt_ack(ab_handle_t irq_h) {
    return __ab_syscall4(irq_h, 0, 0, 0, SYS_INTERRUPT_ACK);
}

ab_status_t sys_object_set_name(ab_handle_t h, const char *name) {
    return __ab_syscall4(h, (uintptr_t)name, name ? u_strlen(name) : 0, 0, SYS_OBJECT_SET_NAME);
}

ab_status_t sys_job_get_ps_info(ab_handle_t job_h, ab_task_info_entry_t *entries,
                                uint32_t max_entries, uint32_t *actual_out) {
    return __ab_syscall4(job_h, (uintptr_t)entries, max_entries, (uintptr_t)actual_out,
                         SYS_JOB_GET_PS_INFO);
}

ab_status_t sys_process_get_maps(ab_handle_t job_h, ab_koid_t proc_koid,
                                 ab_info_maps_t *maps, uint32_t max_maps,
                                 uint32_t *actual_out) {
    ab_process_get_maps_args_t args = { proc_koid, maps, max_maps, actual_out };
    return __ab_syscall4(job_h, (uintptr_t)&args, 0, 0, SYS_PROCESS_GET_MAPS);
}

/* =========================================================================
 * 2. Standard String, Number & IPC Console Helpers
 * ========================================================================= */
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *sr = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = sr[i];
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
    return dst;
}

size_t u_strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int u_strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int u_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0') {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

void u_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++) != '\0') { }
}

int64_t u_atoi(const char *s, int *out_ok) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if (*s < '0' || *s > '9') {
        if (out_ok) *out_ok = 0;
        return 0;
    }
    int64_t val = 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
    }
    while (*s == ' ' || *s == '\t') s++;
    if (*s != '\0') {
        if (out_ok) *out_ok = 0;
        return 0;
    }
    if (out_ok) *out_ok = 1;
    return sign * val;
}

void u_itoa(int64_t val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    int neg = 0;
    uint64_t uval;
    if (val < 0) {
        neg = 1;
        uval = (uint64_t)(-val);
    } else {
        uval = (uint64_t)val;
    }
    char tmp[32];
    int i = 0;
    while (uval > 0) {
        tmp[i++] = (char)('0' + (uval % 10));
        uval /= 10;
    }
    int pos = 0;
    if (neg) buf[pos++] = '-';
    while (i > 0) {
        buf[pos++] = tmp[--i];
    }
    buf[pos] = '\0';
}

void ulog(ab_handle_t log_h, const char *msg) {
    sys_debuglog_write(log_h, 0, msg, u_strlen(msg));
}

void shell_print(ab_handle_t con_ch, const char *text) {
    console_msg_t msg;
    msg.op = CON_OP_WRITE;
    size_t i = 0;
    while (text[i]) {
        size_t chunk = 0;
        while (text[i] && chunk + 1 < sizeof(msg.text)) {
            msg.text[chunk++] = text[i++];
        }
        msg.text[chunk] = '\0';
        sys_channel_write(con_ch, &msg, sizeof(msg), NULL, 0);

        ab_signals_t obs = 0;
        sys_object_wait_one(con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);
        uint32_t ack = 0, ab = 0, ah = 0;
        sys_channel_read(con_ch, &ack, sizeof(ack), NULL, 0, &ab, &ah);
    }
}

void shell_readline(ab_handle_t con_ch, char *out_buf, size_t max_len) {
    console_msg_t req = { CON_OP_READ_LINE, "" };
    sys_channel_write(con_ch, &req, sizeof(uint32_t), NULL, 0);

    ab_signals_t obs = 0;
    sys_object_wait_one(con_ch, AB_SIGNAL_READABLE, AB_TIME_INFINITE, &obs);

    console_msg_t rep;
    uint32_t ab = 0, ah = 0;
    sys_channel_read(con_ch, &rep, sizeof(rep), NULL, 0, &ab, &ah);

    size_t i = 0;
    while (rep.text[i] && i + 1 < max_len) {
        out_buf[i] = rep.text[i];
        i++;
    }
    out_buf[i] = '\0';
}

/* =========================================================================
 * 3. Formatted Output Engine: u_vsnprintf, u_snprintf, con_printf
 * ========================================================================= */
static void buf_putc(char *buf, size_t size, size_t *pos, char c) {
    if (*pos + 1 < size) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void buf_puts(char *buf, size_t size, size_t *pos, const char *s) {
    while (*s) {
        buf_putc(buf, size, pos, *s++);
    }
}

static void buf_put_uint(char *buf, size_t size, size_t *pos,
                         uint64_t val, uint32_t base, int width, char pad, int upper) {
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (val == 0) {
        tmp[i++] = '0';
    } else {
        while (val > 0) {
            tmp[i++] = digits[val % base];
            val /= base;
        }
    }
    while (i < width) {
        tmp[i++] = pad;
    }
    while (i > 0) {
        buf_putc(buf, size, pos, tmp[--i]);
    }
}

int u_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (!buf || size == 0) return 0;

    while (*fmt) {
        if (*fmt != '%') {
            buf_putc(buf, size, &pos, *fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        int left_align = 0;
        if (*fmt == '-') {
            left_align = 1;
            fmt++;
        }

        char pad = ' ';
        if (*fmt == '0' && !left_align) {
            pad = '0';
            fmt++;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;
        while (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int slen = (int)u_strlen(s);
                if (!left_align && width > slen) {
                    for (int p = 0; p < width - slen; p++) buf_putc(buf, size, &pos, ' ');
                }
                buf_puts(buf, size, &pos, s);
                if (left_align && width > slen) {
                    for (int p = 0; p < width - slen; p++) buf_putc(buf, size, &pos, ' ');
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                buf_putc(buf, size, &pos, c);
                break;
            }
            case 'd':
            case 'i': {
                int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
                if (v < 0) {
                    buf_putc(buf, size, &pos, '-');
                    buf_put_uint(buf, size, &pos, (uint64_t)(-v), 10, width, pad, 0);
                } else {
                    buf_put_uint(buf, size, &pos, (uint64_t)v, 10, width, pad, 0);
                }
                break;
            }
            case 'u': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                buf_put_uint(buf, size, &pos, v, 10, width, pad, 0);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t v = is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned int);
                buf_put_uint(buf, size, &pos, v, 16, width, pad, (*fmt == 'X'));
                break;
            }
            case 'p': {
                uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
                buf_puts(buf, size, &pos, "0x");
                buf_put_uint(buf, size, &pos, v, 16, 16, '0', 0);
                break;
            }
            case '%':
                buf_putc(buf, size, &pos, '%');
                break;
            case '\0':
                goto done;
            default:
                buf_putc(buf, size, &pos, '%');
                buf_putc(buf, size, &pos, *fmt);
                break;
        }
        fmt++;
    }

done:
    buf[(pos < size) ? pos : (size - 1)] = '\0';
    return (int)pos;
}

int u_snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = u_vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int con_printf(ab_handle_t con_ch, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = u_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shell_print(con_ch, buf);
    return n;
}

/* Models format_size_fixed() in zircon_org/system/ulib/pretty/sizes.c */
char *u_format_size(char *str, size_t str_size, size_t bytes) {
    const char *units = "BkMGTPE";
    int ui = 0;
    uint32_t r = 0;
    int whole = 1;

    while (bytes >= 10000 || (bytes != 0 && (bytes & 1023) == 0)) {
        ui++;
        if (ui >= 6) break;
        if (bytes & 1023) {
            whole = 0;
        }
        r = (uint32_t)(bytes % 1024);
        bytes /= 1024;
    }

    if (!whole) {
        uint32_t round_up = ((r % 100) >= 50) ? 1U : 0U;
        r = (r / 100) + round_up;
        if (r == 10) {
            bytes++;
            r = 0;
            if (bytes >= 1024 && (bytes & 1023) == 0 && ui < 6) {
                bytes /= 1024;
                ui++;
                whole = 1;
            }
        }
    }

    if (whole) {
        u_snprintf(str, str_size, "%lu%c", (uint64_t)bytes, units[ui]);
    } else {
        u_snprintf(str, str_size, "%lu.%u%c", (uint64_t)bytes, r, units[ui]);
    }
    return str;
}
