# =========================================================================
# ABOS Lesson 19: Hierarchical Subdirectory Makefiles & Common User Linker
#
# Directory & Build Hierarchy:
#   config.mk                - Shared toolchain, flags, paths & compilation rules
#   linker.ld                - EL1 higher-half microkernel linker script
#   include/                 - Shared kernel, MM, object, syscall, and IPC headers
#   kernel/Makefile          - Orchestrates EL1 kernel subdirectories:
#     kernel/core/Makefile   - Boot ASM, exceptions, SMP scheduler, objects, syscalls
#     kernel/mm/Makefile     - Physical Memory Manager (pmm), heap, 4-level MMU
#     kernel/dev/Makefile    - GICv2 interrupt controller, timer, early debuglog
#   user/Makefile            - Orchestrates EL0 user-space subdirectories:
#     user/link.ld           - Common linker script shared by all user-space apps
#     user/ulibc/Makefile    - C Runtime (crt0.S, crt_env.c) & libulibc.a
#     user/drivers/Makefile  - EL0 User-Space Drivers (console_srv, fs_srv)
#     user/userboot/Makefile - EL0 root bootstrap process
#     user/shell/Makefile    - EL0 interactive capability shell (abos-shell)
#     user/apps/Makefile     - Standalone EL0 apps linked with user/link.ld
#   image/Makefile           - Links image/kernel.img and packs image/disk.img
# =========================================================================

ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
include $(ROOT_DIR)/config.mk

SUBDIRS    := kernel user image
KERNEL_IMG := $(IMAGE_DIR)/kernel.img
DISK_IMG   := $(IMAGE_DIR)/disk.img

.PHONY: all kernel user image run clean check-tools install-tools

all: check-tools kernel user image

check-tools:
	@if ! command -v $(CC) >/dev/null 2>&1 && [ ! -x "$(CC)" ]; then \
		echo "Error: Cross-compiler '$(CC)' not found!"; \
		echo "Run './install_tools.sh' (or 'make install-tools') to install AArch64 GCC & QEMU."; \
		exit 1; \
	fi

install-tools:
	./install_tools.sh

kernel: check-tools
	@$(MAKE) --no-print-directory -C kernel all

user: check-tools
	@$(MAKE) --no-print-directory -C user all

image: kernel user
	@$(MAKE) --no-print-directory -C image all

run: all
	@if ! command -v $(QEMU) >/dev/null 2>&1 && [ ! -x "$(QEMU)" ]; then \
		echo "Error: '$(QEMU)' not found! Run './install_tools.sh' first."; \
		exit 1; \
	fi
	@echo "Running ABOS Lesson 19 in QEMU..."
	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel $(KERNEL_IMG) -nographic -serial mon:stdio \
		-drive file=$(DISK_IMG),format=raw,if=none,id=hd0 -device virtio-blk-device,drive=hd0

clean:
	@echo "Cleaning up all subdirectory build artifacts..."
	@for dir in $(SUBDIRS); do \
		$(MAKE) --no-print-directory -C $$dir clean; \
	done
