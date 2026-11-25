# third_party/micropython/Makefile -- MicroPython as one library for the
# HavenDOS kernel. No MicroPython tools needed: genhdr/ is committed
# (regenerate with tools/vendor_micropython.py).
CC      ?= gcc
SRCS    := $(shell cat SOURCES) port/havendos_port.c port/lib.c
OBJS    := $(addprefix build/,$(SRCS:.c=.o))
CFLAGS  := -m32 -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
           -fno-asynchronous-unwind-tables -std=gnu99 -fbuiltin -Os -DNDEBUG \
           -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DMICROPY_ROM_TEXT_COMPRESSION=1 \
           -Iport -I. -w

all: build/libmpy.a
.PHONY: all

HDRS    := $(wildcard genhdr/*.h port/*.h)

$(OBJS): $(HDRS)

build/libmpy.a: $(OBJS)
	@rm -f $@
	ar rcs $@ $^

build/lib/libm/%.o: lib/libm/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -U__FLT_EVAL_METHOD__ -D__FLT_EVAL_METHOD__=0 -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build
.PHONY: clean
