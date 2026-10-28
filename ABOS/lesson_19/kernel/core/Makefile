# =========================================================================
# kernel/core/Makefile - EL1 Boot, Exceptions, SMP Scheduler, Objects, Syscalls
# =========================================================================
ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/config.mk

C_SRCS := $(wildcard *.c)
S_SRCS := $(wildcard *.S)
OBJS   := $(C_SRCS:.c=.o) $(S_SRCS:.S=.o)

.PHONY: all clean

all: $(OBJS)
	@:

clean:
	@rm -f $(OBJS)
