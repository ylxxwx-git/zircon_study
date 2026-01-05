# =================================================================
# Makefile for AArch64 bare-metal kernel targeting QEMU 'virt'
# =================================================================

CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

# Source files
C_SOURCES = kernel.c output.c gic.c irq.c timer.c dtb.c pmm.c mmu.c sched.c syscall.c process.c
ASM_SOURCES = boot.S utils.S switch.S

# Object files
OBJS = boot.o $(C_SOURCES:.c=.o) utils.o switch.o

# Compiler flags
CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g -fno-pie -fno-pic -fno-builtin -mgeneral-regs-only

# Linker flags
LDFLAGS = -n -T linker.ld

.PHONY: all clean run

all: kernel.img

kernel.elf: $(OBJS) linker.ld
	@echo "Linking kernel.elf..."
	$(LD) $(LDFLAGS) -o kernel.elf $(OBJS)

kernel.img: kernel.elf
	@echo "Creating kernel.img..."
	$(OBJCOPY) -O binary kernel.elf kernel.img

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

mmu.o: mmu.c
	$(CC) $(CFLAGS) -c mmu.c -o mmu.o

sched.o: sched.c
	$(CC) $(CFLAGS) -c sched.c -o sched.o

%.o: %.S
	@echo "Assembling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

run: kernel.img
	@echo "Running kernel in QEMU..."
	qemu-system-aarch64 -M virt -cpu cortex-a53 -smp 4 -kernel kernel.img -nographic -serial mon:stdio

clean:
	@echo "Cleaning up..."
	rm -f $(OBJS) kernel.elf kernel.img
