# src/analyze: the one module of ours that links the processing library
# (libdisc-analyze.a) — it is handed copies of data and answers questions;
# no byte that reaches an output file passes through it. Today: the title
# timeline's host (the stock frame parser, the LATM stream header, the
# TrueHD / MLP end-of-stream decode); the probes follow.

ANALYZE_SRC   := $(ROOT)/src/analyze
ANALYZE_OBJ   := $(BUILD)/analyze
ANALYZE_FILES := $(notdir $(wildcard $(ANALYZE_SRC)/*.c))
ANALYZE_OBJS  := $(patsubst %.c,$(ANALYZE_OBJ)/%.o,$(ANALYZE_FILES))
ANALYZE_LIB   := $(BUILD)/lib/libdisc-analyze.a

ifeq ($(MODE),debug)
ANALYZE_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
ANALYZE_CFLAGS := -O0 -g --coverage
else
ANALYZE_CFLAGS := -O2
endif
ANALYZE_CFLAGS += -fPIC -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src -I$(FFMPEG)/include

$(ANALYZE_OBJ)/%.o: $(ANALYZE_SRC)/%.c $(BUILD)/ffmpeg/.installed
	@mkdir -p $(ANALYZE_OBJ)
	$(CC) $(ANALYZE_CFLAGS) -MMD -MP -c $< -o $@

$(ANALYZE_LIB): $(ANALYZE_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(ANALYZE_OBJS)

-include $(ANALYZE_OBJS:.o=.d)
