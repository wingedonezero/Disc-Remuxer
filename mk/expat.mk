# expat 2.9.0: compiled directly with the expat_config.h its own build
# writes for Linux (the c_library rule of dvd.mk; config.h is its stand-in).

EXPAT_SRC := $(LIBS)/expat/lib
EXPAT_INC := $(BUILD)/expat/include
EXPAT_FILES := xcs.c xmlparse.c xmltok.c xmlrole.c random_arc4random.c random_arc4random_buf.c \
               random_dev_urandom.c random_getentropy.c random_getrandom.c
EXPAT_CFLAGS := -I$(EXPAT_INC) -I$(EXPAT_SRC)
$(eval $(call c_library,expat,$(EXPAT_SRC),$(EXPAT_FILES),$(EXPAT_CFLAGS)))

$(EXPAT_INC)/config.h: mk/expat.mk
	@mkdir -p $(dir $@)
	printf '%s\n' '#ifndef EXPAT_CONFIG_H' '#define EXPAT_CONFIG_H 1' '#define BYTEORDER 1234' \
	  '#define HAVE_ARC4RANDOM 1' '#define HAVE_ARC4RANDOM_BUF 1' '#define HAVE_DLFCN_H 1' '#define HAVE_FCNTL_H 1' \
	  '#define HAVE_GETENTROPY 1' '#define HAVE_GETPAGESIZE 1' '#define HAVE_GETRANDOM 1' '#define HAVE_INTTYPES_H 1' \
	  '#define HAVE_MMAP 1' '#define HAVE_STDINT_H 1' '#define HAVE_STDIO_H 1' '#define HAVE_STDLIB_H 1' \
	  '#define HAVE_STRINGS_H 1' '#define HAVE_STRING_H 1' '#define HAVE_SYSCALL_GETRANDOM 1' \
	  '#define HAVE_SYS_PARAM_H 1' '#define HAVE_SYS_STAT_H 1' '#define HAVE_SYS_TYPES_H 1' '#define HAVE_UNISTD_H 1' \
	  '#define PACKAGE "expat"' '#define PACKAGE_NAME "expat"' '#define PACKAGE_STRING "expat 2.9.0"' \
	  '#define PACKAGE_TARNAME "expat"' '#define PACKAGE_VERSION "2.9.0"' '#define STDC_HEADERS 1' \
	  '#define VERSION "2.9.0"' '#define XML_CONTEXT_BYTES 1024' '#define XML_DTD 1' '#define XML_GE 1' \
	  '#define XML_NS 1' '#endif' > $@
	cp $@ $(EXPAT_INC)/expat_config.h

$(PC)/expat.pc: $(BUILD)/expat/libexpat.a mk/expat.mk
	@mkdir -p $(PC)
	printf '%s\n' 'Name: expat' 'Description: expat built by disc-remuxer' 'Version: 2.9.0' \
	  'Cflags: -I$(EXPAT_SRC)' 'Libs: -L$(BUILD)/expat -lexpat' > $@
