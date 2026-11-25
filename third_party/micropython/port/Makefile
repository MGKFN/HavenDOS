include ../../py/mkenv.mk
QSTR_DEFS = qstrdefsport.h
MICROPY_ROM_TEXT_COMPRESSION ?= 1
include $(TOP)/py/py.mk

INC += -I. -I$(TOP) -I$(BUILD)
CC = gcc -m32
CFLAGS += $(INC) -ffreestanding -fno-stack-protector -fno-pie -fno-pic \
          -fno-asynchronous-unwind-tables -fno-builtin-printf -std=gnu99 -Wall \
          -Os -DNDEBUG -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fdata-sections -ffunction-sections
CFLAGS += -DMICROPY_ROM_TEXT_COMPRESSION=1

SRC_C = havendos_port.c lib.c \
	extmod/modtime.c \
	extmod/modrandom.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \
	shared/runtime/gchelper_generic.c \
	shared/runtime/interrupt_char.c \
	shared/libc/printf.c

LIBM_SRC_C = $(addprefix lib/libm/,\
	math.c acoshf.c asinfacosf.c asinhf.c atan2f.c atanf.c atanhf.c \
	ef_rem_pio2.c erf_lgamma.c fmodf.c kf_cos.c kf_rem_pio2.c kf_sin.c \
	kf_tan.c log1pf.c nearbyintf.c roundf.c sf_cos.c sf_erf.c sf_frexp.c \
	sf_ldexp.c sf_modf.c sf_sin.c sf_tan.c wf_lgamma.c wf_tgamma.c ef_sqrt.c)

SRC_QSTR += extmod/modtime.c extmod/modrandom.c havendos_port.c shared/readline/readline.c shared/runtime/pyexec.c

OBJ = $(PY_CORE_O) $(addprefix $(BUILD)/, $(SRC_C:.c=.o)) $(addprefix $(BUILD)/, $(LIBM_SRC_C:.c=.o))

all: $(BUILD)/libmpy.a

$(BUILD)/libmpy.a: $(OBJ)
	$(ECHO) "AR $@"
	$(Q)rm -f $@
	$(Q)ar rcs $@ $^

include $(TOP)/py/mkrules.mk

$(BUILD)/lib/libm/%.o: CFLAGS += -U__FLT_EVAL_METHOD__ -D__FLT_EVAL_METHOD__=0 -Wno-builtin-macro-redefined -Wno-maybe-uninitialized
