# ABOS Development Plan: A Step-by-Step Zircon-Inspired Microkernel on AArch64

Welcome to **ABOS**! While **AAOS** built a classic monolithic Unix-style OS (where device drivers like VirtIO-blk, filesystems like TarFS, and TTY consoles live inside the EL1 kernel and user space invokes POSIX-like `read`/`write` syscalls), **ABOS** builds a **capability-based microkernel OS** modeled directly after **Fuchsia's Zircon kernel** (`/usr/local/google/home/lipingliu/80_Zircon/zircon_org`).

In ABOS:
* The kernel is written in clean, readable **freestanding C** (like AAOS), modeling Zircon's two-layer architecture: the low-level **LK (Little Kernel) core** and the high-level **Zircon Object & Capability layer**.
* Almost every resource is a **Kernel Object** (`ab_object_t`, modeling Zircon's `Dispatcher`) referenced by user space through unforgeable **Handles** (`ab_handle_t`) with fine-grained **Rights** (`ab_rights_t`).
* Device drivers (VirtIO block, UART RX/TX console), filesystems, and shells live in **EL0 user space**, communicating via **Channels** (message + handle passing) and shared **VMOs** (Virtual Memory Objects).

---

## Architectural Contrast: AAOS vs. ABOS (Zircon)

| Subsystem | AAOS (Monolithic Kernel) | ABOS (Zircon-Style Microkernel) | `zircon_org` Reference |
| :--- | :--- | :--- | :--- |
| **System Calls** | POSIX-style (`sys_read`, `sys_write`, `sys_exit`, `sys_yield`) | Object/Capability syscalls (`ab_channel_write`, `ab_vmo_map`, `ab_object_wait_one`, ...) | `system/public/zircon/syscalls.abigen` |
| **Access Control** | Implicit (any user thread can call `sys_read`/`sys_write`) | Capability-based: per-process **Handle Table** + bitmask **Rights** (`AB_RIGHT_READ`, `AB_RIGHT_WRITE`, `AB_RIGHT_DUPLICATE`, `AB_RIGHT_TRANSFER`) | `kernel/object/handle.cpp`, `docs/handles.md`, `docs/rights.md` |
| **Memory Model** | Raw page tables per process + lazy fault allocation | **VMO** (Virtual Memory Object) + **VMAR** (Virtual Memory Address Region) supporting zero-copy shared memory | `kernel/vm/vm_object.cpp`, `kernel/object/vm_object_dispatcher.cpp` |
| **Synchronization** | Spinlocks in kernel | LK **Events/WaitQueues** in kernel; **Object Signals** (`AB_SIGNAL_READABLE`, etc.), **WaitOne**, and **Ports** for user space | `kernel/kernel/event.cpp`, `kernel/object/state_tracker.cpp`, `port_dispatcher.cpp` |
| **IPC** | None (shared kernel buffers / TTY) | **Channels** (bidirectional datagrams carrying bytes + **Handles** across processes) & **Sockets** | `kernel/object/channel_dispatcher.cpp` |
| **Bootstrapping** | Kernel parses TAR on VirtIO disk and launches `shell.bin` | Kernel embeds **`userboot`** + RAM disk VMO (`BOOTFS`) and sends bootstrap handles over a `Channel` | `docs/userboot.md`, `kernel/lib/userboot/userboot.cpp` |
| **Device Drivers** | VirtIO-blk, UART, and TTY compiled into EL1 kernel | User-space EL0 drivers using **Interrupt Objects** (`ab_interrupt_wait`) and **Physical MMIO VMOs** (`Resource`) | `kernel/object/interrupt_dispatcher.cpp`, `resource_dispatcher.cpp` |
| **Filesystem** | `fs/tar.c` inside EL1 kernel | User-space **Filesystem Server** serving file requests over `Channel`s and `VMO`s | `docs/concepts.md`, `system/ulib/fs` |

---

## Step-by-Step Curriculum

### Phase 1: Streamlined LK Hardware & Kernel Core (Lessons 01–06)
*Zircon was born as a branch of LK (Little Kernel). Its bottom half (`kernel/arch`, `kernel/top`, `kernel/kernel`, `kernel/vm`) provides hardware bringup, page tables, `thread_t` scheduling, and `event_t` wait queues.*

#### Lesson 00: Architecture, Zircon Study Guide & Toolchain Setup
* **Theory**: Monolithic vs. Capability Microkernel, Zircon's LK + Dispatcher two-layer design, QEMU `virt` memory map, and prebuilt AArch64 toolchains.
* **References**: `zircon_org/docs/concepts.md`, `zircon_org/docs/zx_and_lk.md`, `zircon_org/docs/objects.md`.
* **Goal**: Understand the roadmap and verify our build/QEMU environment.

#### Lesson 01: Bare-Metal Boot, EL3/EL2 $\rightarrow$ EL1 & Early Debuglog (`lk_main`)
* **Theory**: ARMv8 exception levels, early stack setup, PL011 UART `dprintf` / `debuglog`, and PSCI SMP secondary core bringup into `lk_main()`.
* **Practice**:
  * Implement `start.S` to drop from EL3/EL2 to EL1 and initialize BSS/stacks.
  * Implement `lk_main()` staged init banners and bring up all 4 Cortex-A53 cores via PSCI.
* **References**: `zircon_org/kernel/arch/arm64/start.S`, `zircon_org/kernel/top/main.cpp`.
* **Goal**: All 4 cores boot cleanly into `EL1` and log via serialized `dprintf`.

#### Lesson 02: Exception Vectors, GICv2 Interrupts & ARM Generic Timer
* **Theory**: `VBAR_EL1` vector table layout, saving/restoring `iframe_t` (`pt_regs`), GICv2 Distributor/CPU interface, and per-CPU ARM Generic Timer ticks.
* **Practice**:
  * Set up EL1/EL0 exception vectors (`exceptions.S`).
  * Initialize GICv2 and per-core generic timers (`CNTP_TVAL_EL0` / `CNTP_CTL_EL0`).
* **References**: `zircon_org/kernel/arch/arm64/exceptions.S`, `zircon_org/kernel/dev/interrupt/arm_gic/v2/`, `zircon_org/kernel/dev/timer/arm_generic/`.
* **Goal**: Timer interrupts firing reliably on all cores with clean `iframe_t` exception framing.

#### Lesson 03: Device Tree / ZBI Handoff & Physical Memory Arena (`pmm`)
* **Theory**: Discovering RAM topology via DTB (and how Zircon uses ZBI), tracking physical pages with `vm_page_t` metadata and free lists (`pmm_alloc_page`, `pmm_alloc_contiguous`, `pmm_free_page`), plus a basic kernel slab/heap allocator (`kmalloc`/`kfree`) needed for kernel objects.
* **Practice**:
  * Parse DTB memory nodes.
  * Implement the PMM page frame allocator and a lightweight kernel object allocator (`kmalloc`/`kfree`).
* **References**: `zircon_org/kernel/vm/pmm.cpp`, `zircon_org/kernel/vm/pmm_arena.cpp`, `zircon_org/kernel/lib/heap/`.
* **Goal**: Dynamic allocation of physical pages and kernel structs (`kmalloc`/`kfree`).

#### Lesson 04: Virtual Memory & Address Spaces (`VmAspace`: Kernel `TTBR1` & User `TTBR0`)
* **Theory**: 4-level AArch64 page tables, Higher-Half Kernel address space (`TTBR1_EL1` at `0xFFFFFFFF00000000+`), and isolated per-process User address spaces (`TTBR0_EL1`).
* **Practice**:
  * Transition the kernel to run in higher-half virtual memory (`TTBR1_EL1`) with MMIO remapped to kernel VA.
  * Implement `vm_aspace_t` API (`vmm_create_aspace`, `vmm_map_page`, `vmm_switch_aspace`) for switching `TTBR0_EL1` with ASIDs/TLB invalidation.
* **References**: `zircon_org/kernel/arch/arm64/mmu.cpp`, `zircon_org/kernel/vm/vm_aspace.cpp`.
* **Goal**: Kernel running in higher-half VA with clean `vm_aspace_t` abstraction ready for user processes.

#### Lesson 05: LK Threads (`thread_t`), Context Switching & Preemptive Scheduler
* **Theory**: LK's `thread_t` structure, thread states (`THREAD_READY`, `THREAD_RUNNING`, `THREAD_BLOCKED`, `THREAD_SLEEPING`, `THREAD_DEATH`), `arch_context_switch`, per-CPU run queues, and timer-driven preemption.
* **Practice**:
  * Implement `thread_create`, `thread_resume`, `thread_yield`, `thread_sleep`, and `thread_exit`.
  * Connect the timer interrupt to preemptive scheduling across cores.
* **References**: `zircon_org/kernel/kernel/thread.cpp`, `zircon_org/kernel/kernel/sched.cpp`, `zircon_org/kernel/arch/arm64/asm.S`.
* **Goal**: Multiple preemptive LK kernel threads running and sleeping across CPU cores.

#### Lesson 06: LK Wait Queues (`wait_queue_t`) & Events (`event_t`)
* **Theory**: Why spinlocks aren't enough for a microkernel — threads waiting for IPC messages or signals must **block** (`THREAD_BLOCKED`) on a `wait_queue_t` without burning CPU, and wake up when signaled or timed out.
* **Practice**:
  * Implement `wait_queue_block`, `wait_queue_wake_one`, `wait_queue_wake_all`.
  * Implement LK `event_t` (`event_init`, `event_wait_deadline`, `event_signal`, `event_unsignal`).
* **References**: `zircon_org/kernel/kernel/wait.cpp`, `zircon_org/kernel/kernel/event.cpp`.
* **Goal**: Producer and consumer kernel threads synchronizing via blocking `event_t` and `wait_queue_t`.

---

### Phase 2: Zircon Object, Capability & IPC Layer (Lessons 07–11)
*This is where ABOS diverges completely from AAOS and implements the heart of Zircon (`zircon_org/kernel/object`).*

#### Lesson 07: Kernel Objects (`Dispatcher`), KOIDs, Handles & Rights
* **Theory**:
  * Every user-visible resource is an `ab_object_t` (modeling Zircon's `Dispatcher`) with a unique 64-bit `ab_koid_t`, type tag (`AB_OBJ_TYPE_*`), reference count, and virtual destructor (`on_zero_handles` / `destroy`).
  * Processes never touch pointers; they hold 32-bit `ab_handle_t` indices into a per-process **Handle Table**, paired with a bitmask of `ab_rights_t` (`AB_RIGHT_DUPLICATE`, `AB_RIGHT_TRANSFER`, `AB_RIGHT_READ`, `AB_RIGHT_WRITE`, `AB_RIGHT_WAIT`).
* **Practice**:
  * Implement `object.c` and `handle.c`.
  * Drop into EL0 user mode and implement the core handle syscalls: `ab_handle_close`, `ab_handle_duplicate`, `ab_handle_replace`, plus `ab_debuglog_write`.
* **References**: `zircon_org/docs/objects.md`, `zircon_org/docs/handles.md`, `zircon_org/docs/rights.md`, `zircon_org/kernel/object/dispatcher.cpp`, `zircon_org/kernel/object/handle.cpp`.
* **Goal**: An EL0 user program creating an object, duplicating its handle with reduced rights, verifying right-enforcement errors (`AB_ERR_ACCESS_DENIED`), and closing handles to trigger object destruction.

#### Lesson 08: Object Signals, `StateTracker`, `WaitOne` & `Event` / `EventPair` Objects
* **Theory**:
  * Every Zircon object embeds a `StateTracker` maintaining active 32-bit signals (`ab_signals_t`, e.g., `AB_SIGNAL_READABLE`, `AB_SIGNAL_WRITABLE`, `AB_SIGNAL_PEER_CLOSED`, `AB_USER_SIGNAL_0`).
  * Threads block on signals using `ab_object_wait_one(handle, signals, deadline, &observed)` and `ab_object_wait_many(...)`.
  * `Event` and `EventPair` objects allow user threads/processes to signal each other and detect peer termination (`AB_SIGNAL_PEER_CLOSED`).
* **Practice**:
  * Implement `state_tracker.c` with observer lists backed by Lesson 06's `event_t` / `wait_queue_t`.
  * Implement `EventDispatcher` (`ab_event_create`) and `EventPairDispatcher` (`ab_eventpair_create`), plus `ab_object_signal`, `ab_object_signal_peer`, and `ab_object_wait_one`.
* **References**: `zircon_org/docs/signals.md`, `zircon_org/kernel/object/include/object/state_tracker.h`, `zircon_org/kernel/object/event_dispatcher.cpp`, `zircon_org/kernel/object/event_pair_dispatcher.cpp`.
* **Goal**: User-space threads synchronizing via `EventPair` handles, `ab_object_wait_one`, and automatic `AB_SIGNAL_PEER_CLOSED` notification when a peer handle is closed.

#### Lesson 09: Asynchronous Event Notification with `Port` Objects
* **Theory**: Waiting on many objects in servers/drivers requires an event loop primitive (`Port`, similar to epoll/kqueue but integrated with Zircon objects). Objects bound via `ab_object_wait_async(handle, port, key, signals)` automatically enqueue a `ab_port_packet_t` to the `Port` when signals assert.
* **Practice**:
  * Implement `PortDispatcher` (`ab_port_create`, `ab_port_queue`, `ab_port_wait`) and `WaitStateObserver` (`ab_object_wait_async`).
* **References**: `zircon_org/docs/objects/port.md`, `zircon_org/kernel/object/port_dispatcher.cpp`, `zircon_org/kernel/object/wait_state_observer.cpp`.
* **Goal**: An EL0 server thread multiplexing multiple `Event`s and user packets on a single `Port` via `ab_port_wait`.

#### Lesson 10: Message-Passing IPC: `Channel` & Capability Transfer
* **Theory**:
  * `Channel` is the backbone of a Zircon microkernel: a bidirectional, two-ended datagram transport (`ChannelDispatcher`).
  * A `MessagePacket` carries up to `AB_CHANNEL_MAX_MSG_BYTES` of data **plus up to `AB_CHANNEL_MAX_MSG_HANDLES` Handles**.
  * Writing handles into a channel **removes** them from the sender's handle table (`AB_RIGHT_TRANSFER` required) and **installs** them into the receiver's handle table upon `ab_channel_read`!
* **Practice**:
  * Implement Peered `ChannelDispatcher` (`ab_channel_create`, `ab_channel_write`, `ab_channel_read`, `ab_channel_call`).
  * Update `AB_SIGNAL_READABLE`, `AB_SIGNAL_WRITABLE`, and `AB_SIGNAL_PEER_CLOSED` via `StateTracker`.
* **References**: `zircon_org/docs/objects/channel.md`, `zircon_org/kernel/object/channel_dispatcher.cpp`, `zircon_org/kernel/object/message_packet.cpp`.
* **Goal**: Process A creates an `Event`, sends its Handle across a `Channel` to Process B, and Process B uses that transferred Handle to signal Process A!

#### Lesson 11: Shared Memory IPC: `VMO` (Virtual Memory Object) & `VMAR`
* **Theory**:
  * Copying large payloads (disk blocks, files, framebuffers) through `Channel` messages is slow. Instead, processes create a **VMO** (`VmObjectDispatcher`), read/write or map it into their address space (`VmAddressRegionDispatcher` / `ab_vmar_map`), and transfer a VMO Handle over a `Channel` for zero-copy shared memory!
* **Practice**:
  * Implement `VmObjectDispatcher` (`ab_vmo_create`, `ab_vmo_read`, `ab_vmo_write`, `ab_vmo_get_size`) and `VmAddressRegionDispatcher` (`ab_vmar_map`, `ab_vmar_unmap`), including lazy page allocation on data abort for VMO-backed mappings.
* **References**: `zircon_org/docs/objects/vm_object.md`, `zircon_org/docs/objects/vm_address_region.md`, `zircon_org/kernel/object/vm_object_dispatcher.cpp`, `zircon_org/kernel/object/vm_address_region_dispatcher.cpp`.
* **Goal**: Process A creates a VMO, maps it, writes data, sends a read-only duplicated VMO handle over a `Channel` to Process B, and Process B maps the exact same physical pages into its own address space.

---

### Phase 3: User-Space Bootstrapping, Drivers & Services (Lessons 12–14)
*In a true microkernel, the kernel has NO filesystem, NO block driver, and NO shell. Everything above IPC + VM + scheduling runs in EL0 user space.*

#### Lesson 12: Tasks (`Job`, `Process`, `Thread`) & `userboot` Bootstrapping
* **Theory**:
  * How does a microkernel start the first user process if the filesystem and ELF loader are in user space?
  * Following Zircon's `userboot` design (`docs/userboot.md`): the kernel embeds a tiny initial user process (`userboot`) and wraps the RAM disk / TAR image (`bootfs`) in a VMO. The kernel creates the root `Job`, starts `userboot`, and passes a bootstrap `Channel` containing handles to the Root Job, Root Resource, and the `bootfs` VMO!
  * `userboot` (running in EL0!) parses `bootfs`, creates child processes via `ab_process_create` / `ab_vmo_create` / `ab_vmar_map` / `ab_thread_create` / `ab_process_start`, and hands them their initial channels.
* **Practice**:
  * Implement `JobDispatcher`, `ProcessDispatcher`, and `ThreadDispatcher` syscalls (`ab_process_create`, `ab_process_start`, `ab_thread_create`, `ab_thread_start`, `ab_task_kill`).
  * Implement `userboot` in EL0 that loads a second program from the `bootfs` VMO and starts it with a bootstrap `Channel`.
* **References**: `zircon_org/docs/userboot.md`, `zircon_org/docs/program_loading.md`, `zircon_org/kernel/lib/userboot/userboot.cpp`, `zircon_org/kernel/object/process_dispatcher.cpp`.
* **Goal**: Kernel boots `userboot` in EL0 with zero in-kernel filesystem code; `userboot` loads and spawns a child user process from the `bootfs` VMO.

#### Lesson 13: User-Space Device Drivers (`Resource`, Physical MMIO VMOs & `Interrupt` Objects)
* **Theory**:
  * How do device drivers run in EL0 user space?
  * Zircon provides two driver kernel objects:
    1. **Physical MMIO VMOs** (`ab_vmo_create_physical(resource_handle, paddr, size, &vmo)`) and DMA-pinned memory (`ab_vmo_create_contiguous` / `ab_vmo_get_paddr`), gated by a privileged `Resource` handle.
    2. **Interrupt Objects** (`ab_interrupt_create(resource_handle, irq_num, &intr)`), allowing an EL0 driver thread to block on `ab_interrupt_wait(intr)` when a hardware IRQ fires (GIC masks the IRQ in EL1, wakes the driver thread, and unmasks on `ab_interrupt_ack(intr)`).
* **Practice**:
  * Implement `ResourceDispatcher` and `InterruptDispatcher` (`ab_vmo_create_physical`, `ab_vmo_create_contiguous`, `ab_interrupt_create`, `ab_interrupt_wait`, `ab_interrupt_ack`).
  * Move the **VirtIO Block Driver** and **UART Console Driver** out of the kernel and into unprivileged **EL0 user-space driver processes**!
* **References**: `zircon_org/docs/objects/interrupts.md`, `zircon_org/docs/objects/resource.md`, `zircon_org/kernel/object/interrupt_dispatcher.cpp`, `zircon_org/kernel/object/resource_dispatcher.cpp`.
* **Goal**: An EL0 VirtIO-blk driver process reading disk sectors using physical MMIO VMOs + DMA VMOs, and an EL0 UART driver handling IRQ 33 via an `Interrupt` object!

#### Lesson 14: Complete Microkernel OS — User-Space Filesystem Server, Console & Shell
* **Theory**:
  * Putting it all together: `userboot` launches:
    1. `console_drv` (EL0 UART interrupt + MMIO driver)
    2. `blk_drv` (EL0 VirtIO block driver) & `fs_srv` (EL0 TAR filesystem server)
    3. `shell` (EL0 interactive shell)
  * `userboot` connects them with `Channel` endpoints (capability-based service wiring!).
  * When you type `ls`, `cat`, or run a binary in `shell`, it sends IPC messages over its `fs_channel` and `console_channel`, reading file contents via shared `VMO`s and spawning child processes via `Job`/`Process` syscalls!
* **Practice**:
  * Build the multi-process user-space ecosystem (`userboot`, `console_srv`, `fs_srv`, `shell`, and user apps) and run the complete microkernel OS in QEMU.
* **Goal**: Interactive shell on ABOS where drivers, filesystem, and shell all run as isolated EL0 processes communicating over Zircon-style `Channel`s, `VMO`s, `Event`s, and `Interrupt`s!
