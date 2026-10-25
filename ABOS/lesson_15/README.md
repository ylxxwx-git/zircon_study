# ABOS Lesson 15: User-Space Calculator Application (`calc-app`)

## Overview
Building on Lesson 14's complete microkernel environment, Lesson 15 adds an interactive **EL0 User-Space Calculator Application (`calc-app`)** that can be launched on demand from `abos-shell` using the `calc` command.

### How `calc` Works in a Capability Microkernel
1. When you type `calc` at the `abos$` prompt, `abos-shell` uses its `JobDispatcher` capability (`SYS_PROCESS_CREATE`, `SYS_THREAD_CREATE`, `SYS_PROCESS_START`) to spawn a brand-new isolated EL0 process named **`calc-app`**.
2. `abos-shell` duplicates its **Console IPC Channel** capability (`SYS_HANDLE_DUPLICATE`) and transfers it to `calc-app` over a bootstrap `Channel`, then waits on `SYS_OBJECT_WAIT_ONE(calc_proc, AB_SIGNAL_TASK_TERMINATED)`.
3. **`calc-app`** (running in its own isolated EL0 address space) communicates with `console-srv` over the Console `Channel`:
   - Prompts for the **first number** (`Enter first number: `)
   - Prompts for the **operation** (`Enter operation (+, -, *, /): `)
   - Prompts for the **second number** (`Enter second number: `)
   - Computes the 64-bit signed integer result (including remainder for `/` and division-by-zero protection), prints the equation and result, and exits cleanly back to `abos$`.

## Building and Running
```bash
make
make run
```
