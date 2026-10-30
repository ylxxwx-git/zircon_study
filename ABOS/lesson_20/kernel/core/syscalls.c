/* =========================================================================
 * syscalls.c - Capability & Signal System Call Implementation
 * ========================================================================= */
#include "syscalls.h"
#include "thread.h"
#include "heap.h"
#include "mmu.h"
#include "debuglog.h"

/* =========================================================================
 * Safe EL0 <-> EL1 User Pointer Validation & Copy Helpers
 * ========================================================================= */
static int user_range_valid(uintptr_t uaddr, size_t len) {
    if (len == 0) return 1;
    uintptr_t limit = USER_ASPACE_BASE + USER_ASPACE_SIZE;
    if (uaddr < USER_ASPACE_BASE || uaddr >= limit) return 0;
    if (len > limit - uaddr) return 0;
    return 1;
}

static ab_status_t resolve_user_page(ab_process_t *proc, vaddr_t page_va,
                                     int is_write, paddr_t *out_pa) {
    if (!proc || !proc->aspace) return AB_ERR_BAD_STATE;

    paddr_t pa = 0;
    pte_t pte  = 0;
    if (vmm_resolve_vaddr(proc->aspace, page_va, &pa, &pte) != 0) {
        /* Page may belong to a lazy VMAR mapping not yet faulted in */
        if (ab_process_handle_page_fault(proc, page_va, is_write, 0) != 0) {
            return AB_ERR_INVALID_ARGS;
        }
        if (vmm_resolve_vaddr(proc->aspace, page_va, &pa, &pte) != 0) {
            return AB_ERR_INVALID_ARGS;
        }
    }

    if (!(pte & PTE_AP_USER)) {
        return AB_ERR_ACCESS_DENIED;
    }
    if (is_write && (pte & PTE_AP_RO)) {
        /* May be a writable VMAR mapping where PTE wasn't upgraded, or truly RO */
        return AB_ERR_ACCESS_DENIED;
    }
    /* Reject Device MMIO pages in syscall memory buffers */
    if ((pte & (7ULL << 2)) != PTE_ATTR_NORMAL ||
        pa < 0x40000000ULL || pa >= 0x80000000ULL) {
        return AB_ERR_ACCESS_DENIED;
    }

    *out_pa = pa;
    return AB_OK;
}

ab_status_t copy_from_user(void *kdst, const void *usrc, size_t len) {
    if (len == 0) return AB_OK;
    if (!kdst || !usrc) return AB_ERR_INVALID_ARGS;

    uintptr_t uaddr = (uintptr_t)usrc;
    if (!user_range_valid(uaddr, len)) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_process_t *proc = get_current_process();
    uint8_t *dst = (uint8_t *)kdst;
    size_t done = 0;

    while (done < len) {
        uintptr_t cur_va = uaddr + done;
        vaddr_t page_va  = cur_va & ~(PAGE_SIZE - 1);
        size_t page_off  = (size_t)(cur_va & (PAGE_SIZE - 1));
        size_t chunk     = PAGE_SIZE - page_off;
        if (chunk > len - done) chunk = len - done;

        paddr_t pa = 0;
        ab_status_t st = resolve_user_page(proc, page_va, 0, &pa);
        if (st != AB_OK) return st;

        const uint8_t *kva = (const uint8_t *)paddr_to_physmap(pa) + page_off;
        for (size_t i = 0; i < chunk; i++) {
            dst[done + i] = kva[i];
        }
        done += chunk;
    }
    return AB_OK;
}

ab_status_t copy_to_user(void *udst, const void *ksrc, size_t len) {
    if (len == 0) return AB_OK;
    if (!udst || !ksrc) return AB_ERR_INVALID_ARGS;

    uintptr_t uaddr = (uintptr_t)udst;
    if (!user_range_valid(uaddr, len)) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_process_t *proc = get_current_process();
    const uint8_t *src = (const uint8_t *)ksrc;
    size_t done = 0;

    while (done < len) {
        uintptr_t cur_va = uaddr + done;
        vaddr_t page_va  = cur_va & ~(PAGE_SIZE - 1);
        size_t page_off  = (size_t)(cur_va & (PAGE_SIZE - 1));
        size_t chunk     = PAGE_SIZE - page_off;
        if (chunk > len - done) chunk = len - done;

        paddr_t pa = 0;
        ab_status_t st = resolve_user_page(proc, page_va, 1, &pa);
        if (st != AB_OK) return st;

        uint8_t *kva = (uint8_t *)paddr_to_physmap(pa) + page_off;
        for (size_t i = 0; i < chunk; i++) {
            kva[i] = src[done + i];
        }
        done += chunk;
    }
    return AB_OK;
}

/* =========================================================================
 * System Call Handlers
 * ========================================================================= */
static ab_status_t sys_handle_close(ab_handle_t handle) {
    return handle_close(get_current_process(), handle);
}

static ab_status_t sys_handle_duplicate(ab_handle_t handle, ab_rights_t rights,
                                        ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_handle_t new_h = AB_HANDLE_INVALID;
    ab_status_t st = handle_duplicate(proc, handle, rights, &new_h);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out, &new_h, sizeof(new_h));
    if (st != AB_OK) {
        handle_close(proc, new_h);
    }
    return st;
}

static ab_status_t sys_object_get_info(ab_handle_t handle,
                                       ab_handle_basic_info_t *user_info) {
    if (!user_info) return AB_ERR_INVALID_ARGS;
    ab_object_t *obj = NULL;
    ab_rights_t rights = 0;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                                   AB_RIGHT_INSPECT, &obj, &rights);
    if (st != AB_OK) return st;

    ab_handle_basic_info_t kinfo;
    kinfo.koid         = obj->koid;
    kinfo.related_koid = obj->related_koid;
    kinfo.type         = (uint32_t)obj->type;
    kinfo.rights       = rights;
    kinfo.handle_count = (uint32_t)__atomic_load_n(&obj->handle_count, __ATOMIC_RELAXED);
    ab_object_release(obj);

    return copy_to_user(user_info, &kinfo, sizeof(kinfo));
}

static ab_status_t sys_debuglog_write(ab_handle_t log_handle, uint32_t flags,
                                      const char *user_buf, size_t len) {
    (void)flags;
    if (!user_buf || len > 256) return AB_ERR_INVALID_ARGS;

    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), log_handle,
                                   AB_OBJ_TYPE_LOG, AB_RIGHT_WRITE,
                                   &obj, NULL);
    if (st != AB_OK) return st;

    char kbuf[257];
    st = copy_from_user(kbuf, user_buf, len);
    if (st != AB_OK) {
        ab_object_release(obj);
        return st;
    }
    kbuf[len] = '\0';

    ab_process_t *proc = get_current_process();
    dprintf(INFO, "[debuglog cpu=%u proc='%s' koid=%lu] %s\n",
            arch_curr_cpu_num(), proc ? proc->name : "unknown", obj->koid, kbuf);

    ab_object_release(obj);
    return AB_OK;
}

static ab_status_t sys_event_create(uint32_t options, ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_object_t *ev = NULL;
    ab_status_t st = ab_event_dispatcher_create(options, &ev);
    if (st != AB_OK) return st;

    ab_handle_t h = AB_HANDLE_INVALID;
    st = handle_alloc(proc, ev, AB_RIGHTS_DEFAULT_EVENT, &h);
    ab_object_release(ev);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out, &h, sizeof(h));
    if (st != AB_OK) {
        handle_close(proc, h);
    }
    return st;
}

static ab_status_t sys_eventpair_create(uint32_t options,
                                        ab_handle_t *user_out0,
                                        ab_handle_t *user_out1) {
    if (options != 0 || !user_out0 || !user_out1) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();

    ab_object_t *ep0 = NULL, *ep1 = NULL;
    ab_status_t st = ab_eventpair_dispatcher_create(&ep0, &ep1);
    if (st != AB_OK) return st;

    ab_handle_t h0 = AB_HANDLE_INVALID, h1 = AB_HANDLE_INVALID;
    st = handle_alloc(proc, ep0, AB_RIGHTS_DEFAULT_EVENTPAIR, &h0);
    if (st == AB_OK) {
        st = handle_alloc(proc, ep1, AB_RIGHTS_DEFAULT_EVENTPAIR, &h1);
        if (st != AB_OK) {
            handle_close(proc, h0);
        }
    }
    ab_object_release(ep0);
    ab_object_release(ep1);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out0, &h0, sizeof(h0));
    if (st == AB_OK) {
        st = copy_to_user(user_out1, &h1, sizeof(h1));
    }
    if (st != AB_OK) {
        handle_close(proc, h0);
        handle_close(proc, h1);
    }
    return st;
}

#define USER_ALLOWED_SIGNALS (AB_SIGNAL_SIGNALED | AB_USER_SIGNAL_ALL)

static ab_status_t sys_object_signal(ab_handle_t handle,
                                     ab_signals_t clear_mask,
                                     ab_signals_t set_mask) {
    if ((clear_mask & ~USER_ALLOWED_SIGNALS) || (set_mask & ~USER_ALLOWED_SIGNALS)) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                                   AB_RIGHT_SIGNAL, &obj, NULL);
    if (st != AB_OK) return st;

    state_tracker_update_signals(obj, clear_mask, set_mask);
    ab_object_release(obj);
    return AB_OK;
}

static ab_status_t sys_object_signal_peer(ab_handle_t handle,
                                          ab_signals_t clear_mask,
                                          ab_signals_t set_mask) {
    if ((clear_mask & ~USER_ALLOWED_SIGNALS) || (set_mask & ~USER_ALLOWED_SIGNALS)) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_EVENTPAIR,
                                   AB_RIGHT_SIGNAL_PEER, &obj, NULL);
    if (st != AB_OK) return st;

    st = ab_eventpair_signal_peer(obj, clear_mask, set_mask);
    ab_object_release(obj);
    return st;
}

static ab_status_t sys_object_wait_one(ab_handle_t handle,
                                       ab_signals_t watched_signals,
                                       uint64_t deadline_ns,
                                       ab_signals_t *user_observed) {
    if (user_observed && !user_range_valid((uintptr_t)user_observed, sizeof(ab_signals_t))) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                                   AB_RIGHT_WAIT, &obj, NULL);
    if (st != AB_OK) return st;

    ab_signals_t kobs = 0;
    st = state_tracker_wait_one(obj, watched_signals, deadline_ns, &kobs);
    ab_object_release(obj);

    if (user_observed) {
        ab_status_t cst = copy_to_user(user_observed, &kobs, sizeof(kobs));
        if (cst != AB_OK) return cst;
    }
    return st;
}

static ab_status_t sys_port_create(uint32_t options, ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_object_t *port = NULL;
    ab_status_t st = ab_port_dispatcher_create(options, &port);
    if (st != AB_OK) return st;

    ab_handle_t h = AB_HANDLE_INVALID;
    st = handle_alloc(proc, port, AB_RIGHTS_DEFAULT_PORT, &h);
    ab_object_release(port);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out, &h, sizeof(h));
    if (st != AB_OK) {
        handle_close(proc, h);
    }
    return st;
}

static ab_status_t sys_port_queue(ab_handle_t port_handle,
                                  const ab_port_packet_t *user_pkt) {
    if (!user_pkt) return AB_ERR_INVALID_ARGS;
    ab_port_packet_t kpkt;
    ab_status_t st = copy_from_user(&kpkt, user_pkt, sizeof(kpkt));
    if (st != AB_OK) return st;

    ab_object_t *port = NULL;
    st = handle_lookup(get_current_process(), port_handle,
                       AB_OBJ_TYPE_PORT, AB_RIGHT_WRITE, &port, NULL);
    if (st != AB_OK) return st;

    st = ab_port_queue(port, &kpkt);
    ab_object_release(port);
    return st;
}

static ab_status_t sys_port_wait(ab_handle_t port_handle,
                                 uint64_t deadline_ns,
                                 ab_port_packet_t *user_out_pkt) {
    if (!user_out_pkt || !user_range_valid((uintptr_t)user_out_pkt, sizeof(ab_port_packet_t))) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *port = NULL;
    ab_status_t st = handle_lookup(get_current_process(), port_handle,
                                   AB_OBJ_TYPE_PORT, AB_RIGHT_READ, &port, NULL);
    if (st != AB_OK) return st;

    ab_port_packet_t kpkt;
    st = ab_port_wait(port, deadline_ns, &kpkt);
    ab_object_release(port);
    if (st != AB_OK) return st;

    return copy_to_user(user_out_pkt, &kpkt, sizeof(kpkt));
}

static ab_status_t sys_object_wait_async(ab_handle_t handle,
                                         ab_handle_t port_handle,
                                         uint64_t key,
                                         ab_signals_t signals) {
    ab_process_t *proc = get_current_process();
    ab_object_t *obj = NULL;
    ab_status_t st = handle_lookup(proc, handle, AB_OBJ_TYPE_NONE,
                                   AB_RIGHT_WAIT, &obj, NULL);
    if (st != AB_OK) return st;

    ab_object_t *port = NULL;
    st = handle_lookup(proc, port_handle, AB_OBJ_TYPE_PORT,
                       AB_RIGHT_WRITE, &port, NULL);
    if (st != AB_OK) {
        ab_object_release(obj);
        return st;
    }

    st = state_tracker_wait_async(obj, port, key, signals);
    ab_object_release(port);
    ab_object_release(obj);
    return st;
}

static ab_status_t sys_channel_create(uint32_t options,
                                      ab_handle_t *user_out0,
                                      ab_handle_t *user_out1) {
    if (options != 0 || !user_out0 || !user_out1) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();

    ab_object_t *ch0 = NULL, *ch1 = NULL;
    ab_status_t st = ab_channel_dispatcher_create(&ch0, &ch1);
    if (st != AB_OK) return st;

    ab_handle_t h0 = AB_HANDLE_INVALID, h1 = AB_HANDLE_INVALID;
    st = handle_alloc(proc, ch0, AB_RIGHTS_DEFAULT_CHANNEL, &h0);
    if (st == AB_OK) {
        st = handle_alloc(proc, ch1, AB_RIGHTS_DEFAULT_CHANNEL, &h1);
        if (st != AB_OK) handle_close(proc, h0);
    }
    ab_object_release(ch0);
    ab_object_release(ch1);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out0, &h0, sizeof(h0));
    if (st == AB_OK) {
        st = copy_to_user(user_out1, &h1, sizeof(h1));
    }
    if (st != AB_OK) {
        handle_close(proc, h0);
        handle_close(proc, h1);
    }
    return st;
}

static ab_status_t sys_channel_write(ab_handle_t handle, uint32_t options,
                                     const ab_channel_write_args_t *user_args) {
    if (options != 0 || !user_args) return AB_ERR_INVALID_ARGS;

    ab_channel_write_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;

    if (kargs.num_bytes > CHANNEL_MAX_MSG_BYTES ||
        kargs.num_handles > CHANNEL_MAX_MSG_HANDLES) {
        return AB_ERR_OUT_OF_RANGE;
    }

    uint8_t kbytes[CHANNEL_MAX_MSG_BYTES];
    ab_handle_t khandles[CHANNEL_MAX_MSG_HANDLES];
    if (kargs.num_bytes > 0) {
        st = copy_from_user(kbytes, kargs.bytes, kargs.num_bytes);
        if (st != AB_OK) return st;
    }
    if (kargs.num_handles > 0) {
        st = copy_from_user(khandles, kargs.handles,
                            kargs.num_handles * sizeof(ab_handle_t));
        if (st != AB_OK) return st;
    }

    ab_process_t *proc = get_current_process();
    ab_object_t *ch = NULL;
    st = handle_lookup(proc, handle, AB_OBJ_TYPE_CHANNEL,
                       AB_RIGHT_WRITE, &ch, NULL);
    if (st != AB_OK) return st;

    st = ab_channel_write(proc, ch,
                          (kargs.num_bytes > 0) ? kbytes : NULL, kargs.num_bytes,
                          (kargs.num_handles > 0) ? khandles : NULL, kargs.num_handles);
    ab_object_release(ch);
    return st;
}

static ab_status_t sys_channel_read(ab_handle_t handle, uint32_t options,
                                    ab_channel_read_args_t *user_args) {
    if (options != 0 || !user_args) return AB_ERR_INVALID_ARGS;

    ab_channel_read_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;

    uint32_t max_b = (kargs.max_bytes > CHANNEL_MAX_MSG_BYTES)
                         ? CHANNEL_MAX_MSG_BYTES : kargs.max_bytes;
    uint32_t max_h = (kargs.max_handles > CHANNEL_MAX_MSG_HANDLES)
                         ? CHANNEL_MAX_MSG_HANDLES : kargs.max_handles;
    if ((max_b > 0 && !user_range_valid((uintptr_t)kargs.bytes, max_b)) ||
        (max_h > 0 && !user_range_valid((uintptr_t)kargs.handles, max_h * sizeof(ab_handle_t)))) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_process_t *proc = get_current_process();
    ab_object_t *ch = NULL;
    st = handle_lookup(proc, handle, AB_OBJ_TYPE_CHANNEL,
                       AB_RIGHT_READ, &ch, NULL);
    if (st != AB_OK) return st;

    uint8_t kbytes[CHANNEL_MAX_MSG_BYTES];
    ab_handle_t khandles[CHANNEL_MAX_MSG_HANDLES];
    uint32_t act_b = 0, act_h = 0;

    st = ab_channel_read(proc, ch, kbytes, khandles, max_b, max_h, &act_b, &act_h);
    ab_object_release(ch);

    if (st == AB_OK || st == AB_ERR_BUFFER_TOO_SMALL) {
        if (kargs.actual_bytes) {
            ab_status_t cst = copy_to_user(kargs.actual_bytes, &act_b, sizeof(act_b));
            if (cst != AB_OK && st == AB_OK) st = cst;
        }
        if (kargs.actual_handles) {
            ab_status_t cst = copy_to_user(kargs.actual_handles, &act_h, sizeof(act_h));
            if (cst != AB_OK && st == AB_OK) st = cst;
        }
    }

    if (st == AB_OK) {
        if (act_b > 0) {
            st = copy_to_user(kargs.bytes, kbytes, act_b);
        }
        if (st == AB_OK && act_h > 0) {
            st = copy_to_user(kargs.handles, khandles, act_h * sizeof(ab_handle_t));
        }
        if (st != AB_OK) {
            for (uint32_t i = 0; i < act_h; i++) {
                handle_close(proc, khandles[i]);
            }
        }
    }
    return st;
}

static ab_status_t sys_vmo_create(uint64_t size, uint32_t options,
                                  ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *proc = get_current_process();
    ab_object_t *vmo = NULL;
    ab_status_t st = ab_vmo_dispatcher_create((size_t)size, options, &vmo);
    if (st != AB_OK) return st;

    ab_handle_t h = AB_HANDLE_INVALID;
    st = handle_alloc(proc, vmo, AB_RIGHTS_DEFAULT_VMO, &h);
    ab_object_release(vmo);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out, &h, sizeof(h));
    if (st != AB_OK) {
        handle_close(proc, h);
    }
    return st;
}

static ab_status_t sys_vmo_read(ab_handle_t handle, void *user_buf,
                                uint64_t offset, size_t len) {
    if (len == 0) return AB_OK;
    if (!user_buf || !user_range_valid((uintptr_t)user_buf, len)) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *vmo = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_VMO,
                                   AB_RIGHT_READ, &vmo, NULL);
    if (st != AB_OK) return st;

    uint8_t kchunk[256];
    size_t done = 0;
    while (done < len) {
        size_t chunk = (len - done > sizeof(kchunk)) ? sizeof(kchunk) : (len - done);
        st = ab_vmo_read(vmo, kchunk, offset + done, chunk);
        if (st != AB_OK) break;
        st = copy_to_user((uint8_t *)user_buf + done, kchunk, chunk);
        if (st != AB_OK) break;
        done += chunk;
    }

    ab_object_release(vmo);
    return st;
}

static ab_status_t sys_vmo_write(ab_handle_t handle, const void *user_buf,
                                 uint64_t offset, size_t len) {
    if (len == 0) return AB_OK;
    if (!user_buf || !user_range_valid((uintptr_t)user_buf, len)) {
        return AB_ERR_INVALID_ARGS;
    }
    ab_object_t *vmo = NULL;
    ab_status_t st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_VMO,
                                   AB_RIGHT_WRITE, &vmo, NULL);
    if (st != AB_OK) return st;

    uint8_t kchunk[256];
    size_t done = 0;
    while (done < len) {
        size_t chunk = (len - done > sizeof(kchunk)) ? sizeof(kchunk) : (len - done);
        st = copy_from_user(kchunk, (const uint8_t *)user_buf + done, chunk);
        if (st != AB_OK) break;
        st = ab_vmo_write(vmo, kchunk, offset + done, chunk);
        if (st != AB_OK) break;
        done += chunk;
    }

    ab_object_release(vmo);
    return st;
}

static ab_status_t sys_vmar_map(ab_handle_t vmar_handle, size_t vmar_offset,
                                const ab_vmar_map_args_t *user_args) {
    if (!user_args) return AB_ERR_INVALID_ARGS;
    ab_vmar_map_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;
    if (!kargs.mapped_va_out) return AB_ERR_INVALID_ARGS;

    ab_process_t *proc = get_current_process();
    ab_object_t *vmar = NULL;
    st = handle_lookup(proc, vmar_handle, AB_OBJ_TYPE_VMAR,
                       AB_RIGHT_MAP, &vmar, NULL);
    if (st != AB_OK) return st;

    /* Enforce capability rights on the VMO being mapped */
    ab_rights_t req_vmo_rights = AB_RIGHT_MAP;
    if (kargs.map_flags & AB_VM_PERM_READ)    req_vmo_rights |= AB_RIGHT_READ;
    if (kargs.map_flags & AB_VM_PERM_WRITE)   req_vmo_rights |= AB_RIGHT_WRITE;
    if (kargs.map_flags & AB_VM_PERM_EXECUTE) req_vmo_rights |= AB_RIGHT_EXECUTE;

    ab_object_t *vmo = NULL;
    st = handle_lookup(proc, kargs.vmo_handle, AB_OBJ_TYPE_VMO,
                       req_vmo_rights, &vmo, NULL);
    if (st != AB_OK) {
        ab_object_release(vmar);
        return st;
    }

    vaddr_t mapped_va = 0;
    st = ab_vmar_map(proc, vmar, vmar_offset, vmo,
                     kargs.vmo_offset, (size_t)kargs.len,
                     kargs.map_flags, &mapped_va);
    if (st == AB_OK) {
        st = copy_to_user(kargs.mapped_va_out, &mapped_va, sizeof(mapped_va));
        if (st != AB_OK) {
            ab_vmar_unmap(proc, vmar, mapped_va, (size_t)kargs.len);
        }
    }

    ab_object_release(vmo);
    ab_object_release(vmar);
    return st;
}

static ab_status_t sys_vmar_unmap(ab_handle_t vmar_handle, vaddr_t addr, size_t len) {
    ab_process_t *proc = get_current_process();
    ab_object_t *vmar = NULL;
    ab_status_t st = handle_lookup(proc, vmar_handle, AB_OBJ_TYPE_VMAR,
                                   AB_RIGHT_MAP, &vmar, NULL);
    if (st != AB_OK) return st;

    st = ab_vmar_unmap(proc, vmar, addr, len);
    ab_object_release(vmar);
    return st;
}

static ab_status_t sys_process_create(ab_handle_t job_handle,
                                      const ab_process_create_args_t *user_args) {
    if (!user_args) return AB_ERR_INVALID_ARGS;
    ab_process_create_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;
    if (!kargs.name || !kargs.proc_handle_out || !kargs.vmar_handle_out) {
        return AB_ERR_INVALID_ARGS;
    }

    char kname[32];
    size_t n = (kargs.name_len < 31) ? kargs.name_len : 31;
    st = copy_from_user(kname, kargs.name, n);
    if (st != AB_OK) return st;
    kname[n] = '\0';

    ab_process_t *caller = get_current_process();
    ab_object_t *job_obj = NULL;
    st = handle_lookup(caller, job_handle, AB_OBJ_TYPE_JOB,
                       AB_RIGHT_MANAGE_PROCESS, &job_obj, NULL);
    if (st != AB_OK) return st;

    ab_process_t *new_proc = ab_process_create_kernel(kname, ab_alloc_asid());
    if (!new_proc) {
        ab_object_release(job_obj);
        return AB_ERR_NO_MEMORY;
    }

    ab_object_t *vmar_obj = NULL;
    st = ab_vmar_dispatcher_create(new_proc, &vmar_obj);
    if (st != AB_OK) {
        ab_object_release(&new_proc->base);
        ab_object_release(job_obj);
        return st;
    }

    ab_handle_t proc_h = AB_HANDLE_INVALID, vmar_h = AB_HANDLE_INVALID;
    st = handle_alloc(caller, &new_proc->base, AB_RIGHTS_DEFAULT_PROCESS, &proc_h);
    if (st == AB_OK) {
        st = handle_alloc(caller, vmar_obj, AB_RIGHTS_DEFAULT_VMAR, &vmar_h);
        if (st != AB_OK) handle_close(caller, proc_h);
    }

    if (st == AB_OK) {
        st = copy_to_user(kargs.proc_handle_out, &proc_h, sizeof(proc_h));
        if (st == AB_OK) {
            st = copy_to_user(kargs.vmar_handle_out, &vmar_h, sizeof(vmar_h));
        }
        if (st == AB_OK) {
            ab_job_add_process((ab_job_dispatcher_t *)job_obj, new_proc);
        } else {
            handle_close(caller, proc_h);
            handle_close(caller, vmar_h);
        }
    }

    ab_object_release(vmar_obj);
    ab_object_release(&new_proc->base);
    ab_object_release(job_obj);
    return st;
}

static ab_status_t sys_thread_create(ab_handle_t proc_handle,
                                     const char *user_name, size_t name_len,
                                     ab_handle_t *user_out) {
    if (!user_name || !user_out) return AB_ERR_INVALID_ARGS;

    char kname[32];
    size_t n = (name_len < 31) ? name_len : 31;
    ab_status_t st = copy_from_user(kname, user_name, n);
    if (st != AB_OK) return st;
    kname[n] = '\0';

    ab_process_t *caller = get_current_process();
    ab_object_t *proc_obj = NULL;
    st = handle_lookup(caller, proc_handle, AB_OBJ_TYPE_PROCESS,
                       AB_RIGHT_MANAGE_THREAD, &proc_obj, NULL);
    if (st != AB_OK) return st;

    ab_object_t *td_obj = NULL;
    st = ab_thread_dispatcher_create((ab_process_t *)proc_obj, kname, &td_obj);
    if (st == AB_OK) {
        ab_handle_t thr_h = AB_HANDLE_INVALID;
        st = handle_alloc(caller, td_obj, AB_RIGHTS_DEFAULT_THREAD, &thr_h);
        if (st == AB_OK) {
            st = copy_to_user(user_out, &thr_h, sizeof(thr_h));
            if (st != AB_OK) handle_close(caller, thr_h);
        }
        ab_object_release(td_obj);
    }
    ab_object_release(proc_obj);
    return st;
}

static ab_status_t sys_process_start(ab_handle_t proc_handle,
                                     const ab_process_start_args_t *user_args) {
    if (!user_args) return AB_ERR_INVALID_ARGS;
    ab_process_start_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;

    ab_process_t *caller = get_current_process();
    ab_object_t *proc_obj = NULL;
    st = handle_lookup(caller, proc_handle, AB_OBJ_TYPE_PROCESS,
                       AB_RIGHT_WRITE, &proc_obj, NULL);
    if (st != AB_OK) return st;

    ab_object_t *thr_obj = NULL;
    st = handle_lookup(caller, kargs.thread_handle, AB_OBJ_TYPE_THREAD,
                       AB_RIGHT_WRITE, &thr_obj, NULL);
    if (st != AB_OK) {
        ab_object_release(proc_obj);
        return st;
    }

    st = ab_process_start_user(caller, (ab_process_t *)proc_obj,
                               (ab_thread_dispatcher_t *)thr_obj,
                               kargs.entry_pc, kargs.user_sp,
                               kargs.arg_handle, kargs.arg2);
    ab_object_release(thr_obj);
    ab_object_release(proc_obj);
    return st;
}

static ab_status_t sys_vmo_create_physical(ab_handle_t resource_handle,
                                           const ab_vmo_create_phys_args_t *user_args) {
    if (!user_args) return AB_ERR_INVALID_ARGS;
    ab_vmo_create_phys_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;
    if (!kargs.vmo_out) return AB_ERR_INVALID_ARGS;

    ab_process_t *caller = get_current_process();
    ab_object_t *res_obj = NULL;
    st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
                       AB_RIGHT_WRITE, &res_obj, NULL);
    if (st != AB_OK) return st;
    ab_object_release(res_obj);

    ab_object_t *vmo_obj = NULL;
    st = ab_vmo_create_physical(kargs.paddr, kargs.size, &vmo_obj);
    if (st != AB_OK) return st;

    ab_handle_t vmo_h = AB_HANDLE_INVALID;
    st = handle_alloc(caller, vmo_obj, AB_RIGHTS_DEFAULT_VMO, &vmo_h);
    ab_object_release(vmo_obj);
    if (st != AB_OK) return st;

    st = copy_to_user(kargs.vmo_out, &vmo_h, sizeof(vmo_h));
    if (st != AB_OK) {
        handle_close(caller, vmo_h);
    }
    return st;
}

static ab_status_t sys_vmo_create_contiguous(ab_handle_t resource_handle,
                                             const ab_vmo_create_contig_args_t *user_args) {
    if (!user_args) return AB_ERR_INVALID_ARGS;
    ab_vmo_create_contig_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;
    if (!kargs.vmo_out || !kargs.paddr_out) return AB_ERR_INVALID_ARGS;

    ab_process_t *caller = get_current_process();
    ab_object_t *res_obj = NULL;
    st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
                       AB_RIGHT_WRITE, &res_obj, NULL);
    if (st != AB_OK) return st;
    ab_object_release(res_obj);

    ab_object_t *vmo_obj = NULL;
    paddr_t pa = 0;
    st = ab_vmo_create_contiguous(kargs.size, &vmo_obj, &pa);
    if (st != AB_OK) return st;

    ab_handle_t vmo_h = AB_HANDLE_INVALID;
    st = handle_alloc(caller, vmo_obj, AB_RIGHTS_DEFAULT_VMO, &vmo_h);
    ab_object_release(vmo_obj);
    if (st != AB_OK) return st;

    st = copy_to_user(kargs.paddr_out, &pa, sizeof(pa));
    if (st == AB_OK) {
        st = copy_to_user(kargs.vmo_out, &vmo_h, sizeof(vmo_h));
    }
    if (st != AB_OK) {
        handle_close(caller, vmo_h);
    }
    return st;
}

static ab_status_t sys_interrupt_create(ab_handle_t resource_handle,
                                        uint32_t irq_num,
                                        ab_handle_t *user_out) {
    if (!user_out) return AB_ERR_INVALID_ARGS;
    ab_process_t *caller = get_current_process();

    ab_object_t *res_obj = NULL;
    ab_status_t st = handle_lookup(caller, resource_handle, AB_OBJ_TYPE_RESOURCE,
                                   AB_RIGHT_READ, &res_obj, NULL);
    if (st != AB_OK) return st;
    ab_object_release(res_obj);

    ab_object_t *intr_obj = NULL;
    st = ab_interrupt_dispatcher_create(irq_num, &intr_obj);
    if (st != AB_OK) return st;

    ab_handle_t intr_h = AB_HANDLE_INVALID;
    st = handle_alloc(caller, intr_obj, AB_RIGHTS_DEFAULT_INTERRUPT, &intr_h);
    ab_object_release(intr_obj);
    if (st != AB_OK) return st;

    st = copy_to_user(user_out, &intr_h, sizeof(intr_h));
    if (st != AB_OK) {
        handle_close(caller, intr_h);
    }
    return st;
}

static ab_status_t sys_interrupt_wait(ab_handle_t intr_handle) {
    ab_object_t *intr_obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), intr_handle,
                                   AB_OBJ_TYPE_INTERRUPT, AB_RIGHT_WAIT,
                                   &intr_obj, NULL);
    if (st != AB_OK) return st;
    st = ab_interrupt_wait(intr_obj);
    ab_object_release(intr_obj);
    return st;
}

static ab_status_t sys_interrupt_ack(ab_handle_t intr_handle) {
    ab_object_t *intr_obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), intr_handle,
                                   AB_OBJ_TYPE_INTERRUPT, AB_RIGHT_WRITE,
                                   &intr_obj, NULL);
    if (st != AB_OK) return st;
    st = ab_interrupt_ack(intr_obj);
    ab_object_release(intr_obj);
    return st;
}

static ab_status_t sys_object_set_name(ab_handle_t handle, const char *user_name, size_t len) {
    if (!user_name) return AB_ERR_INVALID_ARGS;
    char kname[32];
    size_t n = (len < 31) ? len : 31;
    ab_status_t st = copy_from_user(kname, user_name, n);
    if (st != AB_OK) return st;
    kname[n] = '\0';

    ab_object_t *obj = NULL;
    st = handle_lookup(get_current_process(), handle, AB_OBJ_TYPE_NONE,
                       AB_RIGHT_NONE, &obj, NULL);
    if (st != AB_OK) return st;
    st = ab_object_set_name(obj, kname, n);
    ab_object_release(obj);
    return st;
}

static ab_status_t sys_job_get_ps_info(ab_handle_t job_handle,
                                       ab_task_info_entry_t *user_entries,
                                       uint32_t max_entries,
                                       uint32_t *user_actual) {
    if (!user_entries || !user_actual || max_entries == 0) {
        return AB_ERR_INVALID_ARGS;
    }
    uint32_t capped = (max_entries > 64U) ? 64U : max_entries;
    if (!user_range_valid((uintptr_t)user_entries, capped * sizeof(ab_task_info_entry_t)) ||
        !user_range_valid((uintptr_t)user_actual, sizeof(uint32_t))) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_object_t *job_obj = NULL;
    ab_status_t st = handle_lookup(get_current_process(), job_handle, AB_OBJ_TYPE_JOB,
                                   AB_RIGHT_INSPECT, &job_obj, NULL);
    if (st != AB_OK) return st;

    ab_task_info_entry_t *kentries =
        (ab_task_info_entry_t *)kzalloc(capped * sizeof(ab_task_info_entry_t));
    if (!kentries) {
        ab_object_release(job_obj);
        return AB_ERR_NO_MEMORY;
    }

    uint32_t actual = 0;
    st = ab_job_get_ps_info(job_obj, kentries, capped, &actual);
    ab_object_release(job_obj);

    if (st == AB_OK) {
        if (actual > 0) {
            st = copy_to_user(user_entries, kentries, actual * sizeof(ab_task_info_entry_t));
        }
        if (st == AB_OK) {
            st = copy_to_user(user_actual, &actual, sizeof(actual));
        }
    }
    kfree(kentries);
    return st;
}

static ab_status_t sys_process_get_maps(ab_handle_t job_handle,
                                        const ab_process_get_maps_args_t *user_args) {
    if (!user_args) return AB_ERR_INVALID_ARGS;
    ab_process_get_maps_args_t kargs;
    ab_status_t st = copy_from_user(&kargs, user_args, sizeof(kargs));
    if (st != AB_OK) return st;
    if (!kargs.maps_out || !kargs.actual_out || kargs.max_maps == 0) {
        return AB_ERR_INVALID_ARGS;
    }

    uint32_t capped = (kargs.max_maps > 32U) ? 32U : kargs.max_maps;
    if (!user_range_valid((uintptr_t)kargs.maps_out, capped * sizeof(ab_info_maps_t)) ||
        !user_range_valid((uintptr_t)kargs.actual_out, sizeof(uint32_t))) {
        return AB_ERR_INVALID_ARGS;
    }

    ab_object_t *job_obj = NULL;
    st = handle_lookup(get_current_process(), job_handle, AB_OBJ_TYPE_JOB,
                       AB_RIGHT_INSPECT, &job_obj, NULL);
    if (st != AB_OK) return st;

    ab_info_maps_t *kmaps = (ab_info_maps_t *)kzalloc(capped * sizeof(ab_info_maps_t));
    if (!kmaps) {
        ab_object_release(job_obj);
        return AB_ERR_NO_MEMORY;
    }

    uint32_t actual = 0;
    st = ab_job_get_process_maps(job_obj, kargs.target_proc_koid, kmaps, capped, &actual);
    ab_object_release(job_obj);

    if (st == AB_OK) {
        if (actual > 0) {
            st = copy_to_user(kargs.maps_out, kmaps, actual * sizeof(ab_info_maps_t));
        }
        if (st == AB_OK) {
            st = copy_to_user(kargs.actual_out, &actual, sizeof(actual));
        }
    }
    kfree(kmaps);
    return st;
}

void ab_process_exit_current(int exit_code) {
    thread_t *cur = get_current_thread();
    ab_process_t *proc = cur ? (ab_process_t *)cur->process_obj : NULL;
    ab_thread_dispatcher_t *uthr = cur ? (ab_thread_dispatcher_t *)cur->thread_obj : NULL;

    if (proc) {
        ab_object_add_ref(&proc->base);
        proc->exit_code = exit_code;
        proc->exited    = 1;

        dprintf(INFO, "[process '%s' koid=%lu] exiting with code %d; closing remaining handles\n",
                proc->name, proc->base.koid, exit_code);

        /* Switch CPU off this process's TTBR0_EL1 before unmapping or freeing aspace */
        if (cur) {
            cur->aspace      = NULL;
            cur->process_obj = NULL;
            cur->thread_obj  = NULL;
        }
        vmm_switch_aspace(NULL);

        /* Unmap and release all active VMAR mappings */
        ab_vmo_dispatcher_t *vmos_to_rel[MAX_VMAR_MAPPINGS];
        uint32_t vmo_rel_cnt = 0;
        spin_lock_saved_state_t irqstate;
        spin_lock_irqsave(&proc->vmar_lock, &irqstate);
        for (uint32_t i = 0; i < MAX_VMAR_MAPPINGS; i++) {
            if (proc->mappings[i].active) {
                if (proc->aspace) {
                    for (size_t off = 0; off < proc->mappings[i].size; off += PAGE_SIZE) {
                        vmm_unmap_page(proc->aspace, proc->mappings[i].base_va + off);
                    }
                }
                if (proc->mappings[i].vmo) {
                    vmos_to_rel[vmo_rel_cnt++] = proc->mappings[i].vmo;
                    proc->mappings[i].vmo = NULL;
                }
                proc->mappings[i].active = 0;
            }
        }
        spin_lock_irqrestore(&proc->vmar_lock, irqstate);

        for (uint32_t i = 0; i < vmo_rel_cnt; i++) {
            ab_object_release(&vmos_to_rel[i]->base);
        }

        /* Close all open capability handles owned by this process */
        for (uint32_t slot = 0; slot < MAX_HANDLES_PER_PROCESS; slot++) {
            uint32_t gen = 0;
            int has_h = 0;
            spin_lock_irqsave(&proc->handle_lock, &irqstate);
            if (proc->handles[slot].obj != NULL) {
                gen   = proc->handles[slot].generation;
                has_h = 1;
            }
            spin_lock_irqrestore(&proc->handle_lock, irqstate);

            if (has_h) {
                ab_handle_t h = (0x5U << 28) | ((gen & 0xFFFFU) << 8) | slot;
                handle_close(proc, h);
            }
        }

        if (uthr) {
            state_tracker_update_signals(&uthr->base, 0, AB_SIGNAL_TASK_TERMINATED);
            ab_object_release(&uthr->base);
        }

        state_tracker_update_signals(&proc->base, 0, AB_SIGNAL_TASK_TERMINATED);
        ab_job_remove_process(proc);
        ab_object_release(&proc->base);
    }

    thread_exit(exit_code);
}

void syscall_dispatch(iframe_t *frame) {
    uint64_t sys_num = frame->r[8];
    ab_status_t ret = AB_ERR_NOT_SUPPORTED;

    switch (sys_num) {
        case SYS_HANDLE_CLOSE:
            ret = sys_handle_close((ab_handle_t)frame->r[0]);
            break;
        case SYS_HANDLE_DUPLICATE:
            ret = sys_handle_duplicate((ab_handle_t)frame->r[0],
                                       (ab_rights_t)frame->r[1],
                                       (ab_handle_t *)frame->r[2]);
            break;
        case SYS_OBJECT_GET_INFO:
            ret = sys_object_get_info((ab_handle_t)frame->r[0],
                                      (ab_handle_basic_info_t *)frame->r[1]);
            break;
        case SYS_DEBUGLOG_WRITE:
            ret = sys_debuglog_write((ab_handle_t)frame->r[0],
                                     (uint32_t)frame->r[1],
                                     (const char *)frame->r[2],
                                     (size_t)frame->r[3]);
            break;
        case SYS_PROCESS_EXIT:
            ab_process_exit_current((int)frame->r[0]);
            break;
        case SYS_NANOSLEEP:
            thread_sleep_ns(frame->r[0]);
            ret = AB_OK;
            break;
        case SYS_EVENT_CREATE:
            ret = sys_event_create((uint32_t)frame->r[0],
                                   (ab_handle_t *)frame->r[1]);
            break;
        case SYS_EVENTPAIR_CREATE:
            ret = sys_eventpair_create((uint32_t)frame->r[0],
                                       (ab_handle_t *)frame->r[1],
                                       (ab_handle_t *)frame->r[2]);
            break;
        case SYS_OBJECT_SIGNAL:
            ret = sys_object_signal((ab_handle_t)frame->r[0],
                                    (ab_signals_t)frame->r[1],
                                    (ab_signals_t)frame->r[2]);
            break;
        case SYS_OBJECT_SIGNAL_PEER:
            ret = sys_object_signal_peer((ab_handle_t)frame->r[0],
                                         (ab_signals_t)frame->r[1],
                                         (ab_signals_t)frame->r[2]);
            break;
        case SYS_OBJECT_WAIT_ONE:
            ret = sys_object_wait_one((ab_handle_t)frame->r[0],
                                      (ab_signals_t)frame->r[1],
                                      (uint64_t)frame->r[2],
                                      (ab_signals_t *)frame->r[3]);
            break;
        case SYS_PORT_CREATE:
            ret = sys_port_create((uint32_t)frame->r[0],
                                  (ab_handle_t *)frame->r[1]);
            break;
        case SYS_PORT_QUEUE:
            ret = sys_port_queue((ab_handle_t)frame->r[0],
                                 (const ab_port_packet_t *)frame->r[1]);
            break;
        case SYS_PORT_WAIT:
            ret = sys_port_wait((ab_handle_t)frame->r[0],
                                (uint64_t)frame->r[1],
                                (ab_port_packet_t *)frame->r[2]);
            break;
        case SYS_OBJECT_WAIT_ASYNC:
            ret = sys_object_wait_async((ab_handle_t)frame->r[0],
                                        (ab_handle_t)frame->r[1],
                                        (uint64_t)frame->r[2],
                                        (ab_signals_t)frame->r[3]);
            break;
        case SYS_CHANNEL_CREATE:
            ret = sys_channel_create((uint32_t)frame->r[0],
                                     (ab_handle_t *)frame->r[1],
                                     (ab_handle_t *)frame->r[2]);
            break;
        case SYS_CHANNEL_WRITE:
            ret = sys_channel_write((ab_handle_t)frame->r[0],
                                    (uint32_t)frame->r[1],
                                    (const ab_channel_write_args_t *)frame->r[2]);
            break;
        case SYS_CHANNEL_READ:
            ret = sys_channel_read((ab_handle_t)frame->r[0],
                                   (uint32_t)frame->r[1],
                                   (ab_channel_read_args_t *)frame->r[2]);
            break;
        case SYS_VMO_CREATE:
            ret = sys_vmo_create((uint64_t)frame->r[0],
                                 (uint32_t)frame->r[1],
                                 (ab_handle_t *)frame->r[2]);
            break;
        case SYS_VMO_READ:
            ret = sys_vmo_read((ab_handle_t)frame->r[0],
                               (void *)frame->r[1],
                               (uint64_t)frame->r[2],
                               (size_t)frame->r[3]);
            break;
        case SYS_VMO_WRITE:
            ret = sys_vmo_write((ab_handle_t)frame->r[0],
                                (const void *)frame->r[1],
                                (uint64_t)frame->r[2],
                                (size_t)frame->r[3]);
            break;
        case SYS_VMAR_MAP:
            ret = sys_vmar_map((ab_handle_t)frame->r[0],
                               (size_t)frame->r[1],
                               (const ab_vmar_map_args_t *)frame->r[2]);
            break;
        case SYS_VMAR_UNMAP:
            ret = sys_vmar_unmap((ab_handle_t)frame->r[0],
                                 (vaddr_t)frame->r[1],
                                 (size_t)frame->r[2]);
            break;
        case SYS_PROCESS_CREATE:
            ret = sys_process_create((ab_handle_t)frame->r[0],
                                     (const ab_process_create_args_t *)frame->r[1]);
            break;
        case SYS_THREAD_CREATE:
            ret = sys_thread_create((ab_handle_t)frame->r[0],
                                    (const char *)frame->r[1],
                                    (size_t)frame->r[2],
                                    (ab_handle_t *)frame->r[3]);
            break;
        case SYS_PROCESS_START:
            ret = sys_process_start((ab_handle_t)frame->r[0],
                                    (const ab_process_start_args_t *)frame->r[1]);
            break;
        case SYS_VMO_CREATE_PHYSICAL:
            ret = sys_vmo_create_physical((ab_handle_t)frame->r[0],
                                          (const ab_vmo_create_phys_args_t *)frame->r[1]);
            break;
        case SYS_VMO_CREATE_CONTIG:
            ret = sys_vmo_create_contiguous((ab_handle_t)frame->r[0],
                                            (const ab_vmo_create_contig_args_t *)frame->r[1]);
            break;
        case SYS_INTERRUPT_CREATE:
            ret = sys_interrupt_create((ab_handle_t)frame->r[0],
                                       (uint32_t)frame->r[1],
                                       (ab_handle_t *)frame->r[2]);
            break;
        case SYS_INTERRUPT_WAIT:
            ret = sys_interrupt_wait((ab_handle_t)frame->r[0]);
            break;
        case SYS_INTERRUPT_ACK:
            ret = sys_interrupt_ack((ab_handle_t)frame->r[0]);
            break;
        case SYS_OBJECT_SET_NAME:
            ret = sys_object_set_name((ab_handle_t)frame->r[0],
                                      (const char *)frame->r[1],
                                      (size_t)frame->r[2]);
            break;
        case SYS_JOB_GET_PS_INFO:
            ret = sys_job_get_ps_info((ab_handle_t)frame->r[0],
                                      (ab_task_info_entry_t *)frame->r[1],
                                      (uint32_t)frame->r[2],
                                      (uint32_t *)frame->r[3]);
            break;
        case SYS_PROCESS_GET_MAPS:
            ret = sys_process_get_maps((ab_handle_t)frame->r[0],
                                       (const ab_process_get_maps_args_t *)frame->r[1]);
            break;
        default:
            dprintf(CRITICAL, "Unknown syscall %lu\n", sys_num);
            ret = AB_ERR_NOT_SUPPORTED;
            break;
    }

    frame->r[0] = (uint64_t)(int64_t)ret;
}
