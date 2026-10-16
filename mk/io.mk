# src/io: the readers (libdisc-io.a) — block reads with retries, the disc
# file systems (ISO 9660 / Joliet, UDF NetBSD- and Linux-based, a disc
# folder on the host), the file-system choice rule and the disc-format
# detection. On base only: no FFmpeg include path, so an FFmpeg dependency
# here fails the build. Log lines leave through the hook dr_io_set_vlog()
# (the program and the test bridge point it at FFmpeg's log while the disc
# layer still lives there).

IO_SRC   := $(ROOT)/src/io
IO_OBJ   := $(BUILD)/io
IO_FILES := $(notdir $(wildcard $(IO_SRC)/*.c))
IO_OBJS  := $(patsubst %.c,$(IO_OBJ)/%.o,$(IO_FILES))
IO_LIB   := $(BUILD)/lib/libdisc-io.a

ifeq ($(MODE),debug)
IO_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
IO_CFLAGS := -O0 -g --coverage
else
IO_CFLAGS := -O2
endif
IO_CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src

$(IO_OBJ)/%.o: $(IO_SRC)/%.c
	@mkdir -p $(IO_OBJ)
	$(CC) $(IO_CFLAGS) -MMD -MP -c $< -o $@

$(IO_LIB): $(IO_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(IO_OBJS)

-include $(IO_OBJS:.o=.d)
