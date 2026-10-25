# ABOS Lesson 12: Tasks (`Job`, `Process`, `Thread`) & `userboot` Bootstrapping

## Overview
In Zircon, the kernel does not contain a built-in ELF loader or filesystem driver to launch user applications. Instead:
1. The kernel creates the **Root Job** (`JobDispatcher`), **Root Log** (`LogDispatcher`), and a **Bootfs VMO** (`VmObjectDispatcher`) containing the initial user-space binaries.
2. The kernel stuffs Handles to those objects into a bootstrap `Channel` and starts **`userboot`** in EL0.
3. **`userboot`** (running in unprivileged EL0) reads the bootstrap `Channel`, parses the `Bootfs VMO`, and invokes `SYS_PROCESS_CREATE`, `SYS_THREAD_CREATE`, and `SYS_PROCESS_START` to spawn child processes in EL0!

## Building and Running
```bash
make
make run
```
