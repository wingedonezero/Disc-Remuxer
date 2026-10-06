# The build of one mode (MODE = debug, release or coverage), included by the top
# Makefile. Layout:
#
#   build/<mode>/<lib>/       objects, generated headers, the library
#   build/<mode>/pkgconfig/   .pc files FFmpeg's configure reads
#   dist/<mode>/              the finished programs only
#
# Sub-builds (autotools, FFmpeg's configure, CMake) are configured again only
# when their arguments change; their own make rebuilds only what changed.

ifeq ($(filter $(MODE),debug release coverage),)
$(error MODE must be debug, release or coverage, not '$(MODE)')
endif

# No built-in suffix rules: the source trees are prerequisites, and make's
# own rules would try to rebuild files in them (e.g. libgcrypt's .info
# manual from its .texi when a fresh checkout leaves the .info older).
.SUFFIXES:

ROOT  := $(abspath .)
LIBS  := $(ROOT)/libs
BUILD := $(ROOT)/build/$(MODE)
DIST  := $(ROOT)/dist/$(MODE)
PC    := $(BUILD)/pkgconfig
JOBS  ?= $(shell nproc 2>/dev/null || echo 4)

CC ?= cc
AR ?= ar

ifeq ($(MODE),debug)
LIB_CFLAGS := -O1 -g
else ifeq ($(MODE),coverage)
LIB_CFLAGS := -O0 -g --coverage
else
LIB_CFLAGS := -O2
endif
LIB_CFLAGS += -fPIC -w

# coverage: every program and the test bridge link the gcov runtime
COV_LDFLAGS := $(if $(filter coverage,$(MODE)),--coverage)

# A file holding the arguments a sub-build was configured with; rewritten
# (so newer) only when they change.
define args_stamp
$(shell mkdir -p $(dir $(1)) && if [ "$$(cat '$(1)' 2>/dev/null)" != '$(2)' ]; then printf '%s' '$(2)' > '$(1)'; fi)
endef

# Every file of a source tree (prerequisites of a sub-build that cannot
# tell by itself whether its sources changed).
src_files = $(shell find $(1) -type f ! -path '*/.git/*')

.PHONY: dist libs
dist: $(DIST)/disc-remuxer $(DIST)/ffmpeg $(DIST)/ffprobe $(DIST)/ccextractor
libs: $(BUILD)/ffmpeg/.installed

include mk/dvd.mk
include mk/expat.mk
include mk/aacs.mk
include mk/ffmpeg.mk
include mk/ccextractor.mk
include mk/tool.mk
include mk/test.mk

$(DIST)/%: $(BUILD)/bin/%
	@mkdir -p $(DIST)
	cp -f $< $@
