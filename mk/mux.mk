# src/mux: the container splitters (libdisc-mux.a) — the MPEG-2 program
# stream pack/sector reading (ps_disc.c) and the transport stream source
# packet / PES reassembly (ts_disc.c), each as a disc reader does it. Pure
# byte logic on base only; our FFmpeg's mpeg.c / mpegts.c disc modes wrap
# them while the title reader still lives there.

MUX_SRC   := $(ROOT)/src/mux
MUX_OBJ   := $(BUILD)/mux
MUX_FILES := $(notdir $(wildcard $(MUX_SRC)/*.c))
MUX_OBJS  := $(patsubst %.c,$(MUX_OBJ)/%.o,$(MUX_FILES))
MUX_LIB   := $(BUILD)/lib/libdisc-mux.a

ifeq ($(MODE),debug)
MUX_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
MUX_CFLAGS := -O0 -g --coverage
else
MUX_CFLAGS := -O2
endif
MUX_CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src

$(MUX_OBJ)/%.o: $(MUX_SRC)/%.c
	@mkdir -p $(MUX_OBJ)
	$(CC) $(MUX_CFLAGS) -MMD -MP -c $< -o $@

$(MUX_LIB): $(MUX_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(MUX_OBJS)

-include $(MUX_OBJS:.o=.d)
