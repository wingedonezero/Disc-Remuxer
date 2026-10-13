# src/disc: the disc layer's own modules (libdisc-disc.a) — today the clip
# demuxer (clip_demux.c: a clip's blocks through the container splitters in
# src/mux, raw PES payloads out). On base, mux and the timeline's headers
# only: no FFmpeg under it (the include path carries none, so an FFmpeg
# dependency is a compile error). The rest of the disc layer joins it as the
# rewrite's step 7 moves it out of libavformat.

DISC_SRC   := $(ROOT)/src/disc
DISC_OBJ   := $(BUILD)/disc
DISC_FILES := $(notdir $(wildcard $(DISC_SRC)/*.c))
DISC_OBJS  := $(patsubst %.c,$(DISC_OBJ)/%.o,$(DISC_FILES))
DISC_LIB   := $(BUILD)/lib/libdisc-disc.a

ifeq ($(MODE),debug)
DISC_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
DISC_CFLAGS := -O0 -g --coverage
else
DISC_CFLAGS := -O2
endif
DISC_CFLAGS += -fPIC -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src

$(DISC_OBJ)/%.o: $(DISC_SRC)/%.c
	@mkdir -p $(DISC_OBJ)
	$(CC) $(DISC_CFLAGS) -MMD -MP -c $< -o $@

$(DISC_LIB): $(DISC_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(DISC_OBJS)

-include $(DISC_OBJS:.o=.d)
