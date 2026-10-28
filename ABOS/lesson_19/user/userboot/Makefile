# =========================================================================
# user/userboot/Makefile - EL0 Root Bootstrap Service (userboot.c)
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
