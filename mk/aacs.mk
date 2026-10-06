# libgpg-error 1.61, libgcrypt 1.12.4, libaacs 0.12.0: each with its own
# configure + make, out of tree, installed into build/<mode>/<lib>/install.
# The autotools are never run again (the generated files in libs/ are used).

NO_AUTOTOOLS := ACLOCAL=true AUTOCONF=true AUTOMAKE=true AUTOHEADER=true MAKEINFO=true

# $(call autotools,name,source dir,configure args,environment,prerequisites)
define autotools
$(1)_ARGS := --prefix=$(BUILD)/$(1)/install $(3)
$$(call args_stamp,$(BUILD)/$(1)/args,$$($(1)_ARGS))
$(BUILD)/$(1)/build/Makefile: $(BUILD)/$(1)/args $(5)
	@mkdir -p $(BUILD)/$(1)/build
	cd $(BUILD)/$(1)/build && $(4) $(2)/configure $$($(1)_ARGS)
	touch $$@
$(BUILD)/$(1)/.installed: $(BUILD)/$(1)/build/Makefile $$(call src_files,$(2))
	$$(MAKE) -C $(BUILD)/$(1)/build -j$$(JOBS) $$(NO_AUTOTOOLS) install
	touch $$@
endef

GPGERR := $(BUILD)/gpg-error/install
GCRYPT := $(BUILD)/gcrypt/install
AACS   := $(BUILD)/aacs/install

$(eval $(call autotools,gpg-error,$(LIBS)/libgpg-error,--enable-static --disable-shared --with-pic --disable-nls \
  --disable-languages --disable-doc --disable-tests --enable-install-gpg-error-config,,))

$(eval $(call autotools,gcrypt,$(LIBS)/libgcrypt,--enable-static --disable-shared --with-pic --disable-doc \
  --with-libgpg-error-prefix=$(GPGERR),PATH=$(GPGERR)/bin:$$$$PATH,$(BUILD)/gpg-error/.installed))

$(eval $(call autotools,aacs,$(LIBS)/libaacs,--enable-static --disable-shared --with-pic --disable-werror \
  --with-libgcrypt-prefix=$(GCRYPT) --with-libgpg-error-prefix=$(GPGERR),\
  PATH=$(GCRYPT)/bin:$(GPGERR)/bin:$$$$PATH PKG_CONFIG_PATH=$(GCRYPT)/lib/pkgconfig:$(GPGERR)/lib/pkgconfig \
  PKG_CONFIG_LIBDIR=$(GCRYPT)/lib/pkgconfig:$(GPGERR)/lib/pkgconfig,$(BUILD)/gcrypt/.installed))

$(PC)/libaacs.pc: $(BUILD)/aacs/.installed mk/aacs.mk
	@mkdir -p $(PC)
	printf '%s\n' 'Name: libaacs' 'Description: libaacs built by disc-remuxer' 'Version: 0.12.0' \
	  'Cflags: -I$(AACS)/include -I$(GCRYPT)/include -I$(GPGERR)/include' \
	  'Libs: -L$(AACS)/lib -laacs -L$(GCRYPT)/lib -lgcrypt -L$(GPGERR)/lib -lgpg-error' > $@
