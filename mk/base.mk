# src/base: the base library (libdisc-base.a) — log, messages, settings
# (with tomlc17), errors, memory, buffers, the typed events header. The one
# part of the program every other part may use. It must not depend on FFmpeg
# or any disc library: its include path holds only src/ and tomlc17, so an
# FFmpeg include here fails the build.

BASE_SRC   := $(ROOT)/src/base
BASE_OBJ   := $(BUILD)/base
BASE_FILES := $(notdir $(wildcard $(BASE_SRC)/*.c))
BASE_OBJS  := $(patsubst %.c,$(BASE_OBJ)/%.o,$(BASE_FILES)) $(BASE_OBJ)/tomlc17.o
BASE_LIB   := $(BUILD)/lib/libdisc-base.a

ifeq ($(MODE),debug)
BASE_CFLAGS := -O0 -g
else ifeq ($(MODE),coverage)
BASE_CFLAGS := -O0 -g --coverage
else
BASE_CFLAGS := -O2
endif
BASE_CFLAGS += -std=gnu17 -Wall -Wextra -Wno-unused-parameter -I$(ROOT)/src -I$(LIBS)/tomlc17

$(BASE_OBJ)/%.o: $(BASE_SRC)/%.c
	@mkdir -p $(BASE_OBJ)
	$(CC) $(BASE_CFLAGS) -MMD -MP -c $< -o $@

$(BASE_OBJ)/tomlc17.o: $(LIBS)/tomlc17/tomlc17.c
	@mkdir -p $(BASE_OBJ)
	$(CC) $(LIB_CFLAGS) -std=c17 -c $< -o $@

$(BASE_LIB): $(BASE_OBJS)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(BASE_OBJS)

-include $(BASE_OBJS:.o=.d)
