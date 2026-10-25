# ABOS Lesson 14: Complete Capability Microkernel OS

## Architecture Overview
In this capstone lesson, the kernel contains **only** scheduling, virtual memory management, capability handle tables, IPC (`Channel`, `VMO`, `Port`, `Event`), and interrupt routing (`InterruptDispatcher`). All device drivers, filesystems, and interactive services run as isolated **EL0 user-space processes**:

1. **`userboot` (EL0)**: Receives `(RootJob, RootLog, RootResource)` on its bootstrap `Channel` and spawns the three system servers, wiring them together via IPC `Channel` capabilities.
2. **`console-srv` (EL0)**: Maps PL011 UART0 MMIO (`0x09000000`) into EL0 via `SYS_VMO_CREATE_PHYSICAL` and handles GIC SPI 33 interrupts via `SYS_INTERRUPT_WAIT` / `SYS_INTERRUPT_ACK`.
3. **`fs-srv` (EL0)**: Drives the VirtIO Block device in EL0 via DMA VMOs and hardware IRQs, parses the USTAR TAR filesystem on `disk.img`, and serves files as **Read-Only VMOs transferred over `Channel`s**.
4. **`abos-shell` (EL0)**: Runs with **zero hardware capabilities**, interacting with the user via `console-srv` and mapping file VMOs received from `fs-srv`.

## Building and Running
```bash
make
make run
```
