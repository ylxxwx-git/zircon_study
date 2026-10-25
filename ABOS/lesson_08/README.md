# ABOS Lesson 08: Object Signals, `StateTracker`, `WaitOne`, `Event` & `EventPair`

## Overview
In Zircon, every kernel object embeds a **`StateTracker`** that maintains a 32-bit bitmask of active signals (`zx_signals_t`) and a list of registered **`StateObserver`** nodes.

This lesson implements:
1. **`ab_state_tracker_t` & `ab_wait_observer_t`**: Signal bitmask management (`AB_SIGNAL_READABLE`, `AB_SIGNAL_WRITABLE`, `AB_SIGNAL_PEER_CLOSED`, `AB_SIGNAL_SIGNALED`, `AB_USER_SIGNAL_0..3`) and `SYS_OBJECT_WAIT_ONE`.
2. **`EventDispatcher` (`SYS_EVENT_CREATE`)**: Single-ended user-signalable synchronization object.
3. **`EventPairDispatcher` (`SYS_EVENTPAIR_CREATE`)**: Peered kernel objects linked by `related_koid`. Supports `SYS_OBJECT_SIGNAL_PEER` and automatically asserts `AB_SIGNAL_PEER_CLOSED` when the peer's handle count drops to zero.

## Building and Running
```bash
make
make run
```
