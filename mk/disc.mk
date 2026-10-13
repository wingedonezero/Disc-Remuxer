# src/disc: the disc layer's own modules (libdisc-disc.a) — the clip
# demuxer (clip_demux.c: a clip's blocks through the container splitters in
# src/mux, raw PES payloads out), the language codes (lang.c), the disc-info
# model (info.h) and the format parts (bluray/, hddvd/). On base, mux and the
# timeline's headers only: no FFmpeg under it (the include path carries
# none, so an FFmpeg dependency is a compile error); the Blu-ray part reads
# AACS through libaacs and metadata XML through expat. The rest of the disc
# layer joins it as the rewrite's step 7 moves it out of libavformat.

DISC_SRC   := $(ROOT)/src/disc
DISC_OBJ   := $(BUILD)/disc
DISC_FILES := $(notdir $(wildcard $(DISC_SRC)/*.c))
DISC_OBJS  := $(patsubst %.c,$(DISC_OBJ)/%.o,$(DISC_FILES)) \
              $(patsubst $(DISC_SRC)/%.c,$(DISC_OBJ)/%.o,$(wildcard $(DISC_SRC)/bluray/*.c)) \
              $(patsubst $(DISC_SRC)/%.c,$(DISC_OBJ)/%.o,$(wildcard $(DISC_SRC)/hddvd/*.c))
DISC_LIB   := $(BUILD)/lib/libdisc-disc.a
DISC_PC    := PKG_CONFIG_PATH=$(PC) PKG_CONFIG_LIBDIR=$(PC)

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

$(DISC_OBJ)/bluray/%.o: $(DISC_SRC)/bluray/%.c $(PC)/expat.pc $(PC)/libaacs.pc
	@mkdir -p $(DISC_OBJ)/bluray
	$(CC) $(DISC_CFLAGS) $$($(DISC_PC) pkg-config --cflags expat libaacs) -MMD -MP -c $< -o $@

$(DISC_OBJ)/hddvd/%.o: $(DISC_SRC)/hddvd/%.c $(PC)/expat.pc $(PC)/libaacs.pc
	@mkdir -p $(DISC_OBJ)/hddvd
	$(CC) $(DISC_CFLAGS) $$($(DISC_PC) pkg-config --cflags expat libaacs) -MMD -MP -c $< -o $@

$(DISC_LIB): $(DISC_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(DISC_OBJS)

-include $(DISC_OBJS:.o=.d)
