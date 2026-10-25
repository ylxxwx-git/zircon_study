# ABOS Lesson 11: Shared Memory IPC (`VMO`, `VMAR` & Demand Paging)

## Overview
While `Channel`s copy small control messages (and transfer Handles), large bulk data in Zircon is shared zero-copy using **Virtual Memory Objects (`VmObjectDispatcher` / `VMO`)** and **Virtual Memory Address Regions (`VmAddressRegionDispatcher` / `VMAR`)**.

This lesson implements:
1. **`SYS_VMO_CREATE` / `SYS_VMO_READ` / `SYS_VMO_WRITE`**: Lazily-allocated physical page arrays owned by a reference-counted `VmObjectDispatcher`.
2. **`SYS_VMAR_MAP`**: Capability-checked mapping of a VMO into a process's user `TTBR0_EL1` address space (enforcing `AB_RIGHT_MAP`, `AB_RIGHT_READ`, and `AB_RIGHT_WRITE`).
3. **EL0 Demand-Paging Page Fault Handler (`EC=0x24`)**: When EL0 touches an unallocated page inside a VMAR mapping, `arm64_sync_exception` calls `ab_process_handle_page_fault` to allocate the physical page in the VMO and map it into the faulting process's page tables.

## Building and Running
```bash
make
make run
```
