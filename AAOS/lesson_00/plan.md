# OS Development Plan: From Bare Metal to User Space

This plan outlines a step-by-step approach to building a basic Operating System on the Armv8 (AArch64) architecture. Each lesson focuses on a core OS concept, blending theory with practical implementation.

## Phase 1: Bootstrapping & Basic Output

### Lesson 00: Environment Setup & Toolchains
*   **Theory**: Cross-compilation, Linkers, ELF format, QEMU emulation.
*   **Practice**: Setting up the development environment, understanding the build system (Makefile), and inspecting binaries.
*   **Goal**: A working build system that produces a binary.

### Lesson 01: Bare Metal "Hello World" (UART)
*   **Theory**: Memory Mapped I/O (MMIO), Serial Communication, ARM Boot Protocol.
*   **Practice**:
    *   Writing the startup assembly (`boot.S`) to set up the stack.
    *   Implementing a polling driver for the PL011 UART.
    *   Transitioning from Assembly to C.
*   **Goal**: `printf("Hello World")` working on the serial console.

### Lesson 02: Exception Levels & EL1 Transition
*   **Theory**: ARM Exception Levels (EL0-EL3), System Registers, Privilege Separation.
*   **Practice**:
    *   Checking current Exception Level.
    *   Switching from EL2 (Hypervisor) or EL3 (Secure Monitor) down to EL1 (Kernel).
    *   Setting up the Vector Base Address Register (`VBAR_EL1`).
*   **Goal**: Booting safely into EL1 and catching a basic exception.

## Phase 2: Interrupts & Timing

### Lesson 03: Interrupt Handling (GIC)
*   **Theory**: Interrupt Controllers (GICv2/v3), IRQ vs FIQ, Vector Tables, Context Saving.
*   **Practice**:
    *   Initializing the Generic Interrupt Controller (GIC).
    *   Enabling interrupts in the CPU (`DAIF` bits).
    *   Handling a basic hardware interrupt.
*   **Goal**: Successfully catching and handling a hardware interrupt.

### Lesson 04: The System Timer
*   **Theory**: Hardware Timers, Clock frequencies, Preemption concepts.
*   **Practice**:
    *   Configuring the ARM Generic Timer.
    *   Writing an interrupt handler for the timer.
*   **Goal**: A "tick" printed to the screen every second via interrupt.

## Phase 3: Memory Management

### Lesson 05: Physical Memory Management (PMM)
*   **Theory**: Physical Address Space, Bitmaps, Linked Lists, Buddy Allocators.
*   **Practice**:
    *   Parsing the memory map (from Device Tree or hardcoded).
    *   Implementing a Page Frame Allocator (allocating 4KB physical pages).
*   **Goal**: `alloc_page()` and `free_page()` working.

### Lesson 06: Virtual Memory (MMU)
*   **Theory**: Virtual vs Physical Addresses, Paging, Translation Lookaside Buffer (TLB), Memory Attributes (Cacheable vs Device).
*   **Practice**:
    *   Setting up Page Tables (L0 - L3).
    *   Enabling the MMU (Memory Management Unit).
    *   Creating an Identity Map and a Higher-Half Kernel map.
*   **Goal**: Kernel running with virtual addresses enabled.

## Phase 4: Process Management

### Lesson 07: Kernel Threads & Scheduling
*   **Theory**: Process Control Blocks (PCB), Thread States, Context Switching, Round-Robin Scheduling.
*   **Practice**:
    *   Defining a `struct task`.
    *   Implementing `cpu_switch_to` (Assembly context switch).
    *   Creating a simple Cooperative Scheduler.
*   **Goal**: Two kernel threads printing messages alternately.

### Lesson 08: Preemptive Multitasking
*   **Theory**: Preemption, Timer-based context switching, Race Conditions, Synchronization (Spinlocks).
*   **Practice**:
    *   Connecting the Timer Interrupt to the Scheduler.
    *   Implementing basic locking primitives.
*   **Goal**: Threads switching automatically without explicit `yield()`.

## Phase 5: User Space

### Lesson 09: User Mode (EL0) & System Calls
*   **Theory**: User vs Kernel mode, System Call Interface (SVC instruction), Security rings.
*   **Practice**:
    *   Setting up the stack for EL0.
    *   Implementing the `svc` handler.
    *   Creating a basic syscall table (e.g., `sys_write`, `sys_exit`).
*   **Goal**: A user-mode function calling the kernel to print text.

### Lesson 10: Loading Programs (ELF)
*   **Theory**: ELF File Format (Headers, Segments), Process Address Space.
*   **Practice**:
    *   Parsing a simple raw binary or ELF file.
    *   Mapping user memory.
    *   Jumping to the entry point.
*   **Goal**: Loading and running a separate program.

## Phase 6: Advanced Topics (Optional)

*   **Lesson 11**: File Systems (Simple Read-Only FS like TarFS).
*   **Lesson 12**: Fork & Exec (Process Creation).
*   **Lesson 13**: Shell (Interactive Command Line).
