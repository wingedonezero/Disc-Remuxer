# zlib 1.3.2: compiled directly, the compression and decompression core only
# (no gz* file functions); used by the MKV writer for a track's zlib
# compression. Its zconf.h is used as released (HAVE_UNISTD_H / HAVE_STDARG_H
# are what its configure finds on Linux); config.h is the c_library rule's
# stand-in.

ZLIB_SRC   := $(LIBS)/zlib
ZLIB_INC   := $(BUILD)/zlib/include
ZLIB_FILES := adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c \
              zutil.c
ZLIB_CFLAGS := -DHAVE_UNISTD_H -DHAVE_STDARG_H -I$(ZLIB_SRC)
ZLIB_LIB   := $(BUILD)/zlib/libzlib.a
$(eval $(call c_library,zlib,$(ZLIB_SRC),$(ZLIB_FILES),$(ZLIB_CFLAGS)))

$(ZLIB_INC)/config.h: mk/zlib.mk
	@mkdir -p $(dir $@)
	printf '/* zlib: no generated configuration (zconf.h as released) */\n' > $@
