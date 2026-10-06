# The tests (tests/, pytest): a Python venv with the test tools, the bridge
# the unit tests call our C code through (cffi, compiled against our headers
# and linked with the static libraries of this mode), then pytest.
#
#   build/venv/              the venv (one for both modes)
#   build/<mode>/pytest/     the bridge module

VENV   := $(ROOT)/build/venv
PY     := $(VENV)/bin/python
BRIDGE := $(BUILD)/pytest
BRIDGE_PKGS := libavformat libavcodec libswresample libavutil

$(VENV)/.installed: $(ROOT)/tests/requirements.txt
	python3 -m venv $(VENV)
	$(VENV)/bin/pip install -q -r $<
	touch $@

# libdvdnav's VM driven directly (its private headers and config.h, which
# cannot meet FFmpeg's in one file)
$(BRIDGE)/dvdnav_selftest.o: $(ROOT)/tests/bridge/dvdnav_selftest.c $(BUILD)/dvdnav/libdvdnav.a
	@mkdir -p $(BRIDGE)
	$(CC) $(LIB_CFLAGS) $(DVDNAV_CFLAGS) -c $< -o $@

$(BRIDGE)/.built: $(VENV)/.installed $(BUILD)/ffmpeg/.installed $(BRIDGE)/dvdnav_selftest.o \
                  $(wildcard $(ROOT)/tests/bridge/*.* $(ROOT)/tests/bridge/cdef/*)
	@mkdir -p $(BRIDGE)
	BRIDGE_CFLAGS="$$(sed -n 's/^CPPFLAGS=//p' $(BUILD)/ffmpeg/build/ffbuild/config.mak) -DHAVE_AV_CONFIG_H -std=c17 \
	  -I$(FFMPEG_SRC) -I$(BUILD)/ffmpeg/build $$($(TOOL_PC) pkg-config --static --cflags $(BRIDGE_PKGS) dvdnav dvdread expat libaacs)" \
	BRIDGE_LIBS="$(BRIDGE)/dvdnav_selftest.o $$($(TOOL_PC) pkg-config --static --libs $(BRIDGE_PKGS)) -lpthread $(COV_LDFLAGS)" \
	$(PY) $(ROOT)/tests/bridge/build_bridge.py $(BRIDGE)
	touch $@

.PHONY: check bridge
bridge: $(BRIDGE)/.built
check: dist $(BRIDGE)/.built
	cd $(ROOT) && DR_MODE=$(MODE) $(PY) -m pytest tests $(PYTEST_ARGS)
