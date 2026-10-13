# disc-remuxer (src/disc-remuxer), linked statically against the base
# library (mk/base.mk), our FFmpeg and everything under it. The engine
# (every file but main.c) is a library of its own, libdisc-remuxer.a, which
# the command line (main.c) and the GUI link, both with libdisc-base.a.

TOOL_SRC   := $(ROOT)/src/disc-remuxer
TOOL_OBJ   := $(BUILD)/tool
TOOL_FILES := $(notdir $(wildcard $(TOOL_SRC)/*.c))
TOOL_OBJS  := $(patsubst %.c,$(TOOL_OBJ)/%.o,$(TOOL_FILES))
ENGINE_OBJS := $(filter-out $(TOOL_OBJ)/main.o,$(TOOL_OBJS))
ENGINE_LIB  := $(BUILD)/lib/libdisc-remuxer.a
TOOL_PC    := PKG_CONFIG_PATH=$(FFMPEG)/lib/pkgconfig:$(PC) PKG_CONFIG_LIBDIR=$(FFMPEG)/lib/pkgconfig:$(PC)
TOOL_VERSION := 0.2.0

ifeq ($(MODE),debug)
TOOL_CFLAGS := -O0 -g
else ifeq ($(MODE),coverage)
TOOL_CFLAGS := -O0 -g --coverage
else
TOOL_CFLAGS := -O2
endif
TOOL_CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src -I$(FFMPEG)/include \
               -DDISC_REMUXER_VERSION='"$(TOOL_VERSION)"'

$(TOOL_OBJ)/%.o: $(TOOL_SRC)/%.c $(BUILD)/ffmpeg/.installed
	@mkdir -p $(TOOL_OBJ)
	$(CC) $(TOOL_CFLAGS) -MMD -MP -c $< -o $@

$(ENGINE_LIB): $(ENGINE_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(ENGINE_OBJS)

$(BUILD)/bin/disc-remuxer: $(TOOL_OBJ)/main.o $(ENGINE_LIB) $(ANALYZE_LIB) $(DISC_LIB) $(TIMELINE_LIB) $(IO_LIB) $(MUX_LIB) $(PARSE_LIB) $(BASE_LIB) $(BUILD)/ffmpeg/.installed
	@mkdir -p $(BUILD)/bin
	$(CC) $(COV_LDFLAGS) -o $@ $(TOOL_OBJ)/main.o $(ENGINE_LIB) $(ANALYZE_LIB) $$($(TOOL_PC) pkg-config --static --libs libavformat libavcodec libswresample libavutil) $(DISC_LIB) $(TIMELINE_LIB) $(IO_LIB) $(MUX_LIB) $(PARSE_LIB) $(BASE_LIB) -lpthread
ifeq ($(MODE),release)
	strip $@
endif

-include $(TOOL_OBJS:.o=.d)
