# ABOS Lesson 09: Asynchronous Event Notification with `Port` Objects

## Overview
Zircon's `PortDispatcher` is the equivalent of `epoll` / `kqueue` for kernel objects. Rather than blocking a thread per handle, user-space servers create a `Port` (`SYS_PORT_CREATE`), subscribe handles to the port using `SYS_OBJECT_WAIT_ASYNC(handle, port_handle, key, signals)`, and run a single event loop calling `SYS_PORT_WAIT`.

## Building and Running
```bash
make
make run
```
