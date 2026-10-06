# FFmpeg (libs/ffmpeg, our copy): its own configure + make, out of tree,
# static, with our DVD libraries, expat and libaacs found through the .pc
# files above. FFmpeg's own make is run every time (it rebuilds only what
# changed); configure only when its arguments change.

FFMPEG_SRC := $(LIBS)/ffmpeg
FFMPEG     := $(BUILD)/ffmpeg/install
FFMPEG_PCS := $(PC)/dvdread.pc $(PC)/dvdnav.pc $(PC)/expat.pc $(PC)/libaacs.pc

FFMPEG_ARGS := --prefix=$(FFMPEG) --enable-gpl --enable-libdvdnav --enable-libdvdread --enable-libexpat \
  --enable-libaacs --disable-autodetect --enable-static --disable-shared --enable-pic --disable-doc \
  --disable-ffplay --pkg-config=pkg-config --pkg-config-flags=--static
ifeq ($(MODE),debug)
FFMPEG_ARGS += --disable-stripping
endif
ifeq ($(MODE),coverage)
FFMPEG_ARGS += --disable-stripping --disable-optimizations --extra-cflags=--coverage --extra-ldflags=--coverage
endif
$(call args_stamp,$(BUILD)/ffmpeg/args,$(FFMPEG_ARGS))

$(BUILD)/ffmpeg/build/ffbuild/config.mak: $(BUILD)/ffmpeg/args $(FFMPEG_PCS) $(FFMPEG_SRC)/configure
	@mkdir -p $(BUILD)/ffmpeg/build
	cd $(BUILD)/ffmpeg/build && PKG_CONFIG_PATH=$(PC) PKG_CONFIG_LIBDIR=$(PC) $(FFMPEG_SRC)/configure $(FFMPEG_ARGS)
	touch $@

.PHONY: ffmpeg-make
ffmpeg-make: $(BUILD)/ffmpeg/build/ffbuild/config.mak
	$(MAKE) -C $(BUILD)/ffmpeg/build -j$(JOBS)

# installed headers and libraries (for our programs), and the programs
$(BUILD)/ffmpeg/.installed: ffmpeg-make
	$(MAKE) -C $(BUILD)/ffmpeg/build install >/dev/null
	@mkdir -p $(BUILD)/bin
	cp -f $(FFMPEG)/bin/ffmpeg $(FFMPEG)/bin/ffprobe $(BUILD)/bin/
	touch $@

$(BUILD)/bin/ffmpeg $(BUILD)/bin/ffprobe: $(BUILD)/ffmpeg/.installed
