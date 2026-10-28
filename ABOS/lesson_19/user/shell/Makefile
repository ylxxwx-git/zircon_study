# =========================================================================
# user/shell/Makefile - EL0 Interactive Capability Shell (shell.c)
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
