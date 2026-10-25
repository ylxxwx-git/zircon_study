# =========================================================================
# Makefile for ABOS Lesson 10: Kernel Objects, Handles, Rights & EL0
# =========================================================================

ZIRCON_PREBUILT := /usr/local/google/home/lipingliu/80_Zircon/zircon_org/prebuilt/downloads

ifneq ($(shell which aarch64-linux-gnu-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-linux-gnu-
else ifneq ($(shell which aarch64-elf-gcc 2>/dev/null),)
    CROSS_COMPILE ?= aarch64-elf-
else
    CROSS_COMPILE ?= $(ZIRCON_PREBUILT)/gcc/bin/aarch64-elf-
endif

ifneq ($(shell which qemu-system-aarch64 2>/dev/null),)
    QEMU ?= qemu-system-aarch64
else
    QEMU ?= $(ZIRCON_PREBUILT)/qemu/bin/qemu-system-aarch64
endif

CC      := $(CROSS_COMPILE)gcc
LD      := $(CROSS_COMPILE)ld
OBJCOPY := $(CROSS_COMPILE)objcopy

C_SOURCES   := main.c debuglog.c exceptions_c.c gic.c timer.c dtb.c pmm.c heap.c \
               mmu.c thread.c wait.c object.c syscalls.c
ASM_SOURCES := start.S exceptions.S switch.S uspace_entry.S
OBJS        := $(C_SOURCES:.c=.o) $(ASM_SOURCES:.S=.o)

CFLAGS  := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
           -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g
LDFLAGS := -n -T linker.ld

.PHONY: all clean run

all: kernel.img

kernel.elf: $(OBJS) linker.ld
	@echo "Linking kernel.elf..."
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

kernel.img: kernel.elf
	@echo "Creating kernel.img..."
	$(OBJCOPY) -O binary kernel.elf kernel.img

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.img
	@echo "Running ABOS Lesson 10 in QEMU..."
	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel kernel.img -nographic -serial mon:stdio

clean:
	rm -f $(OBJS) kernel.elf kernel.img
