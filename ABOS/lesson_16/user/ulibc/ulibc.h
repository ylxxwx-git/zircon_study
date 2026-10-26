/* =========================================================================
 * user/ulibc/ulibc.h - ABOS User-Space C Library & Wrapped System Calls
 * ========================================================================= */
#ifndef ABOS_ULIBC_H
#define ABOS_ULIBC_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "object.h"
#include "syscalls.h"
#include "ipc_proto.h"

/* Low-level SVC entry in ulibc.c */
ab_status_t __ab_syscall4(uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t syscall_num);

/* =========================================================================
 * 1. Wrapped ABOS System Calls (User-Space ulibc API)
 * ========================================================================= */
ab_status_t sys_debuglog_write(ab_handle_t log_h, uint32_t flags, const char *msg, size_t len);
ab_status_t sys_nanosleep(uint64_t ticks);
void        sys_process_exit(int64_t retcode);

/* Handle & Object Management */
ab_status_t sys_handle_close(ab_handle_t h);
ab_status_t sys_handle_duplicate(ab_handle_t h, ab_rights_t rights, ab_handle_t *out_h);
ab_status_t sys_object_get_info(ab_handle_t h, ab_handle_basic_info_t *out_info);

/* Signals, Events & Synchronization */
ab_status_t sys_event_create(uint32_t options, ab_handle_t *out_h);
ab_status_t sys_eventpair_create(uint32_t options, ab_handle_t *out_h0, ab_handle_t *out_h1);
ab_status_t sys_object_signal(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask);
ab_status_t sys_object_signal_peer(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask);
ab_status_t sys_object_wait_one(ab_handle_t h, ab_signals_t signals,
                                uint64_t deadline, ab_signals_t *out_observed);

/* Ports (Async Event Multiplexing) */
ab_status_t sys_port_create(uint32_t options, ab_handle_t *out_port);
ab_status_t sys_port_queue(ab_handle_t port_h, const ab_port_packet_t *pkt);
ab_status_t sys_port_wait(ab_handle_t port_h, uint64_t deadline, ab_port_packet_t *out_pkt);
ab_status_t sys_object_wait_async(ab_handle_t obj_h, ab_handle_t port_h,
                                  uint64_t key, ab_signals_t signals);

/* Channels (Message + Handle Capability IPC) */
ab_status_t sys_channel_create(uint32_t options, ab_handle_t *out_ch0, ab_handle_t *out_ch1);
ab_status_t sys_channel_write(ab_handle_t ch_h, const void *bytes, uint32_t num_bytes,
                              const ab_handle_t *handles, uint32_t num_handles);
ab_status_t sys_channel_read(ab_handle_t ch_h, void *bytes, uint32_t buf_bytes,
                             ab_handle_t *handles, uint32_t buf_handles,
                             uint32_t *actual_bytes, uint32_t *actual_handles);

/* Virtual Memory Objects (VMO) & Address Space Regions (VMAR) */
ab_status_t sys_vmo_create(uint64_t size, uint32_t options, ab_handle_t *out_vmo);
ab_status_t sys_vmo_read(ab_handle_t vmo_h, void *buf, uint64_t offset, size_t len);
ab_status_t sys_vmo_write(ab_handle_t vmo_h, const void *buf, uint64_t offset, size_t len);
ab_status_t sys_vmar_map(ab_handle_t vmar_h, uint64_t vmar_offset,
                         ab_handle_t vmo_h, uint64_t vmo_offset,
                         uint64_t len, uint32_t perms, vaddr_t *out_va);

/* Tasks: Jobs, Processes & Threads */
ab_status_t sys_process_create(ab_handle_t job_h, const char *name,
                               ab_handle_t *out_proc, ab_handle_t *out_vmar);
ab_status_t sys_thread_create(ab_handle_t proc_h, const char *name, ab_handle_t *out_thr);
ab_status_t sys_process_start(ab_handle_t proc_h, ab_handle_t thr_h, uintptr_t entry,
                              uintptr_t sp, ab_handle_t arg_h, uintptr_t arg2);

/* User-Space Driver Capabilities: Physical/Contiguous DMA VMOs & Hardware IRQs */
ab_status_t sys_vmo_create_physical(ab_handle_t res_h, paddr_t pa, uint64_t size,
                                    ab_handle_t *out_vmo);
ab_status_t sys_vmo_create_contiguous(ab_handle_t res_h, uint64_t size,
                                      ab_handle_t *out_vmo, paddr_t *out_pa);
ab_status_t sys_interrupt_create(ab_handle_t res_h, uint32_t vector, ab_handle_t *out_irq);
ab_status_t sys_interrupt_wait(ab_handle_t irq_h);
ab_status_t sys_interrupt_ack(ab_handle_t irq_h);

/* =========================================================================
 * 2. C Runtime Environment, Formatted Printf & Standard Helpers
 * ========================================================================= */
void        __ulibc_init(ab_handle_t boot_ch);
int         main(void);

ab_handle_t ulibc_get_log_handle(void);
ab_handle_t ulibc_get_console_handle(void);

/* Standard Console & DebugLog I/O for standalone apps (initialized by crt0) */
void        u_puts(const char *s);
void        u_gets(char *out_buf, size_t max_len);
void        u_dlog(const char *msg);
int         printf(const char *fmt, ...);
int         dlog_printf(const char *fmt, ...);

/* Formatted string & explicit handle Console/DebugLog helpers */
int         u_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int         u_snprintf(char *buf, size_t size, const char *fmt, ...);
int         con_printf(ab_handle_t con_ch, const char *fmt, ...);
void        ulog(ab_handle_t log_h, const char *msg);
void        shell_print(ab_handle_t con_ch, const char *text);
void        shell_readline(ab_handle_t con_ch, char *out_buf, size_t max_len);

/* String & Number utilities */
void       *memcpy(void *dst, const void *src, size_t n);
void       *memset(void *dst, int c, size_t n);
size_t      u_strlen(const char *s);
int         u_strcmp(const char *a, const char *b);
int         u_strncmp(const char *a, const char *b, size_t n);
void        u_strcpy(char *dst, const char *src);
int64_t     u_atoi(const char *s, int *out_ok);
void        u_itoa(int64_t val, char *buf);

#endif /* ABOS_ULIBC_H */
