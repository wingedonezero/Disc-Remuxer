# src/timeline: the title timeline (libdisc-timeline.a) — the frame step on
# the splitter, the video grid and audio timing, the joiner and junction,
# sub-pictures, chapters and the stage-5 checks. On base and parse only; the
# few things it still borrows from the processing library come through the
# host hooks (timeline/disctl.h DRTimelineHost).

TIMELINE_SRC   := $(ROOT)/src/timeline
TIMELINE_OBJ   := $(BUILD)/timeline
TIMELINE_FILES := $(notdir $(wildcard $(TIMELINE_SRC)/*.c))
TIMELINE_OBJS  := $(patsubst %.c,$(TIMELINE_OBJ)/%.o,$(TIMELINE_FILES))
TIMELINE_LIB   := $(BUILD)/lib/libdisc-timeline.a

ifeq ($(MODE),debug)
TIMELINE_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
TIMELINE_CFLAGS := -O0 -g --coverage
else
TIMELINE_CFLAGS := -O2
endif
TIMELINE_CFLAGS += -fPIC -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src

$(TIMELINE_OBJ)/%.o: $(TIMELINE_SRC)/%.c
	@mkdir -p $(TIMELINE_OBJ)
	$(CC) $(TIMELINE_CFLAGS) -MMD -MP -c $< -o $@

$(TIMELINE_LIB): $(TIMELINE_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(TIMELINE_OBJS)

-include $(TIMELINE_OBJS:.o=.d)
