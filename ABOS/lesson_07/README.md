# ABOS Lesson 07: Kernel Objects (`Dispatcher`), KOIDs, Handles, Rights & EL0 Syscalls

## Overview
Lesson 07 begins **Phase 2 (Zircon Object & Capability Layer)** by modeling `zircon_org/kernel/object/dispatcher.cpp`, `handle.cpp`, and `process_dispatcher.cpp`:

1. **Kernel Objects & Reference Counting (`object.h`, `object.c`)**:
   * Every user-visible kernel resource is an `ab_object_t` with a globally unique 64-bit `ab_koid_t`, type tag (`AB_OBJ_TYPE_*`), `ref_count`, `handle_count`, `on_zero_handles` callback, and `destroy` destructor.
2. **Per-Process Handle Table & Capability Rights (`handle_alloc`, `handle_lookup`, `handle_duplicate`, `handle_replace`, `handle_close`)**:
   * User space only sees generation-encoded 32-bit `ab_handle_t` values (`(0x5 << 28) | (gen << 8) | slot`), preventing use-after-close bugs and forging of pointers.
   * Every syscall resolves handles via `handle_lookup`, enforcing both the expected object type (`AB_ERR_WRONG_TYPE`) and required capability rights bitmask (`AB_ERR_ACCESS_DENIED`).
3. **EL0 User Mode & Capability Enforcement (`uspace_entry.S`, `syscalls.c`, `main.c`)**:
   * Drops into EL0 via `arm64_uspace_entry` passing a bootstrap `LogDispatcher` handle in `x0`.
   * The EL0 program logs via `SYS_DEBUGLOG_WRITE`, duplicates the handle without `AB_RIGHT_WRITE`, verifies that writing with the reduced handle fails with `AB_ERR_ACCESS_DENIED`, and closes both handles to trigger `on_zero_handles` and object destruction.

## Building & Running
```bash
make
make run
```
