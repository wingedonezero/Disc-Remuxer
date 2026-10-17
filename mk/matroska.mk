# libebml 1.4.7 + libmatroska 1.7.2 (the Matroska writer's libraries): each
# with its own CMake, out of tree, static, installed into
# build/<mode>/matroska/ (include/, lib/); libmatroska on that libebml.

MKV_PREFIX := $(BUILD)/matroska
MKV_CMAKE  := -DCMAKE_BUILD_TYPE=$(if $(filter release,$(MODE)),Release,Debug) -DBUILD_SHARED_LIBS=OFF \
              -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DDISABLE_CMAKE_CONFIG=OFF -DCMAKE_INSTALL_PREFIX=$(MKV_PREFIX) \
              -DCMAKE_INSTALL_LIBDIR=lib -DEBML_DIR=$(MKV_PREFIX)/lib/cmake/EBML \
              -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
              $(if $(filter coverage,$(MODE)),-DCMAKE_CXX_FLAGS=--coverage)
$(call args_stamp,$(BUILD)/matroska/args,$(MKV_CMAKE))

EBML_LIB     := $(MKV_PREFIX)/lib/libebml.a
MATROSKA_LIB := $(MKV_PREFIX)/lib/libmatroska.a

$(EBML_LIB): $(BUILD)/matroska/args $(call src_files,$(LIBS)/libebml)
	@mkdir -p $(BUILD)/matroska/ebml-build
	cd $(BUILD)/matroska/ebml-build && cmake $(LIBS)/libebml $(MKV_CMAKE) >/dev/null
	$(MAKE) -C $(BUILD)/matroska/ebml-build -j$(JOBS) install >/dev/null
	touch $@

$(MATROSKA_LIB): $(EBML_LIB) $(call src_files,$(LIBS)/libmatroska)
	@mkdir -p $(BUILD)/matroska/matroska-build
	cd $(BUILD)/matroska/matroska-build && cmake $(LIBS)/libmatroska $(MKV_CMAKE) >/dev/null
	$(MAKE) -C $(BUILD)/matroska/matroska-build -j$(JOBS) install >/dev/null
	touch $@
