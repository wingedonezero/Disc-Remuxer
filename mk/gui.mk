# disc-remuxer-gui (src/gui, C++ with Qt 6 Widgets): the GUI, linked with the
# engine (libdisc-remuxer.a) and our FFmpeg like the command line. Qt is the
# host's (Qt 6 Widgets development files); everything else is ours.

GUI_SRC  := $(ROOT)/src/gui
GUI_DIR  := $(BUILD)/gui
ifeq ($(MODE),release)
GUI_TYPE := Release
else
GUI_TYPE := Debug
endif
GUI_ARGS := -DCMAKE_BUILD_TYPE=$(GUI_TYPE) -DENGINE_LIB=$(ENGINE_LIB) -DIO_LIB=$(IO_LIB) -DMUX_LIB=$(MUX_LIB) -DPARSE_LIB=$(PARSE_LIB) -DBASE_LIB=$(BASE_LIB) \
            -DENGINE_INCLUDE=$(TOOL_SRC) -DSRC_INCLUDE=$(ROOT)/src \
            -DFFMPEG_PREFIX=$(FFMPEG) -DFFMPEG_PKGCONFIG=$(FFMPEG)/lib/pkgconfig:$(PC) \
            -DDISC_REMUXER_VERSION=$(TOOL_VERSION) -DCOV_LDFLAGS=$(COV_LDFLAGS)
$(call args_stamp,$(GUI_DIR)/args,$(GUI_ARGS))

$(GUI_DIR)/build/Makefile: $(GUI_DIR)/args $(GUI_SRC)/CMakeLists.txt
	@mkdir -p $(GUI_DIR)/build
	cd $(GUI_DIR)/build && cmake $(GUI_SRC) $(GUI_ARGS) >/dev/null
	touch $@

$(BUILD)/bin/disc-remuxer-gui: $(GUI_DIR)/build/Makefile $(ENGINE_LIB) $(IO_LIB) $(MUX_LIB) $(PARSE_LIB) $(BASE_LIB) $(call src_files,$(GUI_SRC)) \
                               $(TOOL_SRC)/engine.h $(BUILD)/ffmpeg/.installed
	$(MAKE) -C $(GUI_DIR)/build -j$(JOBS) disc-remuxer-gui
	@mkdir -p $(BUILD)/bin
	cp -f $(GUI_DIR)/build/disc-remuxer-gui $@
ifeq ($(MODE),release)
	strip $@
endif
