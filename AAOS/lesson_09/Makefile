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

# User Space Sources
USER_C_SOURCES = user/lib.c user/shell.c
USER_ASM_SOURCES = user/start.S
USER_OBJS = $(USER_ASM_SOURCES:.S=.o) $(USER_C_SOURCES:.c=.o)

# User Space Flags (No stdlib, freestanding)
USER_CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g -fno-pie -fno-pic -fno-builtin -mgeneral-regs-only
USER_LDFLAGS = -n -Ttext=0x0

.PHONY: all clean run

all: kernel.img

# Build User Shell Binary
shell.bin: $(USER_OBJS)
	@echo "Linking shell.bin..."
	$(LD) $(USER_LDFLAGS) -o shell.elf $(USER_OBJS)
	$(OBJCOPY) -O binary shell.elf shell.bin

# Convert Shell Binary to Object File
shell_bin.o: shell.bin
	@echo "Converting shell.bin to object..."
	$(LD) -r -b binary -o shell_bin.o shell.bin

# Link Kernel with Shell Object
kernel.elf: $(OBJS) shell_bin.o linker.ld
	@echo "Linking kernel.elf..."
	$(LD) $(LDFLAGS) -o kernel.elf $(OBJS) shell_bin.o

kernel.img: kernel.elf
	@echo "Creating kernel.img..."
	$(OBJCOPY) -O binary kernel.elf kernel.img

%.o: %.c
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -c $< -o $@

user/%.o: user/%.c
	@echo "Compiling User $<..."
	$(CC) $(USER_CFLAGS) -c $< -o $@

user/%.o: user/%.S
	@echo "Assembling User $<..."
	$(CC) $(USER_CFLAGS) -c $< -o $@

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
	rm -f $(OBJS) $(USER_OBJS) kernel.elf kernel.img shell.elf shell.bin shell_bin.o
