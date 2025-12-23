# Define the cross-compiler toolchain prefix for AArch64 bare-metal development
# You will need to install the 'aarch64-linux-gnu-gcc' package on your system
CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
AS = $(CROSS_COMPILE)as
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

# Source files
C_SOURCES = main.c
ASM_SOURCES = startup.s

# Object files
OBJS = $(C_SOURCES:.c=.o) $(ASM_SOURCES:.s=.o)

# Target executable name
TARGET = kernel.elf

# Compiler flags for C files
# -O0: No optimization (easier debugging)
# -Wall: All warnings
# -nostdlib: Don't link standard C libraries (we are bare-metal!)
# -nostartfiles: Don't link standard startup files
# -mcpu=cortex-a53: Target the CPU model QEMU will emulate
# -ffreestanding: Compliance for a free-standing environment
CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g

# Linker flags
# -n: Read/write data sections in text segment
# -T linker.ld: Use our custom linker script
LDFLAGS = -n -T linker.ld

.PHONY: all clean run

all: $(TARGET)

# Rule to link the object files into the final ELF executable
$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Rule to compile C files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to assemble Assembly files
%.o: %.s
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to run the kernel in QEMU
# -M virt: Emulate the generic AArch64 virtual machine
# -cpu cortex-a53: Emulate a modern AArch64 CPU
# -kernel $(TARGET): Load our kernel as the boot image
# -nographic -serial mon:stdio: Output serial data to the host console
run: $(TARGET)
	qemu-system-aarch64 -M virt -cpu cortex-a53 -kernel $(TARGET) -nographic -serial mon:stdio

clean:
	rm -f $(OBJS) $(TARGET)