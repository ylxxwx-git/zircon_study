# =========================================================================
# kernel/dev/Makefile - EL1 GICv2 Interrupt Controller, Timer & Early DebugLog
# =========================================================================
ROOT_DIR ?= $(abspath ../..)
include $(ROOT_DIR)/config.mk

C_SRCS := $(wildcard *.c)
OBJS   := $(C_SRCS:.c=.o)

.PHONY: all clean

all: $(OBJS)
	@:

clean:
	@rm -f $(OBJS)
