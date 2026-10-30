# References

Following the annotated-bibliography tradition of Dr. Zhao Jiong's *A Heavily Commented Linux Kernel Source Code*, each entry below lists a primary hardware specification, operating-system monograph, or foundational research paper together with the specific subsystems of the ABOS microkernel that draw directly upon it.

1. **[1] ARM Limited, *ARM Architecture Reference Manual for A-profile architecture* (ARM DDI 0487).**
   The definitive architectural specification for 64-bit ARMv8-A processors. Essential reference for ABOS's Exception Levels (`EL0`–`EL3`), `VBAR_EL1` exception vector table layout (`kernel/core/exceptions.S`), Exception Syndrome Register `ESR_EL1` and Fault Address Register `FAR_EL1` decoding (`kernel/core/exceptions_c.c`), Stage-1 Virtual Memory System Architecture (`VMSA`) 4 KB translation tables, `TCR_EL1` / `MAIR_EL1` / `SCTLR_EL1`, 16-bit ASID-tagged TLB maintenance (`tlbi vae1is` / `tlbi aside1is` in `kernel/mm/mmu.c`), Load-Acquire / Store-Release spinlocks (`ldaxr` / `stlxr`), and the ARMv8 Generic Timer (`cntpct_el0`, `cntfrq_el0`, `cntp_tval_el0`, `cntkctl_el1` in `kernel/dev/timer.c`).

2. **[2] Google Fuchsia Project, *Zircon Kernel Documentation and Source Tree* (`https://fuchsia.dev/fuchsia-src/concepts/kernel`).**
   Primary architectural blueprint for ABOS's capability-based object model (`Dispatcher`, `StateTracker`, and `WaitStateObserver` in `include/object.h` and `kernel/core/object.c`), 32-bit generation-checked handles (`ab_handle_t`), Kernel Object IDs (`ab_koid_t`), asynchronous event multiplexing (`PortDispatcher`), capability-transferring IPC (`ChannelDispatcher` + `MessagePacket`), virtual memory management (`VmoDispatcher` + `VmarDispatcher`), task hierarchy (`JobDispatcher`, `ProcessDispatcher`, `ThreadDispatcher`), user-space driver primitives (`ResourceDispatcher`, `InterruptDispatcher`, physical & contiguous DMA VMOs), `BOOTFS` `userboot` handoff (`user/userboot/userboot.c`), and `ps` / `vmaps` diagnostics (`user/shell/shell.c`).

3. **[3] Zhao Jiong (赵炯), *A Heavily Commented Linux Kernel Source Code (Linux内核完全注释, V5.0 — Based on Linux 0.12)*, China Machine Press.**
   The direct structural and pedagogical inspiration for this book. Its chapter progression—moving from hardware fundamentals and boot assembly through core subsystems, build/packaging tools, emulator debugging labs, and quick-reference appendices—as well as its interleaved line-by-line source commentary format, guided the organization of all eighteen files in this volume.

4. **[4] John Lions, *Lions' Commentary on UNIX 6th Edition, with Source Code*, Peer-to-Peer Communications, 1977 (reprinted 1996).**
   The pioneering classic that demonstrated an entire real-world operating system kernel (~9,000 lines of PDP-11 C and assembly) could be taught completely, without hand-waving, by pairing clean engineering prose with every single line of source code. ABOS Lesson 20 (~6,300 lines across `EL1` and `EL0`) carries John Lions' completeness ethic forward into the 64-bit capability microkernel era.

5. **[5] Andrew S. Tanenbaum and Albert S. Woodhull, *Operating Systems: Design and Implementation* (3rd Edition), Prentice Hall, 2006.**
   Classic exposition of microkernel architecture (illustrated through MINIX 3) in which device drivers, file servers, and terminal servers execute as isolated user-mode processes communicating via message passing rather than supervisor-mode function calls—directly mirrored by ABOS's standalone `EL0` `console-srv` and `fs-srv` servers.

6. **[6] Maurice J. Bach, *The Design of the UNIX Operating System*, Prentice-Hall, 1986.**
   Foundational treatment of process lifecycles, context switching, sleep/wakeup synchronization races, demand-paged virtual memory regions, and terminal line disciplines—useful background for contrasting traditional monolithic UNIX `fork()`/`exec()`/`sbrk()` semantics against ABOS's capability-explicit `sys_process_create()` + `sys_vmar_map()` + `sys_process_start()` pipeline.

7. **[7] Jochen Liedtke, *"On μ-Kernel Construction"*, in *Proceedings of the 15th ACM Symposium on Operating Systems Principles (SOSP '95)*, pp. 237–250, December 1995.**
   The landmark L4 microkernel paper articulating Liedtke's minimality principle: *"A concept is tolerated inside the μ-kernel only if moving it outside the kernel, i.e., permitting competing implementations, would prevent the implementation of the system's required functionality."* Explains why ABOS retains only address spaces (`VmAspace`), threads/scheduling, and capability IPC/interrupt forwarding in `EL1`, pushing UART, VirtIO-Blk, TAR filesystems, and virtual terminals into `EL0`.

8. **[8] Brian W. Kernighan and Dennis M. Ritchie, *The C Programming Language* (2nd Edition), Prentice Hall, 1988.**
   Canonical reference for freestanding ANSI/ISO C idioms, fixed-layout structs, bitwise capability masks, intrusive doubly linked lists (`ab_wait_observer_t`), variadic formatting (`<stdarg.h>` in `dprintf` and `u_vsnprintf`), and minimal C runtime libraries (`user/ulibc/`).

9. **[9] ARM Limited, *ARM PrimeCell UART (PL011) Technical Reference Manual* (ARM DDI 0183).**
   Hardware register specification for the AMBA PL011 serial controller mapped at physical address `0x09000000` on QEMU `virt`. Defines `UARTDR` (`0x000`), `UARTFR` (`0x018`, `RXFE`/`TXFF` FIFO flags), `UARTLCR_H` (`0x02C`, 8N1 + 16-byte FIFO enable), `UARTIMSC` (`0x038`, `RXIM` + `RTIM` interrupt masks), and `UARTICR` (`0x044`, interrupt clear) programmed directly from `EL0` in `user/drivers/console_srv.c` and from `EL1` in `kernel/dev/debuglog.c`.

10. **[10] OASIS Open, *Virtual I/O Device (VIRTIO) Version 1.0 Specification* (Committee Specification 04), March 2016.**
    Defines the VirtIO-MMIO transport register layout (`0x0a000000`, magic `0x74726976` `"virt"`, legacy/v1 queue setup `QUEUE_SEL`, `QUEUE_NUM`, `QUEUE_ALIGN`, `QUEUE_PFN`, `QUEUE_NOTIFY`, `INTERRUPT_STAT`/`ACK`), split-virtqueue 3-descriptor chaining (`virtq_desc_t`, `virtq_avail_t`), and the VirtIO Block Device (`DEVICE_ID = 2`, `virtio_blk_req_t`) implemented in `EL0` by `user/drivers/fs_srv.c`.

11. **[11] ARM Limited, *ARM Generic Interrupt Controller Architecture Specification, Version 2.0* (ARM IHI 0048B) & *ARM Power State Coordination Interface* (ARM DEN 0022D).**
    Specifies the GICv2 Distributor (`GICD_CTLR`, `GICD_ISENABLER`, `GICD_ICENABLER`, `GICD_ITARGETSR`) and CPU Interface (`GICC_CTLR`, `GICC_PMR`, `GICC_IAR`, `GICC_EOIR`) driven by `kernel/dev/gic.c`, as well as the `PSCI_CPU_ON` (`0xC4000003`) `HVC #0` firmware call used in `kernel/core/main.c` to wake secondary SMP cores `1..3`.

---

# Appendix

## A1 Complete ABOS System Call Table (1–34)

### A1.1 AArch64 `SVC #0` System Call ABI Summary

All 34 ABOS system calls cross the `EL0 -> EL1` boundary via the `__ab_syscall4()` veneer in `user/ulibc/ulibc.c` (lines 5–17), trapped by `arm64_sync_exception()` (`EC == 0x15`) and routed by `syscall_dispatch(iframe_t *frame)` in `kernel/core/syscalls.c`:

* **System call number register**: `x8` (`frame->r[8]`, valid range `1..34`).
* **Argument registers**: `x0`, `x1`, `x2`, `x3` (`frame->r[0..3]`). System calls requiring more than four logical parameters pack extended fields into a stack-allocated C argument struct on the `EL0` stack and pass its pointer in `x1` or `x2` (see Table A1-2).
* **Return register**: `x0` (`frame->r[0]`), returning an `ab_status_t` (`0` = `AB_OK`, negative = `AB_ERR_*`; `SYS_PROCESS_EXIT` never returns).
* **Memory safety invariant**: The kernel *never* dereferences raw `EL0` pointers directly. Every user pointer is range-checked against `[0x00001000, 0x80000000)`, demand-faulted if backed by a lazy VMAR mapping, verified for `PTE_AP_USER`, write permission (`!PTE_AP_RO` on `copy_to_user`), and `PTE_ATTR_NORMAL` DRAM attributes (rejecting Device MMIO pages!), and accessed exclusively through the kernel's higher-half `paddr_to_physmap(pa)` alias.

### A1.2 Master System Call Reference (`1` – `34`)

| # | Symbolic Constant (`include/syscalls.h`) | User-Space `ulibc` Wrapper Prototype (`user/ulibc/ulibc.h`) | Required Object Type & Capability Rights | Functional Summary |
| :-: | :--- | :--- | :--- | :--- |
| **1** | `SYS_HANDLE_CLOSE` | `ab_status_t sys_handle_close(ab_handle_t h)` | Any valid handle (`AB_RIGHT_NONE`) | Closes `h` in caller's handle table, increments slot generation, fires `on_zero_handles` if `handle_count == 0`, and drops ref. |
| **2** | `SYS_HANDLE_DUPLICATE` | `ab_status_t sys_handle_duplicate(ab_handle_t h, ab_rights_t rights, ab_handle_t *out_h)` | Any object: `AB_RIGHT_DUPLICATE` (`rights` subset or `AB_RIGHT_SAME_RIGHTS`) | Creates a second handle referring to the same kernel object with identical or strictly attenuated rights. |
| **3** | `SYS_OBJECT_GET_INFO` | `ab_status_t sys_object_get_info(ab_handle_t h, ab_handle_basic_info_t *out_info)` | Any object: `AB_RIGHT_INSPECT` | Populates `ab_handle_basic_info_t` (`koid`, `related_koid`, `type`, `rights`, `handle_count`). |
| **4** | `SYS_DEBUGLOG_WRITE` | `ab_status_t sys_debuglog_write(ab_handle_t log_h, uint32_t flags, const char *msg, size_t len)` | `LOG` (`10`): `AB_RIGHT_WRITE` (`len <= 256`) | Emits an interrupt-safe kernel diagnostic line tagged with CPU ID, process name, and `LOG` `koid`. |
| **5** | `SYS_PROCESS_EXIT` | `void sys_process_exit(int64_t retcode)` | None (operates on calling process) | Unmaps all VMARs, closes all remaining handles, asserts `AB_SIGNAL_TASK_TERMINATED`, and terminates caller. |
| **6** | `SYS_NANOSLEEP` | `ab_status_t sys_nanosleep(uint64_t ns)` | None | Blocks calling thread on the timer sleep queue for `ns` nanoseconds (`thread_sleep_ns`). |
| **7** | `SYS_EVENT_CREATE` | `ab_status_t sys_event_create(uint32_t options, ab_handle_t *out_h)` | None (mints `0x0d03` `DEFAULT_EVENT`) | Allocates a standalone `EventDispatcher` (`AB_OBJ_TYPE_EVENT`) for thread/port signaling. |
| **8** | `SYS_EVENTPAIR_CREATE` | `ab_status_t sys_eventpair_create(uint32_t options, ab_handle_t *out_h0, ab_handle_t *out_h1)` | None (`options == 0`; mints `0x0f03` `DEFAULT_EVENTPAIR`) | Allocates two peer-coupled `EventPairDispatcher` objects; closing one asserts `AB_SIGNAL_PEER_CLOSED` on the other. |
| **9** | `SYS_OBJECT_SIGNAL` | `ab_status_t sys_object_signal(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask)` | Any object: `AB_RIGHT_SIGNAL` (`SIGNALED \| USER_SIGNAL_0..3` only) | Atomically clears/sets user-permitted signals on `h`'s `StateTracker` and wakes matching observers. |
| **10** | `SYS_OBJECT_SIGNAL_PEER` | `ab_status_t sys_object_signal_peer(ab_handle_t h, ab_signals_t clear_mask, ab_signals_t set_mask)` | `EVENTPAIR` (`6`): `AB_RIGHT_SIGNAL_PEER` | Atomically clears/sets user-permitted signals on the peer endpoint of `h`. |
| **11** | `SYS_OBJECT_WAIT_ONE` | `ab_status_t sys_object_wait_one(ab_handle_t h, ab_signals_t signals, uint64_t deadline, ab_signals_t *out_observed)` | Any object: `AB_RIGHT_WAIT` | Synchronously blocks until `(cur_signals & signals) != 0` or monotonic `deadline` expires. |
| **12** | `SYS_PORT_CREATE` | `ab_status_t sys_port_create(uint32_t options, ab_handle_t *out_port)` | None (mints `0x080f` `DEFAULT_PORT`) | Creates a 32-packet ring-buffered `PortDispatcher` (`AB_OBJ_TYPE_PORT`) for `O(1)` event multiplexing. |
| **13** | `SYS_PORT_QUEUE` | `ab_status_t sys_port_queue(ab_handle_t port_h, const ab_port_packet_t *pkt)` | `PORT` (`7`): `AB_RIGHT_WRITE` | Enqueues a user-supplied `ab_port_packet_t` onto `port_h` and wakes a waiting port consumer. |
| **14** | `SYS_PORT_WAIT` | `ab_status_t sys_port_wait(ab_handle_t port_h, uint64_t deadline, ab_port_packet_t *out_pkt)` | `PORT` (`7`): `AB_RIGHT_READ` | Dequeues the oldest `ab_port_packet_t` from `port_h`, sleeping until `deadline` if the queue is empty. |
| **15** | `SYS_OBJECT_WAIT_ASYNC` | `ab_status_t sys_object_wait_async(ab_handle_t obj_h, ab_handle_t port_h, uint64_t key, ab_signals_t signals)` | `obj_h` (any): `AB_RIGHT_WAIT`; `port_h` (`PORT`): `AB_RIGHT_WRITE` | Attaches a one-shot async observer on `obj_h` that posts an `AB_PKT_TYPE_SIGNAL_ONE` packet tagged `key` to `port_h`. |
| **16** | `SYS_CHANNEL_CREATE` | `ab_status_t sys_channel_create(uint32_t options, ab_handle_t *out_ch0, ab_handle_t *out_ch1)` | None (`options == 0`; mints `0x0f0f` `DEFAULT_CHANNEL`) | Creates a bidirectional pair of `ChannelDispatcher` endpoints (`AB_OBJ_TYPE_CHANNEL`). |
| **17** | `SYS_CHANNEL_WRITE` | `ab_status_t sys_channel_write(ab_handle_t ch_h, const void *bytes, uint32_t num_bytes, const ab_handle_t *handles, uint32_t num_handles)` | `ch_h` (`CHANNEL`): `AB_RIGHT_WRITE`; each `handles[i]`: `AB_RIGHT_TRANSFER` | Enqueues up to `256` bytes + `8` moved capability handles onto `ch_h`'s peer (removing `handles[]` from caller!). |
| **18** | `SYS_CHANNEL_READ` | `ab_status_t sys_channel_read(ab_handle_t ch_h, void *bytes, uint32_t buf_bytes, ab_handle_t *handles, uint32_t buf_handles, uint32_t *actual_bytes, uint32_t *actual_handles)` | `CHANNEL` (`4`): `AB_RIGHT_READ` | Dequeues the next message from `ch_h` and installs transferred capabilities into the receiver's handle table. |
| **19** | `SYS_VMO_CREATE` | `ab_status_t sys_vmo_create(uint64_t size, uint32_t options, ab_handle_t *out_vmo)` | None (mints `0x0cbf` `DEFAULT_VMO`; `<= 512` KB) | Creates a demand-paged anonymous RAM `VmoDispatcher` (`AB_OBJ_TYPE_VMO`). |
| **20** | `SYS_VMO_READ` | `ab_status_t sys_vmo_read(ab_handle_t vmo_h, void *buf, uint64_t offset, size_t len)` | `VMO` (`3`): `AB_RIGHT_READ` | Copies `len` bytes from `vmo_h` at `offset` into user buffer `buf`. |
| **21** | `SYS_VMO_WRITE` | `ab_status_t sys_vmo_write(ab_handle_t vmo_h, const void *buf, uint64_t offset, size_t len)` | `VMO` (`3`): `AB_RIGHT_WRITE` | Copies `len` bytes from `buf` into `vmo_h` at `offset`, allocating physical frames on demand. |
| **22** | `SYS_VMAR_MAP` | `ab_status_t sys_vmar_map(ab_handle_t vmar_h, uint64_t vmar_offset, ab_handle_t vmo_h, uint64_t vmo_offset, uint64_t len, uint32_t perms, vaddr_t *out_va)` | `vmar_h` (`VMAR`): `MAP`; `vmo_h` (`VMO`): `MAP` + matching `READ`/`WRITE`/`EXECUTE` | Maps `vmo_h` into target `VMAR` (at `0x10000000 + vmar_offset`, or bump-allocated when `vmar_offset == 0`). |
| **23** | `SYS_PROCESS_CREATE` | `ab_status_t sys_process_create(ab_handle_t job_h, const char *name, ab_handle_t *out_proc, ab_handle_t *out_vmar)` | `JOB` (`11`): `AB_RIGHT_MANAGE_PROCESS` | Creates a new isolated `ProcessDispatcher` (with fresh ASID & `TTBR0_EL1`) and root `VmarDispatcher` under `job_h`. |
| **24** | `SYS_THREAD_CREATE` | `ab_status_t sys_thread_create(ab_handle_t proc_h, const char *name, ab_handle_t *out_thr)` | `PROCESS` (`1`): `AB_RIGHT_MANAGE_THREAD` | Creates an unstarted `ThreadDispatcher` (`AB_OBJ_TYPE_THREAD`) bound to `proc_h`. |
| **25** | `SYS_PROCESS_START` | `ab_status_t sys_process_start(ab_handle_t proc_h, ab_handle_t thr_h, uintptr_t entry, uintptr_t sp, ab_handle_t arg_h, uintptr_t arg2)` | `proc_h` (`PROCESS`): `WRITE`; `thr_h` (`THREAD`): `WRITE`; `arg_h`: `TRANSFER` | Allocates 24 KB `EL0` stack (if `sp == 0`), transfers `arg_h` -> child `x0`, sets `arg2` -> `x1`, and starts `thr_h` at `entry`. |
| **26** | `SYS_VMO_CREATE_PHYSICAL` | `ab_status_t sys_vmo_create_physical(ab_handle_t res_h, paddr_t pa, uint64_t size, ab_handle_t *out_vmo)` | `RESOURCE` (`9`): `AB_RIGHT_WRITE` | Wraps physical MMIO range `[pa, pa+size)` into a `Device-nGnRnE` `VmoDispatcher` for `EL0` drivers. |
| **27** | `SYS_VMO_CREATE_CONTIG` | `ab_status_t sys_vmo_create_contiguous(ab_handle_t res_h, uint64_t size, ab_handle_t *out_vmo, paddr_t *out_pa)` | `RESOURCE` (`9`): `AB_RIGHT_WRITE` | Allocates physically contiguous zeroed RAM pages for DMA, returning both `out_vmo` and physical bus address `*out_pa`. |
| **28** | `SYS_INTERRUPT_CREATE` | `ab_status_t sys_interrupt_create(ab_handle_t res_h, uint32_t vector, ab_handle_t *out_irq)` | `RESOURCE` (`9`): `AB_RIGHT_READ` (`vector < 128`) | Binds GICv2 INTID `vector` to a new `InterruptDispatcher` (`AB_OBJ_TYPE_INTERRUPT`) and enables it in `GICD`. |
| **29** | `SYS_INTERRUPT_WAIT` | `ab_status_t sys_interrupt_wait(ab_handle_t irq_h)` | `INTERRUPT` (`8`): `AB_RIGHT_WAIT` | Synchronously blocks calling `EL0` driver thread until hardware IRQ `irq_h` fires. |
| **30** | `SYS_INTERRUPT_ACK` | `ab_status_t sys_interrupt_ack(ab_handle_t irq_h)` | `INTERRUPT` (`8`): `AB_RIGHT_WRITE` | Clears `AB_SIGNAL_SIGNALED` on `irq_h` and unmasks `irq_num` at `GICD` for the next interrupt cycle. |
| **31** | `SYS_OBJECT_SET_NAME` | `ab_status_t sys_object_set_name(ab_handle_t h, const char *name)` | Any valid `VMO`/`PROCESS`/`THREAD`/`JOB` handle (`AB_RIGHT_NONE`) | Sets the 31-character diagnostic name shown by `ps` and `vmaps`. |
| **32** | `SYS_JOB_GET_PS_INFO` | `ab_status_t sys_job_get_ps_info(ab_handle_t job_h, ab_task_info_entry_t *entries, uint32_t max_entries, uint32_t *actual_out)` | `JOB` (`11`): `AB_RIGHT_INSPECT` | Exports task hierarchy, PSS/Private/Shared memory stats, and per-type handle counts under `job_h`. |
| **33** | `SYS_PROCESS_GET_MAPS` | `ab_status_t sys_process_get_maps(ab_handle_t job_h, ab_koid_t proc_koid, ab_info_maps_t *maps, uint32_t max_maps, uint32_t *actual_out)` | `JOB` (`11`): `AB_RIGHT_INSPECT` | Exports `/A`, `/R`, ` R`, and ` M` virtual memory mappings for `proc_koid` (`0` = caller). |
| **34** | `SYS_VMAR_UNMAP` | `ab_status_t sys_vmar_unmap(ab_handle_t vmar_h, vaddr_t addr, uint64_t len)` | `VMAR` (`12`): `AB_RIGHT_MAP` | Tears down mapping `[addr, addr+len)`, flushes ASID-tagged TLB entries, and drops backing VMO reference. |

### A1.3 Packed Argument Descriptors Used by Multi-Parameter Syscalls

Because `__ab_syscall4` passes four register arguments (`x0..x3`), eight system calls marshal extended parameter lists through ABI structs defined in `include/object.h`:

| Syscall # & Name | Register Mapping (`x0`, `x1`, `x2`) | Marshaled C Struct (`include/object.h`) |
| :--- | :--- | :--- |
| **17 `SYS_CHANNEL_WRITE`** | `x0 = ch_h`, `x1 = 0`, `x2 = &args` | `ab_channel_write_args_t { bytes, num_bytes, handles, num_handles }` |
| **18 `SYS_CHANNEL_READ`** | `x0 = ch_h`, `x1 = 0`, `x2 = &args` | `ab_channel_read_args_t { bytes, handles, max_bytes, max_handles, actual_bytes, actual_handles }` |
| **22 `SYS_VMAR_MAP`** | `x0 = vmar_h`, `x1 = vmar_offset`, `x2 = &args` | `ab_vmar_map_args_t { vmo_handle, vmo_offset, len, map_flags, mapped_va_out }` |
| **23 `SYS_PROCESS_CREATE`** | `x0 = job_h`, `x1 = &args` | `ab_process_create_args_t { name, name_len, proc_handle_out, vmar_handle_out }` |
| **25 `SYS_PROCESS_START`** | `x0 = proc_h`, `x1 = &args` | `ab_process_start_args_t { thread_handle, entry_pc, user_sp, arg_handle, arg2 }` |
| **26 `SYS_VMO_CREATE_PHYSICAL`** | `x0 = res_h`, `x1 = &args` | `ab_vmo_create_phys_args_t { paddr, size, vmo_out }` |
| **27 `SYS_VMO_CREATE_CONTIG`** | `x0 = res_h`, `x1 = &args` | `ab_vmo_create_contig_args_t { size, vmo_out, paddr_out }` |
| **33 `SYS_PROCESS_GET_MAPS`** | `x0 = job_h`, `x1 = &args` | `ab_process_get_maps_args_t { target_proc_koid, maps_out, max_maps, actual_out }` |

---

## A2 Handle Encoding, Capability Rights, Object Signals and Error Codes

### A2.1 32-Bit Capability Handle Encoding (`ab_handle_t`)

In `kernel/core/object.c` (`make_handle_value()` and `decode_handle_value()`, lines 186–197), every non-zero user-space handle is a 32-bit opaque token protecting against forged indices and ABA use-after-close bugs:

```text
  31      28 27      24 23                              8 7              0
 +----------+----------+---------------------------------+----------------+
 |   0x5    |   0x0    |      16-Bit Generation          |   8-Bit Slot   |
 +----------+----------+---------------------------------+----------------+
   Magic Tag  Reserved   Starts at 1; increments mod 64K   Index 0 .. 63
                         on every handle_close()           in proc->handles
```

* **Special sentinel constants** (`include/object.h` lines 25–27):
  * `AB_HANDLE_INVALID = 0x00000000U`
  * `AB_KOID_INVALID   = 0ULL`
  * `AB_KOID_KERNEL    = 1ULL` (used for `'userboot-rodata'` kernel image projection; dynamic dispatcher KOIDs start at `1024`)
  * `MAX_HANDLES_PER_PROCESS = 64`, `CHANNEL_MAX_MSG_BYTES = 256`, `CHANNEL_MAX_MSG_HANDLES = 8`, `PORT_QUEUE_CAPACITY = 32`, `VMO_MAX_PAGES = 128`.

### A2.2 Kernel Object Types (`ab_obj_type_t`) and Default Rights Bundles

| Value | Enumerator (`include/object.h`) | `ps` Column | Default Rights Macro | Exact Hex Value |
| :---: | :--- | :---: | :--- | :---: |
| `0` | `AB_OBJ_TYPE_NONE` | — | `AB_RIGHT_NONE` | `0x00000000` |
| `1` | `AB_OBJ_TYPE_PROCESS` | `#pr` | `AB_RIGHTS_DEFAULT_PROCESS` | `0x00002C0F` |
| `2` | `AB_OBJ_TYPE_THREAD` | `#th` | `AB_RIGHTS_DEFAULT_THREAD` | `0x00000C0F` |
| `3` | `AB_OBJ_TYPE_VMO` | `#vo` | `AB_RIGHTS_DEFAULT_VMO` | `0x00000CBF` |
| `4` | `AB_OBJ_TYPE_CHANNEL` | `#ch` | `AB_RIGHTS_DEFAULT_CHANNEL` | `0x00000F0F` |
| `5` | `AB_OBJ_TYPE_EVENT` | `#ev` | `AB_RIGHTS_DEFAULT_EVENT` | `0x00000D03` |
| `6` | `AB_OBJ_TYPE_EVENTPAIR` | `#ev` | `AB_RIGHTS_DEFAULT_EVENTPAIR` | `0x00000F03` |
| `7` | `AB_OBJ_TYPE_PORT` | `#po` | `AB_RIGHTS_DEFAULT_PORT` | `0x0000080F` |
| `8` | `AB_OBJ_TYPE_INTERRUPT` | `#ir` | `AB_RIGHTS_DEFAULT_INTERRUPT` | `0x00000D0F` |
| `9` | `AB_OBJ_TYPE_RESOURCE` | `#rs` | `AB_RIGHTS_DEFAULT_RESOURCE` | `0x00000C1F` |
| `10` | `AB_OBJ_TYPE_LOG` | `#lg` | `AB_RIGHTS_DEFAULT_LOG` | `0x00000C0B` |
| `11` | `AB_OBJ_TYPE_JOB` | `#jb` | `AB_RIGHTS_DEFAULT_JOB` | `0x00003C0F` |
| `12` | `AB_OBJ_TYPE_VMAR` | `#vm` | `AB_RIGHTS_DEFAULT_VMAR` | `0x00000C3F` |
| `13` | `AB_OBJ_TYPE_COUNT` | — | *(Array dimension sentinel)* | — |

### A2.3 Capability Rights Bitmask (`ab_rights_t`) & VM Mapping Permissions

| Constant (`include/object.h`) | Bit Expression | Hex Mask | Meaning |
| :--- | :---: | :---: | :--- |
| `AB_RIGHT_NONE` | `0U` | `0x00000000` | Zero capability rights |
| `AB_RIGHT_DUPLICATE` | `1U << 0` | `0x00000001` | May duplicate handle via `sys_handle_duplicate` |
| `AB_RIGHT_TRANSFER` | `1U << 1` | `0x00000002` | May send handle over `sys_channel_write` / `sys_process_start` |
| `AB_RIGHT_READ` | `1U << 2` | `0x00000004` | May read data/packets (`VMO`, `CHANNEL`, `PORT`, `RESOURCE` IRQ) |
| `AB_RIGHT_WRITE` | `1U << 3` | `0x00000008` | May write/modify (`VMO`, `CHANNEL`, `PORT`, `LOG`, `INTERRUPT` ack) |
| `AB_RIGHT_EXECUTE` | `1U << 4` | `0x00000010` | May map `VMO` / `VMAR` with `EL0` instruction execution (`AB_VM_PERM_EXECUTE`) |
| `AB_RIGHT_MAP` | `1U << 5` | `0x00000020` | May map `VMO` into a `VMAR` or unmap a `VMAR` range |
| `AB_RIGHT_GET_PROPERTY` | `1U << 6` | `0x00000040` | Reserved for property queries (Zircon parity) |
| `AB_RIGHT_SET_PROPERTY` | `1U << 7` | `0x00000080` | Included in `AB_RIGHTS_DEFAULT_VMO` |
| `AB_RIGHT_SIGNAL` | `1U << 8` | `0x00000100` | May assert/clear signals via `sys_object_signal` |
| `AB_RIGHT_SIGNAL_PEER` | `1U << 9` | `0x00000200` | May signal peer endpoint via `sys_object_signal_peer` |
| `AB_RIGHT_WAIT` | `1U << 10` | `0x00000400` | May block or register async wait (`wait_one`, `wait_async`, `interrupt_wait`) |
| `AB_RIGHT_INSPECT` | `1U << 11` | `0x00000800` | May inspect metadata (`object_get_info`, `job_get_ps_info`, `process_get_maps`) |
| `AB_RIGHT_MANAGE_PROCESS` | `1U << 12` | `0x00001000` | May create child processes in a `JOB` (`sys_process_create`) |
| `AB_RIGHT_MANAGE_THREAD` | `1U << 13` | `0x00002000` | May create threads in a `PROCESS` (`sys_thread_create`) |
| `AB_RIGHT_SAME_RIGHTS` | `1U << 31` | `0x80000000` | Sentinel telling `sys_handle_duplicate` to preserve existing rights |
| `AB_RIGHTS_BASIC` | `0\|1\|10\|11` | `0x00000C03` | `TRANSFER \| DUPLICATE \| WAIT \| INSPECT` |
| `AB_RIGHTS_IO` | `2\|3` | `0x0000000C` | `READ \| WRITE` |

**VMAR Mapping Permission Flags (`sys_vmar_map` `perms`)**:
* `AB_VM_PERM_READ = 0x1` (`1U << 0`), `AB_VM_PERM_WRITE = 0x2` (`1U << 1`), `AB_VM_PERM_EXECUTE = 0x4` (`1U << 2`).

### A2.4 Object Signals (`ab_signals_t`), Port Packets, and Deadlines

| Constant (`include/object.h`) | Bit / Value | Hex Value | Asserted / Cleared By |
| :--- | :---: | :---: | :--- |
| `AB_SIGNAL_NONE` | `0U` | `0x00000000` | Initial empty signal mask |
| `AB_SIGNAL_READABLE` | `1U << 0` | `0x00000001` | Kernel: `Channel` has queued messages (`msg_count > 0`) |
| `AB_SIGNAL_WRITABLE` | `1U << 1` | `0x00000002` | Kernel: `Channel` peer is open and can accept writes |
| `AB_SIGNAL_PEER_CLOSED` | `1U << 2` | `0x00000004` | Kernel: `Channel` or `EventPair` peer reached 0 handles |
| `AB_SIGNAL_SIGNALED` | `1U << 3` | `0x00000008` | User `sys_object_signal` (`Event`) or Kernel (`Interrupt` fired) |
| `AB_SIGNAL_TASK_TERMINATED` | `1U << 3` | `0x00000008` | Alias of `AB_SIGNAL_SIGNALED`; set on `Process`/`Thread` exit |
| `AB_USER_SIGNAL_0` | `1U << 24` | `0x01000000` | User-defined application/IPC custom signal 0 |
| `AB_USER_SIGNAL_1` | `1U << 25` | `0x02000000` | User-defined application/IPC custom signal 1 |
| `AB_USER_SIGNAL_2` | `1U << 26` | `0x04000000` | User-defined application/IPC custom signal 2 |
| `AB_USER_SIGNAL_3` | `1U << 27` | `0x08000000` | User-defined application/IPC custom signal 3 |
| `AB_USER_SIGNAL_ALL` | `bits 27:24` | `0x0F000000` | Union of `AB_USER_SIGNAL_0 .. AB_USER_SIGNAL_3` |
| `AB_PKT_TYPE_USER` | `0U` | `0x00000000` | Port packet queued explicitly via `sys_port_queue` |
| `AB_PKT_TYPE_SIGNAL_ONE` | `1U` | `0x00000001` | Port packet posted by `sys_object_wait_async` observer |
| `AB_TIME_INFINITE` | `UINT64_MAX` | `0xFFFFFFFFFFFFFFFF` | Never time out (`include/wait.h`) |

### A2.5 Status and Error Codes (`ab_status_t`)

| Symbolic Name (`include/object.h`) | Decimal | 32-Bit Hex (`w0`) | Meaning |
| :--- | :---: | :---: | :--- |
| `AB_OK` | `0` | `0x00000000` | Operation succeeded |
| `AB_ERR_INTERNAL` | `-1` | `0xFFFFFFFF` | Unspecified internal kernel error |
| `AB_ERR_NOT_SUPPORTED` | `-2` | `0xFFFFFFFE` | Unknown syscall number or unsupported object operation |
| `AB_ERR_NO_RESOURCES` | `-3` | `0xFFFFFFFD` | Handle table (`64`) or port/mapping table full |
| `AB_ERR_NO_MEMORY` | `-4` | `0xFFFFFFFC` | Kernel heap (`kmalloc`) or physical page (`pmm`) exhaustion |
| `AB_ERR_INVALID_ARGS` | `-10` | `0xFFFFFFF6` | Null/unmapped user pointer, invalid flags, or rights escalation attempt |
| `AB_ERR_BAD_HANDLE` | `-11` | `0xFFFFFFF5` | Handle is `0`, lacks `0x5` tag, out-of-range slot, or stale generation |
| `AB_ERR_WRONG_TYPE` | `-12` | `0xFFFFFFF4` | Valid handle points to wrong `ab_obj_type_t` (e.g., `EVENT` passed as `CHANNEL`) |
| `AB_ERR_BAD_SYSCALL` | `-13` | `0xFFFFFFF3` | Malformed system call invocation |
| `AB_ERR_OUT_OF_RANGE` | `-14` | `0xFFFFFFF2` | Offset/length/IRQ/message size exceeds object or protocol bounds |
| `AB_ERR_BUFFER_TOO_SMALL` | `-15` | `0xFFFFFFF1` | `sys_channel_read` caller buffer smaller than head message (`actual_*` updated) |
| `AB_ERR_BAD_STATE` | `-20` | `0xFFFFFFEC` | Object in wrong lifecycle state (e.g., starting an already-started process) |
| `AB_ERR_TIMED_OUT` | `-21` | `0xFFFFFFEB` | Monotonic `deadline` expired before watched signal/packet arrived |
| `AB_ERR_SHOULD_WAIT` | `-22` | `0xFFFFFFEA` | Non-blocking `sys_channel_read` found `0` queued messages while peer is open |
| `AB_ERR_CANCELED` | `-23` | `0xFFFFFFE9` | Pending wait canceled because handle/object was closed |
| `AB_ERR_PEER_CLOSED` | `-24` | `0xFFFFFFE8` | Write/read/signal attempted on a `Channel`/`EventPair` whose peer closed |
| `AB_ERR_NOT_FOUND` | `-25` | `0xFFFFFFE7` | Requested KOID (`vmaps`) or TAR filename (`fs-srv`) does not exist |
| `AB_ERR_ACCESS_DENIED` | `-30` | `0xFFFFFFE2` | Handle lacks required `AB_RIGHT_*` bit or user buffer points to RO/MMIO page |

### A2.6 Bootstrap Processargs (`AB_PA_*`) and User-Space IPC Opcodes (`include/ipc_proto.h`)

* **`ab_proc_args_t` Header**: `AB_PROC_ARGS_MAGIC = 0x41425041U` (ASCII `"ABPA"`), followed by `uint8_t handle_types[8]`:
  * `AB_PA_NONE (0)`, `AB_PA_LOG (1)`, `AB_PA_VMAR_ROOT (2)`, `AB_PA_CONSOLE (3)`, `AB_PA_FS (4)`, `AB_PA_JOB_DEFAULT (5)`, `AB_PA_RESOURCE (6)`, `AB_PA_CHANNEL_SRV (7)`.
* **Console Server Protocol (`console_msg_t`, `NUM_VTS = 4`)**:
  * `CON_OP_WRITE (1)`, `CON_OP_READ_LINE (2)`, `CON_OP_SHUTDOWN (3)`, `CON_OP_SWITCH_VT (4)`.
* **Filesystem Server Protocol (`fs_msg_t`)**:
  * `FS_OP_LIST (1)`, `FS_OP_OPEN_VMO (2)`, `FS_OP_SHUTDOWN (3)`.

---

## A3 ARMv8-A Exception Syndrome (ESR_EL1) and 4 KB PTE Bitfield Quick Reference

### A3.1 `ESR_EL1` Register Layout and Exception Classes (`EC`)

When a synchronous exception traps into `EL1`, the Cortex-A53 hardware populates `ESR_EL1` as follows (`kernel/core/exceptions_c.c` lines 23–24):

```text
  31          26 25 24                                                0
 +--------------+--+---------------------------------------------------+
 |  EC [31:26]  |IL|                    ISS [24:0]                     |
 +--------------+--+---------------------------------------------------+
   6-bit Class   1=32b insn    25-bit Instruction-Specific Syndrome
```

| `EC` (Hex) | `EC` (Bin) | Architectural Origin | Handling in ABOS (`arm64_sync_exception`) |
| :---: | :---: | :--- | :--- |
| `0x00` | `000000` | Unknown reason / `udf #0` | Terminates `EL0` process (`-1`) or `EL1` `[PANIC]` |
| `0x07` | `000111` | Trapped SVE / Advanced SIMD / FP access | Prevented by `CPACR_EL1.FPEN = 0b11` + `-mgeneral-regs-only` |
| `0x0E` | `001110` | Illegal Execution State (`PSTATE.IL == 1`) | Terminates `EL0` process or `EL1` `[PANIC]` |
| `0x15` | `010101` | **`SVC` instruction executed in AArch64 (`svc #0`)** | Enables IRQs and calls `syscall_dispatch(iframe)`! |
| `0x18` | `011000` | Trapped `MSR`, `MRS`, or System instruction in `EL0` | Terminates faulting `EL0` process (privilege violation) |
| `0x20` | `100000` | **Instruction Abort from a lower EL (`EL0`)** | Checks `FSC`; runs VMO demand-paging (`is_exec=1`) or kills `EL0` proc |
| `0x21` | `100001` | Instruction Abort taken without a change in EL (`EL1`) | Unhandled `EL1` `[PANIC]` |
| `0x22` | `100010` | `PC` Alignment Fault | Terminates `EL0` process or `EL1` `[PANIC]` |
| `0x24` | `100100` | **Data Abort from a lower EL (`EL0`)** | Checks `FSC` & `WnR`; runs VMO demand-paging or kills `EL0` proc |
| `0x25` | `100101` | Data Abort taken without a change in EL (`EL1`) | Unhandled `EL1` `[PANIC]` (note `copy_from_user` walks PTEs safely!) |
| `0x26` | `100110` | `SP` Alignment Fault (`SP` not 16-byte aligned) | Terminates `EL0` process or `EL1` `[PANIC]` |
| `0x30`/`0x31` | `11000x` | Hardware Breakpoint (`hbreak` in GDB) | Intercepted by QEMU GDB stub when `-s` is active |
| `0x3C` | `111100` | `BRK` software breakpoint instruction (`break` in GDB) | Intercepted by QEMU GDB stub when `-s` is active |

### A3.2 Instruction & Data Abort `ISS` Bitfields (`EC = 0x20, 0x21, 0x24, 0x25`)

* **Bit `[10]` (`FnV`)**: `FAR` not Valid (`0` = `FAR_EL1` holds exact faulting virtual address).
* **Bit `[6]` (`WnR`, Data Aborts only)**: Write not Read (`0` = Fault caused by memory read/load; `1` = Fault caused by memory write/store).
* **Bits `[5:0]` (`DFSC` / `IFSC` — Fault Status Code)**:

| `FSC` (Hex) | Fault Type & Translation Level | Demand-Paging Note in `exceptions_c.c` |
| :---: | :--- | :--- |
| `0x04` – `0x07` | **Translation Fault**, Level 0 (`0x04`), L1 (`0x05`), L2 (`0x06`), L3 (`0x07`) | Matched by `(fsc & 0x3CU) == 0x04U`! Invokes `ab_process_handle_page_fault()`. |
| `0x09` – `0x0B` | **Access Flag Fault**, Level 1 (`0x09`), L2 (`0x0A`), L3 (`0x0B`) | Avoided in ABOS because `flags_to_pte()` pre-sets `PTE_AF` (`1 << 10`). |
| `0x0D` – `0x0F` | **Permission Fault**, Level 1 (`0x0D`), L2 (`0x0E`), L3 (`0x0F`) | E.g., writing to an `r--` mapped VMO (`cat` file) or executing `rw-` stack (`UXN=1`); kills `EL0` proc! |
| `0x21` | **Alignment Fault** (e.g., unaligned access to `Device-nGnRnE` MMIO) | Device MMIO requires natural alignment; terminates faulting `EL0` driver. |

### A3.3 ARMv8-A Stage-1 4 KB Page Descriptor (`pte_t`) & MMU System Registers

```text
  63     55 54  53  52      48 47                     12 11 10 9  8 7  6  5  4    2  1  0
 +---------+---+---+----------+-------------------------+--+--+----+----+---+------+--+--+
 | Ignored |UXN|PXN| Reserved | Output Phys Addr [47:12]|nG|AF| SH | AP |NS |AttrIx| 1| 1|
 +---------+---+---+----------+-------------------------+--+--+----+----+---+------+--+--+
            54  53             PTE_ADDR_MASK             11 10 9..8 7..6     4..2   Pg V
```

| Bit(s) | Macro in `include/mmu.h` | Hex Value | Architectural Meaning in ABOS |
| :---: | :--- | :---: | :--- |
| `[0]` | `PTE_VALID` | `0x0000000000000001` | Descriptor is valid (`0` triggers Translation Fault `FSC 0x05..0x07`). |
| `[1]` | `PTE_TABLE` / `PTE_PAGE` | `0x0000000000000002` | `1` = Next-level table pointer (at L1/L2) or 4 KB Page (at L3); `0` at L1 = 1 GB Block! |
| `[4:2]` | `PTE_ATTR_DEVICE` (`0<<2`) | `0x0000000000000000` | `MAIR_EL1` Attr0 (`0x00`) = **Device-nGnRnE** (strongly ordered, non-cacheable MMIO). |
| `[4:2]` | `PTE_ATTR_NORMAL` (`1<<2`) | `0x0000000000000004` | `MAIR_EL1` Attr1 (`0xFF`) = **Normal Inner/Outer Write-Back Write-Allocate** DRAM. |
| `[6]` | `PTE_AP_USER` | `0x0000000000000040` | `AP[1]=1`: Accessible from unprivileged `EL0` (`0` = `EL1` supervisor only). |
| `[7]` | `PTE_AP_RO` | `0x0000000000000080` | `AP[2]=1`: Read-Only (`0` = Read/Write). |
| `[9:8]` | `PTE_SH_INNER` (`3<<8`) | `0x0000000000000300` | Inner Shareable across all 4 Cortex-A53 SMP cores. |
| `[10]` | `PTE_AF` | `0x0000000000000400` | Access Flag (`1` = hardware will not raise Access Flag Fault on first touch). |
| `[11]` | `PTE_NG` | `0x0000000000000800` | **Non-Global**: tags TLB entry with `TTBR0_EL1[63:48]` ASID (`0` on `TTBR1_EL1` kernel blocks = Global). |
| `[47:12]` | `PTE_ADDR_MASK` | `0x0000FFFFFFFFF000` | 4 KB-aligned physical page or next-level page-table physical address. |
| `[53]` | `PTE_PXN` | `0x0020000000000000` | **Privileged Execute-Never**: **always `1` on all `EL0` pages** (SMEP equivalent—`EL1` can never execute user memory!). |
| `[54]` | `PTE_UXN` | `0x0040000000000000` | **Unprivileged Execute-Never**: `1` on stacks, heaps, MMIO, and DMA buffers (`W^X` / NX protection). |

* **Key MMU System Register Values in ABOS**:
  * `MAIR_EL1 = 0x000000000000FF00` (`Attr0 = 0x00` Device-nGnRnE, `Attr1 = 0xFF` Normal WBWA).
  * `TCR_EL1 = 0x00000010B5213521` (`AS = 1` 16-bit ASIDs, `IPS = 0b010` 40-bit PA, `T0SZ = 33` 2 GB `TTBR0`, `T1SZ = 33` 2 GB `TTBR1`, `TG0 = 4KB`, `TG1 = 4KB`).
  * `TTBR0_EL1 = ((uint64_t)asid << 48) | l1_phys_addr`.

---

## A4 Terminal Control Characters, VT Hotkeys and CSI Escape Sequences

In Dr. Zhao Jiong's *A Heavily Commented Linux Kernel Source Code*, Appendices A2 and A3 catalog ASCII C0 control characters and VT102 console escape sequences interpreted by `tty_io.c` and `console.c`. In ABOS, terminal line editing, virtual-terminal switching (`tty0`–`tty3`), and CSI keyboard negotiation live entirely in unprivileged `EL0` inside **`user/drivers/console_srv.c`**.

### A4.1 C0 Control Characters and Line-Discipline Bytes (`console_srv.c`)

| Byte (Hex) | Dec | ASCII / Keystroke | Mode / Handler in `console_srv.c` | Exact Runtime Action |
| :---: | :---: | :--- | :--- | :--- |
| `0x00` | `0` | `NUL` (`Ctrl+Space` or `Ctrl+2` / `Ctrl+@`) | `drain_uart_rx` (normal) | **Immediate VT hotkey**: switches active terminal to **`tty2`**. |
| `0x08` | `8` | `BS` (`Backspace` / `Ctrl+H`) | `process_vt_rx` (line edit) | Decrements `vt->line_len` (if `> 0`) and echoes `"\b \b"` to erase character on UART. |
| `0x0A` | `10` | `LF` (`\n` / `Ctrl+J`) | `process_vt_rx` & `el0_uart_putc` | **RX**: Ignored if immediately preceded by `\r` (`last_was_cr`), else completes line and sends `CON_OP_READ_LINE` reply. **TX**: `el0_uart_putc` expands `\n` -> `\r\n`. |
| `0x0D` | `13` | `CR` (`\r` / `Enter` / `Ctrl+M`) | `process_vt_rx` (line edit) | Sets `last_was_cr = 1`, echoes `\r\n`, appends line to `screen_buf`, and delivers line to waiting shell over IPC. |
| `0x0E` | `14` | `SO` (`Ctrl+N`) | `drain_uart_rx` (normal) | **Cycle Next VT hotkey**: switches active VT to `(active_vt + 1) % 4` (`tty0 -> tty1 -> tty2 -> tty3 -> tty0`). |
| `0x14` | `20` | `DC4` (`Ctrl+T`) | `drain_uart_rx` -> `prefix_mode = 1` | **Two-key VT prefix**: awaits next byte `'0'..'3'` to switch to `tty0..tty3`. |
| `0x1B` | `27` | `ESC` (`Escape` or `Ctrl+3` / `Ctrl+[`) | `drain_uart_rx` -> `prefix_mode = 2` | Starts **`25 ms` `ESC_TIMEOUT_NS` timer** (`rx->esc_deadline`). If no byte arrives within 25 ms, standalone `ESC`/`Ctrl+3` switches to **`tty3`**; if followed by `'0'..'3'`, handles `Alt+0..3`; if followed by `'['`, enters CSI parser (`prefix_mode = 3`). |
| `0x1C` | `28` | `FS` (`Ctrl+4` or `Ctrl+\`) | `drain_uart_rx` (normal) | **Immediate VT hotkey**: switches active terminal to **`tty0`**. |
| `0x1D` | `29` | `GS` (`Ctrl+5` or `Ctrl+]`) | `drain_uart_rx` (normal) | **Immediate VT hotkey**: switches active terminal to **`tty1`**. |
| `0x1E` | `30` | `RS` (`Ctrl+6` or `Ctrl+^`) | `drain_uart_rx` (normal) | **Cycle Next VT hotkey**: switches active VT to `(active_vt + 1) % 4`. |
| `0x20..0x7E` | `32..126` | Printable ASCII (except `'^'` in normal mode) | `process_vt_rx` (line edit) | Appended to `vt->line_buf` (up to `VT_LINE_CAP - 1 = 127` chars) and echoed if `is_active`. |
| `0x5E` | `94` | `'^'` (Caret character) | `drain_uart_rx` -> `prefix_mode = 1` | Plain-ASCII prefix fallback (`^0` .. `^3` switches to `tty0..tty3` over web/dumb serial consoles that swallow `Ctrl`/`Alt`). |
| `0x7F` | `127` | `DEL` (Modern `Backspace` key) | `process_vt_rx` (line edit) | Identical to `0x08` (`BS`): erases last character from `line_buf` and echoes `"\b \b"`. |

### A4.2 `rx_parser_t` State Machine and Multi-Byte Escape / CSI Sequences

```text
                       0x14 (Ctrl+T) or '^'
         +--------------------------------------------+
         |                                            v
   +-----+------+     '0'..'3' -> switch VT    +--------------+
   |   Mode 0   |<-----------------------------+    Mode 1    |
   |  (Normal)  |                              | (Prefix ^/T) |
   +-----+------+                              +--------------+
         |   ^
0x1B(ESC)|   | 25ms timeout -> switch tty3
         v   | or '0'..'3' (Alt+0..3) -> switch VT
   +-----+------+                              +--------------+
   |   Mode 2   |----------- '[' ------------->|    Mode 3    |
   | (After ESC)|                              | (CSI Parser) |
   +------------+                              +------+-------+
         ^                                            |
         +------- Final byte 0x40..0x7E ('u' / '~') --+
                  parse_csi_vt_hotkey() -> switch VT (back to Mode 0)
```

| Direction | Escape / CSI Sequence | Hex Bytes | Protocol & Meaning in `console_srv.c` |
| :---: | :--- | :--- | :--- |
| **TX (Init)** | `ESC [ > 4 ; 2 m` | `1B 5B 3E 34 3B 32 6D` | Enables **xterm `modifyOtherKeys` mode 2** on host terminal (line 312) so `Ctrl+0..4` emit unambiguous CSI sequences. |
| **TX (Init)** | `ESC [ > 1 u` | `1B 5B 3E 31 75` | Pushes **Kitty keyboard protocol** disambiguate flag (`1`) on host terminals (kitty, WezTerm, Ghostty, foot, Alacritty). |
| **TX (Exit)** | `ESC [ < u` | `1B 5B 3C 75` | Pops Kitty keyboard protocol stack cleanly on `shutdown` (`exit`, line 399). |
| **TX (Exit)** | `ESC [ > 4 ; 0 m` | `1B 5B 3E 34 3B 30 6D` | Resets xterm `modifyOtherKeys` back to mode `0` before QEMU exits (line 399). |
| **RX (Hotkey)** | `ESC 0` .. `ESC 3` | `1B 30` .. `1B 33` | **`Alt+0` .. `Alt+3`** (Meta key): switches directly to **`tty0` .. `tty3`**. |
| **RX (Hotkey)** | `ESC [ 48 ; 5 u` .. `ESC [ 51 ; 5 u` | `1B 5B 34 38 3B 35 75` .. | **`Ctrl+0` .. `Ctrl+3`** (Kitty / `modifyOtherKeys` CSI-`u`, ASCII `48..51` = `'0'..'3'`, modifier `5` = `Ctrl`): switches to **`tty0` .. `tty3`**. |
| **RX (Hotkey)** | `ESC [ 52 ; 5 u` | `1B 5B 35 32 3B 35 75` | **`Ctrl+4`** (CSI-`u`, ASCII `52` = `'4'`): mapped to **`tty0`**. |
| **RX (Hotkey)** | `ESC [ 27 ; 5 ; 48 ~` .. `51 ~` | `1B 5B 32 37 3B 35 3B ...` | **`Ctrl+0` .. `Ctrl+3`** (xterm `modifyOtherKeys` format 0): switches to **`tty0` .. `tty3`**. |
| **RX (Hotkey)** | `ESC [ 27 ; 5 ; 52 ~` | `1B 5B 32 37 3B 35 3B 35 32 7E` | **`Ctrl+4`** (xterm `modifyOtherKeys` format 0): mapped to **`tty0`**. |

### A4.3 Host QEMU `-serial mon:stdio` Escape Sequences (Intercepted Before Guest UART)

Note that QEMU's `-serial mon:stdio` character multiplexer intercepts **`0x01` (`Ctrl+A`)** on the host **before** bytes reach the emulated PL011 RX FIFO (which is why `console_srv.c` uses `Ctrl+T`, `Ctrl+N`, and `Alt+0..3` rather than `Ctrl+A`!):

* **`Ctrl+A x`**: Quit QEMU immediately (`exit(0)` on host).
* **`Ctrl+A c`**: Switch between guest PL011 UART0 (`abos[ttyN]$ `) and QEMU Monitor (`(qemu)`).
* **`Ctrl+A b`**: Send a serial `BREAK` condition to PL011 UART0.
* **`Ctrl+A h`**: Display QEMU terminal multiplexer help.
* **`Ctrl+A Ctrl+A`**: Forward a single `0x01` byte to the guest UART.
