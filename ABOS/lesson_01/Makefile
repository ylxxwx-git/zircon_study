# =========================================================================
# Makefile for ABOS Lesson 01: LK Boot, EL Transition & SMP Bringup
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

C_SOURCES   := main.c debuglog.c
ASM_SOURCES := start.S
OBJS        := $(C_SOURCES:.c=.o) $(ASM_SOURCES:.S=.o)
TARGET      := kernel.elf

CFLAGS  := -O0 -Wall -Wextra -nostdlib -nostartfiles -ffreestanding \
           -mcpu=cortex-a53 -mgeneral-regs-only -fno-pie -fno-pic -g
LDFLAGS := -n -T linker.ld

.PHONY: all clean run run_el2 run_el1

all: $(TARGET)

$(TARGET): $(OBJS) linker.ld
	@echo "Linking $(TARGET)..."
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	@echo "Running ABOS Lesson 01 in QEMU (EL3 + EL2 enabled)..."
	$(QEMU) -M virt,secure=on,virtualization=on -cpu cortex-a53 -smp 4 -kernel $(TARGET) -nographic -serial mon:stdio

run_el2: $(TARGET)
	@echo "Running ABOS Lesson 01 in QEMU (EL2 enabled)..."
	$(QEMU) -M virt,virtualization=on -cpu cortex-a53 -smp 4 -kernel $(TARGET) -nographic -serial mon:stdio

run_el1: $(TARGET)
	@echo "Running ABOS Lesson 01 in QEMU (EL1 default)..."
	$(QEMU) -M virt -cpu cortex-a53 -smp 4 -kernel $(TARGET) -nographic -serial mon:stdio

clean:
	rm -f $(OBJS) $(TARGET)
