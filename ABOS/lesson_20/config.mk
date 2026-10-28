# =========================================================================
# config.mk - Shared Toolchain & Build Configuration for ABOS Lesson 19
#
# Included by the top-level Makefile and every subdirectory Makefile.
# =========================================================================

ROOT_DIR      ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ZIRCON_PRE    := $(abspath $(ROOT_DIR)/../../../zircon_org/prebuilt/downloads)
PREBUILT_GCC  := $(ZIRCON_PRE)/gcc/bin/aarch64-elf-gcc
PREBUILT_QEMU := $(ZIRCON_PRE)/qemu/bin/qemu-system-aarch64

ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-linux-gnu-
else ifneq ($(shell which aarch64-elf-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-elf-
else ifneq ($(wildcard $(PREBUILT_GCC)),)
    CROSS_COMPILE ?= $(ZIRCON_PRE)/gcc/bin/aarch64-elf-
else
    CROSS_COMPILE ?= aarch64-linux-gnu-
endif

ifneq ($(shell which qemu-system-aarch64 2>/dev/null),)
    QEMU ?= qemu-system-aarch64
else ifneq ($(wildcard $(PREBUILT_QEMU)),)
    QEMU ?= $(PREBUILT_QEMU)
else
    QEMU ?= qemu-system-aarch64
endif

CC            := $(CROSS_COMPILE)gcc
LD            := $(CROSS_COMPILE)ld
AR            := $(CROSS_COMPILE)ar
OBJCOPY       := $(CROSS_COMPILE)objcopy

# Shared directories & artifacts
INC_DIR       := $(ROOT_DIR)/include
ULIBC_DIR     := $(ROOT_DIR)/user/ulibc
IMAGE_DIR     := $(ROOT_DIR)/image

KERNEL_LD     := $(ROOT_DIR)/linker.ld
USER_LD       := $(ROOT_DIR)/user/link.ld

CRT0_OBJ      := $(ULIBC_DIR)/crt0.o
CRT_ENV_OBJ   := $(ULIBC_DIR)/crt_env.o
ULIBC_OBJ     := $(ULIBC_DIR)/ulibc.o
ULIBC_A       := $(ULIBC_DIR)/libulibc.a

HEADERS       := $(wildcard $(INC_DIR)/*.h) $(wildcard $(ULIBC_DIR)/*.h)

CFLAGS        := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
                 -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g \
                 -I$(INC_DIR) -I$(ULIBC_DIR)

KERNEL_LDFLAGS := -n -T $(KERNEL_LD)
USER_LDFLAGS   := -n -T $(USER_LD)

# Standard compilation rules for C and Assembly sources
%.o: %.c $(HEADERS)
	@echo "  [CC]      $(patsubst $(ROOT_DIR)/%,%,$(abspath $<))"
	@$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S $(HEADERS)
	@echo "  [AS]      $(patsubst $(ROOT_DIR)/%,%,$(abspath $<))"
	@$(CC) $(CFLAGS) -c $< -o $@
