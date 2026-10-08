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

struct DRTitleTrack;

/** What of a Blu-ray clip's stream data a track takes: the payloads of
 *  its PID with one of its PES stream keys (stream_id, or 0xFD |
 *  stream_id_extension << 8; key 0: any key of the PID). A track with two
 *  keys takes both in the order they arrive (a core frame and its
 *  extension: they alternate, the first key first). */
typedef struct DiscTsFeed {
    int      pid;
    uint16_t key[2];
    int      nb_keys;
} DiscTsFeed;

/** What the title reader needs of the opened title; the format part fills
 *  DiscContext.src in its open (the arrays and strings stay its own until
 *  its close). Track i is stream i; track 0 is the video. Program-stream
 *  clips (ts 0): 2048-byte sectors through FFmpeg's MPEG-PS demuxer; a
 *  track takes the payloads mpeg.c gives the id of its stream (the start
 *  code 0x1xx of an MPEG stream, the sub-stream id of a private stream 1
 *  stream, 0xFD55 for VC-1). Blu-ray clips (ts 1): 6144-byte units of
 *  192-byte source packets through FFmpeg's MPEG-TS demuxer in disc mode;
 *  a track takes what its feed says. */
typedef struct DiscTitleSource {
    int          nb_clips;
    const char  *clip_kind;                                  /**< for the log: "EVOB" */
    const char *(*clip_name)(AVFormatContext *s, int clip);
    uint32_t    (*clip_blocks)(AVFormatContext *s, int clip);  /**< blocks of the clip (2048 / 6144 bytes) */
    /** Block of the clip into buf (decrypted): 1 read, 0 not usable (left
     *  out), < 0 cannot be read (the rest of the clip is not read). */
    int         (*read_block)(AVFormatContext *s, int clip, uint32_t block, uint8_t *buf);

    int          ts;                /**< Blu-ray stream data (6144-byte units, MPEG-TS) */
    const int   *pids;              /**< ts: the PIDs read */
    int          nb_pids;
    const DiscTsFeed *feeds;        /**< ts: what each track takes */

    int          lpcm_header;       /**< bytes of a PES packet's LPCM audio frame header (HD DVD 5) */
    int64_t      tolerance;         /**< audio lead-in tolerance (ticks; the format's own) */
    const struct DRTitleTrack *tracks;
    int          nb_tracks;
    const int64_t     *chapter_times;   /**< chapter records (ticks from the title start) */
    const char *const *chapter_names;   /**< their names (NULL or "": none), or NULL */
    int          nb_chapters;
} DiscTitleSource;

/** One disc format's part. */
typedef struct DiscFormat {
    const char             *name;       /**< the disc's kind for the caller ("HD DVD") */
    enum DiscIODiscFormat   kind;
    size_t                  priv_size;  /**< the part's own context, zeroed before open */
    /** Read the disc's structure from DiscContext.fs, and give the disc
     *  info's titles (DiscContext.info, ff_disc_add_title...); unless option
     *  title is -1 (only the disc info, no packets), open that title (with
     *  option part >= 0: that part of it alone, a part the disc info lists):
     *  its streams (docs/FORMATS.md) and DiscContext.src, which the disc
     *  layer's title reader then reads. */
    int  (*open)(AVFormatContext *s);
    /** Also after a failed open (the context as far as open came). */
    void (*close)(AVFormatContext *s);
} DiscFormat;

typedef struct DiscTitleReader DiscTitleReader;

/** The disc demuxer's context (AVFormatContext.priv_data). */
typedef struct DiscContext {
    const AVClass    *class;

    /* options, for every format part */
    int               opt_read_attempts;
    int               opt_udf_reader;
    int               opt_title;        /**< -1: every title listed, none read */
    int               opt_part;         /**< -1: the whole title, else that part of it (DiscTitleInfo.parts) */
    int               opt_second_view;  /**< 3-D titles: the second view is read (a track of its own) */
    int               opt_details;      /**< the title list (info) reads each track's details from the stream */
    int               opt_dolby_vision; /**< Dolby Vision enhancement layer: 0 merged into the video's frames,
                                             1 a track of its own */
    int               opt_min_length;
    int               opt_dvd_cell_mode;  /**< DVD: a title's cells (DVD_CELL_MODE_*) */
    int               opt_dvd_cell_check; /**< DVD: the VOBUs a full cell check reads (DVD_CELL_CHECK_*) */
    char            **opt_keydb;
    unsigned          nb_opt_keydb;
    int64_t           progress;         /**< exported: how far the title is read (1/10000) */

    /* the disc */
    int               dvd_flat;         /**< DVD: VIDEO_TS.IFO at the top of the folder (a VIDEO_TS or .dvdmedia
                                             folder opened itself) */
    DiscIOSource     *image;
    DiscIOFS         *fs;
    const DiscFormat *fmt;
    void             *part;             /**< the format part's context */
    DiscInfo          info;             /**< the disc info (disc_info()) */

    /* the opened title */
    DiscTitleSource   src;              /**< filled by the format part (nb_clips 0: no title) */
    DiscTitleReader  *reader;
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
DiscPartInfo *ff_disc_title_add_part(DiscTitleInfo *t);
int ff_disc_title_add_file(DiscTitleInfo *t, const char *name);

/* ---- disctitle.c ---- */

/** A clip through FFmpeg's demuxer as a disc reader reads it, from the
 *  blocks read gives (one per call; AVERROR_EOF at the end): raw PES
 *  payloads with their own times (no parsing, no filled-in or corrected
 *  timestamps). parent (may be NULL): the context whose whitelists and
 *  interrupt callback the demuxer takes. */
typedef struct DiscDemux {
    AVFormatContext *ctx;
    AVIOContext     *pb;
} DiscDemux;

/** MPEG-PS (mpeg.c, option disc): 2048-byte sectors, the format's LPCM
 *  header size. */
int  ff_disc_ps_open(DiscDemux *dm, AVFormatContext *parent, int lpcm_header,
                     int (*read)(void *opaque, uint8_t *buf, int size), void *opaque);
/** MPEG-TS (mpegts.c, option disc): 6144-byte units of source packets, the
 *  PIDs read; each payload's stream id is PID | PES stream key << 16. */
int  ff_disc_ts_open(DiscDemux *dm, AVFormatContext *parent, const int *pids, int nb_pids,
                     int (*read)(void *opaque, uint8_t *buf, int size), void *opaque);
void ff_disc_demux_close(DiscDemux *dm);

/** The title reader (DiscContext.reader) over src: the title timeline for
 *  its tracks and chapters; reading starts at its first clip. */
int  ff_disc_title_reader_open(AVFormatContext *s, const DiscTitleSource *src);
/** The next packet: a frame of the title timeline; at the end (AVERROR_EOF)
 *  the chapters, s->duration and each stream's result are set. */
int  ff_disc_title_reader_packet(AVFormatContext *s, AVPacket *pkt);
void ff_disc_title_reader_close(DiscTitleReader **reader);

extern const DiscFormat ff_disc_dvd;
extern const DiscFormat ff_disc_hddvd;
extern const DiscFormat ff_disc_bluray;

#endif /* AVFORMAT_DISC_H */
