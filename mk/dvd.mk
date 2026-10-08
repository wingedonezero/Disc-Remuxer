# libdvdcss, libdvdread, libdvdnav: compiled directly (no autotools), with
# the config.h / version.h their own builds would generate for Linux.

# $(call c_library,name,source dir,files,cflags): the objects of the files
# (with header dependencies) into build/<mode>/<name>/lib<name>.a.
define c_library
$(1)_OBJ := $$(patsubst %.c,$(BUILD)/$(1)/obj/%.o,$(3))
$(BUILD)/$(1)/obj/%.o: $(2)/%.c $(BUILD)/$(1)/include/config.h
	@mkdir -p $$(dir $$@)
	$$(CC) $$(LIB_CFLAGS) $(4) -MMD -MP -c $$< -o $$@
$(BUILD)/$(1)/lib$(1).a: $$($(1)_OBJ)
	@rm -f $$@
	$$(AR) rcs $$@ $$^
-include $$($(1)_OBJ:.o=.d)
endef

# $(call version_h,input,output,major var,minor var,micro var,major,minor,micro)
define version_h
	@mkdir -p $(dir $(2))
	sed -e 's/@$(3)@/$(6)/' -e 's/@$(4)@/$(7)/' -e 's/@$(5)@/$(8)/' $(1) > $(2)
endef

# ---- libdvdcss 1.6.0 ----
DVDCSS_SRC := $(LIBS)/libdvdcss/src
DVDCSS_INC := $(BUILD)/dvdcss/include
DVDCSS_FILES := cpxm.c css.c device.c error.c ioctl.c libdvdcpxm.c libdvdcss.c
DVDCSS_CFLAGS := -std=c17 -D_DEFAULT_SOURCE -I$(DVDCSS_INC) -I$(LIBS)/libdvdcss -I$(DVDCSS_SRC) -I$(DVDCSS_SRC)/dvdcss
$(eval $(call c_library,dvdcss,$(DVDCSS_SRC),$(DVDCSS_FILES),$(DVDCSS_CFLAGS)))

$(DVDCSS_INC)/config.h: mk/dvd.mk $(DVDCSS_SRC)/dvdcss/version.h.in
	@mkdir -p $(dir $@)
	printf '%s\n' '#define PACKAGE_VERSION "1.6.0"' '#define HAVE_ERRNO_H 1' '#define HAVE_FCNTL_H 1' \
	  '#define HAVE_PWD_H 1' '#define HAVE_SCSI_SG_H 1' '#define HAVE_SYS_IOCTL_H 1' '#define HAVE_SYS_PARAM_H 1' \
	  '#define HAVE_SYS_STAT_H 1' '#define HAVE_SYS_TYPES_H 1' '#define HAVE_SYS_UIO_H 1' '#define HAVE_UNISTD_H 1' \
	  '#define DVD_STRUCT_IN_LINUX_CDROM_H 1' '#define HAVE_LINUX_DVD_STRUCT 1' \
	  '#define SUPPORT_ATTRIBUTE_VISIBILITY_DEFAULT 1' > $@
	$(call version_h,$(DVDCSS_SRC)/dvdcss/version.h.in,$(DVDCSS_INC)/dvdcss/version.h,DVDCSS_VERSION_MAJOR,DVDCSS_VERSION_MINOR,DVDCSS_VERSION_MICRO,1,6,0)

# ---- libdvdread 7.1.1 ----
DVDREAD_SRC := $(LIBS)/libdvdread/src
DVDREAD_INC := $(BUILD)/dvdread/include
DVDREAD_FILES := bitreader.c dvd_input.c dvd_reader.c dvd_udf.c ifo_print.c ifo_read.c logger.c md5.c \
                 nav_print.c nav_read.c file/file_posix.c
DVDREAD_CFLAGS := -std=c11 -D_DEFAULT_SOURCE -D_LARGEFILE64_SOURCE -I$(DVDREAD_INC) -I$(LIBS)/libdvdread \
                  -I$(DVDREAD_SRC) -I$(DVDREAD_SRC)/dvdread -I$(DVDCSS_INC) -I$(DVDCSS_SRC)
$(eval $(call c_library,dvdread,$(DVDREAD_SRC),$(DVDREAD_FILES),$(DVDREAD_CFLAGS)))

$(DVDREAD_INC)/config.h: mk/dvd.mk $(DVDREAD_SRC)/dvdread/version.h.in $(DVDCSS_INC)/config.h
	@mkdir -p $(dir $@)
	printf '%s\n' '#define PACKAGE_VERSION "7.1.1"' '#define HAVE_SYS_PARAM_H 1' '#define HAVE_LIMITS_H 1' \
	  '#define HAVE_DIRENT_H 1' '#define UNUSED __attribute__((unused))' '#define HAVE_GETMNTENT_R 1' \
	  '#define HAVE_STRERROR_R 1' '#define HAVE_DECL_STRERROR_R 1' '#define STRERROR_R_CHAR_P 1' \
	  '#define HAVE_STATIC_ASSERT 1' '#define HAVE_DVDCSS_DVDCSS_H 1' '#define HAVE_DVDCSS_DVDCPXM_H 1' > $@
	$(call version_h,$(DVDREAD_SRC)/dvdread/version.h.in,$(DVDREAD_INC)/dvdread/version.h,DVDREAD_VERSION_MAJOR,DVDREAD_VERSION_MINOR,DVDREAD_VERSION_MICRO,7,1,1)

# ---- libdvdnav 7.0.0 ----
DVDNAV_SRC := $(LIBS)/libdvdnav/src
DVDNAV_INC := $(BUILD)/dvdnav/include
DVDNAV_FILES := dvdnav.c highlight.c logger.c navigation.c read_cache.c searching.c settings.c \
                vm/decoder.c vm/getset.c vm/play.c vm/rand.c vm/vm.c vm/vmcmd.c vm/vmget.c
DVDNAV_CFLAGS := -std=c17 -DHAVE_CONFIG_H -D_DEFAULT_SOURCE -I$(DVDNAV_INC) -I$(LIBS)/libdvdnav -I$(DVDNAV_SRC) \
                 -I$(DVDNAV_SRC)/dvdnav -I$(DVDNAV_SRC)/vm -I$(DVDREAD_INC) -I$(DVDREAD_SRC)
$(eval $(call c_library,dvdnav,$(DVDNAV_SRC),$(DVDNAV_FILES),$(DVDNAV_CFLAGS)))

$(DVDNAV_INC)/config.h: mk/dvd.mk $(DVDNAV_SRC)/dvdnav/version.h.in $(DVDREAD_INC)/config.h
	@mkdir -p $(dir $@)
	printf '%s\n' '#define VERSION "7.0.0"' '#define HAVE_DLFCN_H 1' '#define HAVE_INTTYPES_H 1' \
	  '#define HAVE_MEMORY_H 1' '#define HAVE_STDINT_H 1' '#define HAVE_STDLIB_H 1' '#define HAVE_STRINGS_H 1' \
	  '#define HAVE_STRING_H 1' '#define HAVE_SYS_STAT_H 1' '#define HAVE_SYS_TYPES_H 1' '#define HAVE_UNISTD_H 1' \
	  '#define HAVE_GETTIMEOFDAY 1' > $@
	$(call version_h,$(DVDNAV_SRC)/dvdnav/version.h.in,$(DVDNAV_INC)/dvdnav/version.h,DVDNAV_MAJOR,DVDNAV_MINOR,DVDNAV_SUB,7,0,0)

# ---- .pc files for FFmpeg's configure ----
$(PC)/dvdread.pc: $(BUILD)/dvdread/libdvdread.a $(BUILD)/dvdcss/libdvdcss.a mk/dvd.mk
	@mkdir -p $(PC)
	printf '%s\n' 'Name: dvdread' 'Description: dvdread built by disc-remuxer' 'Version: 7.1.1' \
	  'Cflags: -I$(DVDREAD_INC) -I$(DVDREAD_SRC) -I$(DVDCSS_INC) -I$(DVDCSS_SRC)' \
	  'Libs: -L$(BUILD)/dvdread -ldvdread -L$(BUILD)/dvdcss -ldvdcss' > $@

$(PC)/dvdnav.pc: $(BUILD)/dvdnav/libdvdnav.a $(PC)/dvdread.pc mk/dvd.mk
	@mkdir -p $(PC)
	printf '%s\n' 'Name: dvdnav' 'Description: dvdnav built by disc-remuxer' 'Version: 7.0.0' \
	  'Cflags: -I$(DVDNAV_INC) -I$(DVDNAV_SRC) -I$(DVDREAD_INC) -I$(DVDREAD_SRC) -I$(DVDCSS_INC) -I$(DVDCSS_SRC)' \
	  'Libs: -L$(BUILD)/dvdnav -ldvdnav -L$(BUILD)/dvdread -ldvdread -L$(BUILD)/dvdcss -ldvdcss -lpthread' > $@
