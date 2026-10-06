# CCExtractor 0.94 (C only, CMake WITHOUT_RUST): the `ccextractor` helper
# program that turns closed captions into SRT. CMake writes into its source
# tree, so it builds from a copy of libs/ccextractor/src kept in step with
# rsync (libs/ stays untouched).

CCX_SRC  := $(LIBS)/ccextractor/src
CCX_COPY := $(BUILD)/ccextractor/src
CCX_ARGS := -DWITHOUT_RUST=ON -DWITH_OCR=OFF -DWITH_FFMPEG=OFF -DWITH_SHARING=OFF -DWITH_HARDSUBX=OFF \
            -DCMAKE_BUILD_TYPE=Release
$(call args_stamp,$(BUILD)/ccextractor/args,$(CCX_ARGS))

$(BUILD)/ccextractor/.synced: $(call src_files,$(CCX_SRC))
	@mkdir -p $(CCX_COPY)
	rsync -a --checksum $(CCX_SRC)/ $(CCX_COPY)/
	touch $@

$(BUILD)/ccextractor/build/Makefile: $(BUILD)/ccextractor/args $(BUILD)/ccextractor/.synced
	@mkdir -p $(BUILD)/ccextractor/build
	cd $(BUILD)/ccextractor/build && cmake $(CCX_COPY) $(CCX_ARGS) >/dev/null
	touch $@

$(BUILD)/bin/ccextractor: $(BUILD)/ccextractor/build/Makefile $(BUILD)/ccextractor/.synced
	$(MAKE) -C $(BUILD)/ccextractor/build -j$(JOBS) ccextractor
	@mkdir -p $(BUILD)/bin
	cp -f $(BUILD)/ccextractor/build/ccextractor $@
