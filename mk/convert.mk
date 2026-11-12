# src/convert: the one conversion the program makes (libdisc-convert.a), and
# the second module of ours that links the processing library: a lossless
# audio track of a title with cuts decoded and written as FLAC, cut between
# two samples (off unless asked for).

CONVERT_SRC   := $(ROOT)/src/convert
CONVERT_OBJ   := $(BUILD)/convert
CONVERT_FILES := $(notdir $(wildcard $(CONVERT_SRC)/*.c))
CONVERT_OBJS  := $(patsubst %.c,$(CONVERT_OBJ)/%.o,$(CONVERT_FILES))
CONVERT_LIB   := $(BUILD)/lib/libdisc-convert.a

ifeq ($(MODE),debug)
CONVERT_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
CONVERT_CFLAGS := -O0 -g --coverage
else
CONVERT_CFLAGS := -O2
endif
CONVERT_CFLAGS += -fPIC -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src -I$(FFMPEG)/include

$(CONVERT_OBJ)/%.o: $(CONVERT_SRC)/%.c $(BUILD)/ffmpeg/.installed
	@mkdir -p $(CONVERT_OBJ)
	$(CC) $(CONVERT_CFLAGS) -MMD -MP -c $< -o $@

$(CONVERT_LIB): $(CONVERT_OBJS)
	@mkdir -p $(BUILD)/lib
	$(AR) rcs $@.new $(CONVERT_OBJS)
	mv -f $@.new $@

-include $(CONVERT_OBJS:.o=.d)
