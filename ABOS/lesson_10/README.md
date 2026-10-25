# ABOS Lesson 10: Message-Passing IPC (`Channel` & Capability Transfer)

## Overview
In a capability-based microkernel like Zircon, a `Channel` transfers not only byte buffers (`MessagePacket` data) but also **Handles (capabilities)** between isolated user-space processes (`zx_channel_write` / `zx_channel_read`).

When a process attaches Handles to `SYS_CHANNEL_WRITE`:
1. The kernel verifies `AB_RIGHT_TRANSFER` on each Handle and atomically removes it from the **sender process's handle table**.
2. The underlying `(ab_object_t *, ab_rights_t)` references travel inside the `ab_message_packet_t` queued on the peer endpoint.
3. When the receiver process calls `SYS_CHANNEL_READ`, the kernel installs those objects into the **receiver process's handle table** and returns new process-local Handle values.

## Building and Running
```bash
make
make run
```
