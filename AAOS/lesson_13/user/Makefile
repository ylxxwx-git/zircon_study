CROSS_COMPILE = aarch64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
AR = $(CROSS_COMPILE)ar

INCLUDE_DIR = ../include

CFLAGS = -O0 -Wall -Wextra -nostdlib -nostartfiles -mcpu=cortex-a53 -ffreestanding -g -fno-pie -fno-pic -fno-builtin -mgeneral-regs-only -I$(INCLUDE_DIR) -Ilib
LDFLAGS = -n -Ttext=0x0

all: shell.bin disk.img

# Runtime (crt0.o)
lib/crt0.o: lib/crt0.S
	$(CC) $(CFLAGS) -c $< -o $@

# C Library (libc.a)
lib/libc.a: lib/lib.o
	$(AR) rcs $@ $<

lib/lib.o: lib/lib.c
	$(CC) $(CFLAGS) -c $< -o $@

# Shell
shell/shell.o: shell/shell.c
	$(CC) $(CFLAGS) -c $< -o $@

shell.elf: lib/crt0.o shell/shell.o lib/libc.a
	$(LD) $(LDFLAGS) -o shell.elf lib/crt0.o shell/shell.o lib/libc.a

shell.bin: shell.elf
	$(OBJCOPY) -O binary shell.elf shell.bin

# Disk Image
disk.img: shell.bin
	@echo "Creating disk.img in user/..."
	mkdir -p rootfs
	cp shell.bin rootfs/shell.bin
	tar -cf disk.img -C rootfs .

clean:
	rm -f *.o *.a *.elf *.bin disk.img
	rm -f lib/*.o lib/*.a
	rm -f shell/*.o
	rm -rf rootfs
