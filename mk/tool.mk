# disc-remuxer (src/disc-remuxer) with tomlc17, linked statically against
# our FFmpeg and everything under it. The engine (every file but main.c, with
# tomlc17) is a library of its own, libdisc-remuxer.a, which the command
# line (main.c) and the GUI link.

TOOL_SRC   := $(ROOT)/src/disc-remuxer
TOOL_OBJ   := $(BUILD)/tool
TOOL_FILES := $(notdir $(wildcard $(TOOL_SRC)/*.c))
TOOL_OBJS  := $(patsubst %.c,$(TOOL_OBJ)/%.o,$(TOOL_FILES)) $(TOOL_OBJ)/tomlc17.o
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
TOOL_CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(FFMPEG)/include -I$(LIBS)/tomlc17 \
               -DDISC_REMUXER_VERSION='"$(TOOL_VERSION)"'

$(TOOL_OBJ)/%.o: $(TOOL_SRC)/%.c $(BUILD)/ffmpeg/.installed
	@mkdir -p $(TOOL_OBJ)
	$(CC) $(TOOL_CFLAGS) -MMD -MP -c $< -o $@

$(TOOL_OBJ)/tomlc17.o: $(LIBS)/tomlc17/tomlc17.c
	@mkdir -p $(TOOL_OBJ)
	$(CC) $(LIB_CFLAGS) -std=c17 -c $< -o $@

$(ENGINE_LIB): $(ENGINE_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(ENGINE_OBJS)

$(BUILD)/bin/disc-remuxer: $(TOOL_OBJ)/main.o $(ENGINE_LIB) $(BUILD)/ffmpeg/.installed
	@mkdir -p $(BUILD)/bin
	$(CC) $(COV_LDFLAGS) -o $@ $(TOOL_OBJ)/main.o $(ENGINE_LIB) $$($(TOOL_PC) pkg-config --static --libs libavformat libavcodec libswresample libavutil) -lpthread
ifeq ($(MODE),release)
	strip $@
endif

-include $(TOOL_OBJS:.o=.d)
