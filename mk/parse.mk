# src/parse: the frame cutters' rules and the splitter that runs them
# (libdisc-parse.a) — how each codec's stream from a disc is cut into units
# (parse/rules.h, parse/split.h). Pure byte logic on base only.

PARSE_SRC   := $(ROOT)/src/parse
PARSE_OBJ   := $(BUILD)/parse
PARSE_FILES := $(notdir $(wildcard $(PARSE_SRC)/*.c))
PARSE_OBJS  := $(patsubst %.c,$(PARSE_OBJ)/%.o,$(PARSE_FILES))
PARSE_LIB   := $(BUILD)/lib/libdisc-parse.a

ifeq ($(MODE),debug)
PARSE_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
PARSE_CFLAGS := -O0 -g --coverage
else
PARSE_CFLAGS := -O2
endif
PARSE_CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src

$(PARSE_OBJ)/%.o: $(PARSE_SRC)/%.c
	@mkdir -p $(PARSE_OBJ)
	$(CC) $(PARSE_CFLAGS) -MMD -MP -c $< -o $@

$(PARSE_LIB): $(PARSE_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(PARSE_OBJS)

-include $(PARSE_OBJS:.o=.d)
