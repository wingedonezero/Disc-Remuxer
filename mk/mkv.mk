# src/mkv: the Matroska writer (libdisc-mkv.a), C++ on libebml + libmatroska
# (mk/matroska.mk) and zlib (mk/zlib.mk) behind a C interface (mkv.h). Its
# include path holds only src/ and those libraries.

MKV_SRC   := $(ROOT)/src/mkv
MKV_OBJ   := $(BUILD)/mkv
MKV_FILES := $(notdir $(wildcard $(MKV_SRC)/*.cpp))
MKV_OBJS  := $(patsubst %.cpp,$(MKV_OBJ)/%.o,$(MKV_FILES))
MKV_LIB   := $(BUILD)/lib/libdisc-mkv.a
# the writer and its libraries, in link order, with the C++ runtime
MKV_LINK  := $(MKV_LIB) $(MATROSKA_LIB) $(EBML_LIB) $(ZLIB_LIB) -lstdc++

CXX ?= c++
ifeq ($(MODE),debug)
MKV_CXXFLAGS := -O0 -g
else ifeq ($(MODE),coverage)
MKV_CXXFLAGS := -O0 -g --coverage
else
MKV_CXXFLAGS := -O2
endif
MKV_CXXFLAGS += -fPIC -std=c++17 -Wall -Wextra -Wno-unused-parameter -Wno-overloaded-virtual -I$(ROOT)/src \
                -I$(MKV_PREFIX)/include -I$(ZLIB_SRC)

$(MKV_OBJ)/%.o: $(MKV_SRC)/%.cpp $(MATROSKA_LIB)
	@mkdir -p $(MKV_OBJ)
	$(CXX) $(MKV_CXXFLAGS) -MMD -MP -c $< -o $@

$(MKV_LIB): $(MKV_OBJS) $(ZLIB_LIB)
	@mkdir -p $(BUILD)/lib
	rm -f $@
	$(AR) rcs $@ $(MKV_OBJS)

-include $(MKV_OBJS:.o=.d)
