/*
 * The disc layer (discdec.c) and its format parts: the disc layer opens a
 * source once (disc readers), tells which kind of disc it is and hands it to
 * that kind's format part, which reads the disc's structure, opens the
 * chosen title and gives its packets.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef AVFORMAT_DISC_H
#define AVFORMAT_DISC_H

#include <stddef.h>
#include <stdint.h>

#include "discinfo.h"
#include "avformat.h"
#include "discio.h"

/** One disc format's part. */
typedef struct DiscFormat {
    const char             *name;       /**< the disc's kind for the caller ("HD DVD") */
    enum DiscIODiscFormat   kind;
    size_t                  priv_size;  /**< the part's own context, zeroed before open */
    /** Read the disc's structure from DiscContext.fs, open the title of
     *  option title: its streams (docs/FORMATS.md); and in any case the
     *  disc info's titles (DiscContext.info, ff_disc_add_title...).
     *  Title -1: only the disc info, no packets. */
    int  (*open)(AVFormatContext *s);
    int  (*read_packet)(AVFormatContext *s, AVPacket *pkt);
    /** Also after a failed open (the context as far as open came). */
    void (*close)(AVFormatContext *s);
} DiscFormat;

/** The disc demuxer's context (AVFormatContext.priv_data). */
typedef struct DiscContext {
    const AVClass    *class;

    /* options, for every format part */
    int               opt_read_attempts;
    int               opt_udf_reader;
    int               opt_title;        /**< -1: every title listed, none read */
    int               opt_min_length;
    char            **opt_keydb;
    unsigned          nb_opt_keydb;
    int64_t           progress;         /**< exported: how far the title is read (1/10000) */

    /* the disc */
    DiscIOSource     *image;
    DiscIOFS         *fs;
    const DiscFormat *fmt;
    void             *part;             /**< the format part's context */
    DiscInfo          info;             /**< the disc info (disc_info()) */
} DiscContext;

/** The format part's context of s. */
static inline void *ff_disc_part(AVFormatContext *s)
{
    return ((DiscContext *)s->priv_data)->part;
}

/** A new title at the end of the disc info (zeroed but for its index);
 *  NULL: out of memory. Strings are the format part's, kept until close;
 *  a title is finished before the next one is added. */
DiscTitleInfo *ff_disc_add_title(AVFormatContext *s);
/** A new stream (zeroed, no linked streams) / chapter / file at the end of
 *  title t; NULL / < 0: out of memory. */
DiscStreamInfo *ff_disc_title_add_stream(DiscTitleInfo *t);
DiscChapterInfo *ff_disc_title_add_chapter(DiscTitleInfo *t);
int ff_disc_title_add_file(DiscTitleInfo *t, const char *name);

extern const DiscFormat ff_disc_hddvd;

#endif /* AVFORMAT_DISC_H */
