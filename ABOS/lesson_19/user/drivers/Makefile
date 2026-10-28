# =========================================================================
# user/drivers/Makefile - EL0 User-Space Drivers (console_srv.c, fs_srv.c)
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
