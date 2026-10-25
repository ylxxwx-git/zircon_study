# Studying `zircon_org`: How Zircon Works & How ABOS Models It in C

This document bridges the gap between the production C++ Zircon codebase in `/usr/local/google/home/lipingliu/80_Zircon/zircon_org` and our step-by-step C implementation in **ABOS**.

---

## 1. Zircon's Two-Layer Kernel Architecture (`zx_and_lk.md`)

When you inspect `/usr/local/google/home/lipingliu/80_Zircon/zircon_org/kernel/`, you will notice two distinct layers:

```
+-----------------------------------------------------------------------+
|                       EL0 User Space Processes                        |
|        (userboot, devmgr / drivers, filesystems, shell, apps)         |
+-----------------------------------------------------------------------+
                   |                                 ^
                   |  SVC #0 (Syscall ABI / vDSO)    |
                   v                                 |
+=======================================================================+
| LAYER 2: Zircon Object & Capability Layer (kernel/object, syscalls)   |
|  - Handle Table & Rights enforcement (Handle, ProcessDispatcher)      |
|  - Reference-counted Dispatchers (Channel, VMO, VMAR, Port, Event,    |
|    Interrupt, Resource, Job, Process, Thread)                         |
|  - StateTracker & Observers (Signals, WaitOne, WaitMany, Port Wait)   |
+-----------------------------------------------------------------------+
| LAYER 1: LK (Little Kernel) Core Layer (kernel/arch, kernel, vm, dev) |
|  - AArch64 Startup, EL1 Exception Vectors, GICv2, ARM Generic Timer   |
|  - Physical Memory Manager (PMM arenas) & VmAspace (TTBR0 / TTBR1)    |
|  - Kernel Threads (thread_t), Preemptive Scheduler, Spinlocks         |
|  - Blocking Synchronization (wait_queue_t, event_t, mutex_t)          |
+=======================================================================+
```

### Layer 1: The LK Core (Lessons 01–06)
* **Entry Point (`kernel/top/main.cpp`)**: `lk_main()` brings up early console logging, early threading (`thread_init_early`), physical/virtual memory (`vm_init_preheap`, `heap_init`, `vm_init`), kernel primitives (`kernel_init`), and then spawns `bootstrap2` thread before calling `thread_become_idle()`.
* **AArch64 Arch (`kernel/arch/arm64/`)**:
  * `start.S`: Checks `CurrentEL`, drops from EL2/EL3 to EL1, sets up early page tables linking the kernel in higher-half virtual memory, zeroes `.bss`, and branches to `lk_main`.
  * `exceptions.S` & `exceptions_c.cpp`: Saves user/kernel register state into an `iframe_t` on the kernel stack and dispatches synchronous exceptions (syscalls `EC=0x15`, data/instruction aborts) and IRQs.
  * `mmu.cpp`: Manages 4-level page tables (`TTBR1_EL1` for kernel space, `TTBR0_EL1` with 16-bit ASIDs for user processes).
* **Threads & Wait Queues (`kernel/kernel/thread.cpp`, `wait.cpp`, `event.cpp`)**:
  * `thread_t` is the schedulable unit of execution. Even a Zircon user-mode thread (`ThreadDispatcher`) wraps an underlying LK `thread_t`!
  * `wait_queue_t` and `event_t` provide blocking sleep/wakeup so threads waiting on IPC or signals consume zero CPU time.

### Layer 2: The Zircon Object Layer (Lessons 07–14)
* **Dispatchers (`kernel/object/include/object/dispatcher.h`)**:
  * Every kernel object exposed to user space inherits from `Dispatcher`.
  * Each instance gets a globally unique 64-bit **KOID** (`zx_koid_t`), a reference count, and an optional `StateTracker`.
  * Some objects are **Solo** (`EventDispatcher`, `VmObjectDispatcher`, `PortDispatcher`), while two-ended IPC objects are **Peered** (`ChannelDispatcher`, `SocketDispatcher`, `EventPairDispatcher`), holding a reference to their peer endpoint and asserting `ZX_SIGNAL_PEER_CLOSED` when the peer's last handle closes.
* **Handles & Rights (`kernel/object/handle.cpp`)**:
  * A `Handle` binds `(process_id, RefPtr<Dispatcher>, zx_rights_t)` to a 32-bit integer (`zx_handle_t`) in a process's handle table.
  * Rights are checked at the syscall boundary when resolving a `zx_handle_t` into a `Dispatcher*`!

---

## 2. How ABOS Models Zircon's C++ Classes in Clean C

To keep ABOS as approachable and transparent as AAOS, we translate Zircon's C++ `Dispatcher` hierarchy into clean C structs with explicit reference counting and function pointers where needed:

### Kernel Object Base (`ab_object_t`)
```c
typedef uint64_t ab_koid_t;
typedef uint32_t ab_handle_t;
typedef uint32_t ab_rights_t;
typedef uint32_t ab_signals_t;

typedef enum {
    AB_OBJ_TYPE_NONE = 0,
    AB_OBJ_TYPE_PROCESS,
    AB_OBJ_TYPE_THREAD,
    AB_OBJ_TYPE_VMO,
    AB_OBJ_TYPE_CHANNEL,
    AB_OBJ_TYPE_EVENT,
    AB_OBJ_TYPE_EVENTPAIR,
    AB_OBJ_TYPE_PORT,
    AB_OBJ_TYPE_INTERRUPT,
    AB_OBJ_TYPE_RESOURCE,
    AB_OBJ_TYPE_JOB,
    AB_OBJ_TYPE_VMAR,
} ab_obj_type_t;

struct ab_object {
    ab_koid_t koid;
    ab_obj_type_t type;
    volatile int32_t ref_count;     // Total references (handles + in-flight kernel refs)
    volatile int32_t handle_count;  // Number of open handles pointing to this object
    ab_state_tracker_t state_tracker;
    void (*on_zero_handles)(struct ab_object *obj); // Alerts peered objects (PEER_CLOSED)
    void (*destroy)(struct ab_object *obj);         // Frees underlying object memory
};
```

### Handle & Per-Process Handle Table (`ab_handle_entry_t`)
```c
struct ab_handle_entry {
    struct ab_object *obj;  // NULL if slot is free
    ab_rights_t rights;     // Bitmask of AB_RIGHT_*
};
```

### StateTracker & Signal Observers (`ab_state_tracker_t`)
In Zircon (`zircon_org/kernel/object/include/object/state_tracker.h`), when an object's state changes (for instance, a message is written to a `Channel`, asserting `AB_CHANNEL_READABLE`), the object calls `state_tracker_update_signals(&obj->state_tracker, set_mask, clear_mask)`.
* That function walks the list of registered **Observers**:
  1. **Synchronous `WaitStateObserver`** (created on the stack during `ab_object_wait_one` / `ab_object_wait_many`), which signals an LK `event_t` to wake up the sleeping thread!
  2. **Asynchronous `PortObserver`** (registered via `ab_object_wait_async`), which queues a packet into a `PortDispatcher` and wakes any thread waiting in `ab_port_wait`!

---

## 3. Key `zircon_org` Files to Reference During Each Lesson

| Topic | Path in `/usr/local/google/home/lipingliu/80_Zircon/zircon_org/` |
| :--- | :--- |
| **AArch64 Boot & MMU** | `kernel/arch/arm64/start.S`, `kernel/arch/arm64/mmu.cpp` |
| **Exceptions & Syscall Entry** | `kernel/arch/arm64/exceptions.S`, `kernel/arch/arm64/exceptions_c.cpp` |
| **LK Main & Init Sequence** | `kernel/top/main.cpp` |
| **LK Threads & Scheduler** | `kernel/kernel/thread.cpp`, `kernel/kernel/sched.cpp`, `kernel/arch/arm64/asm.S` |
| **LK Events & Wait Queues** | `kernel/kernel/event.cpp`, `kernel/kernel/wait.cpp` |
| **Physical Memory (PMM)** | `kernel/vm/pmm.cpp`, `kernel/vm/pmm_node.cpp` |
| **VMO & VMAR** | `kernel/vm/vm_object_paged.cpp`, `kernel/object/vm_object_dispatcher.cpp`, `kernel/object/vm_address_region_dispatcher.cpp` |
| **Dispatcher & Handles** | `kernel/object/include/object/dispatcher.h`, `kernel/object/handle.cpp` |
| **Signals & StateTracker** | `kernel/object/include/object/state_tracker.h`, `kernel/object/wait_state_observer.cpp` |
| **Event & EventPair** | `kernel/object/event_dispatcher.cpp`, `kernel/object/event_pair_dispatcher.cpp` |
| **Port** | `kernel/object/port_dispatcher.cpp` |
| **Channel & MessagePacket** | `kernel/object/channel_dispatcher.cpp`, `kernel/object/message_packet.cpp` |
| **Job, Process & Thread** | `kernel/object/job_dispatcher.cpp`, `kernel/object/process_dispatcher.cpp`, `kernel/object/thread_dispatcher.cpp` |
| **Userboot** | `kernel/lib/userboot/userboot.cpp`, `system/core/userboot/start.c`, `docs/userboot.md` |
| **Interrupt & Resource** | `kernel/object/interrupt_dispatcher.cpp`, `kernel/object/resource_dispatcher.cpp` |
