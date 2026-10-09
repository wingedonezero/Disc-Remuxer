/*
 * Disc info: what the disc demuxer ("disc") knows about the disc it opened,
 * for the caller's overview of the disc. The disc (kind, label, file
 * system), each title of its title list (name, length, size, the files it
 * plays, chapters) and each title's streams (kind, codec, language and the
 * stream details). It is filled on open whichever title is opened; with the
 * option title = -1 the disc is only described: no title is opened and
 * nothing is read.
 *
 * Separate from the streams a title is ripped from (AVStream): this only
 * describes the disc. A value of 0 (a number) or NULL (a string) is not
 * known; the stream details (bit rate, channels, sample rate, frame size,
 * aspect, frame rate) are not filled yet (they come from reading the start
 * of each title, still to be built).
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

#ifndef AVFORMAT_DISCINFO_H
#define AVFORMAT_DISCINFO_H

#include <stdint.h>

#include "libavcodec/codec_id.h"
#include "libavutil/avutil.h"
#include "libavutil/rational.h"

#include "avformat.h"

/** A stream of a title. */
/* DiscStreamInfo.roles: what the disc says a stream is for */
#define DISC_ROLE_COMMENTARY        0x0001
#define DISC_ROLE_ALT_COMMENTARY    0x0002
#define DISC_ROLE_VISUALLY_IMPAIRED 0x0004
#define DISC_ROLE_LARGE             0x0008
#define DISC_ROLE_CHILDREN          0x0010
#define DISC_ROLE_FORCED            0x1000  /**< a stream of forced subtitles (not DiscStreamInfo.forced) */

typedef struct DiscStreamInfo {
    enum AVMediaType type;
    enum AVCodecID   codec;          /**< AV_CODEC_ID_NONE: no codec for the disc's stream */
    char             language[4];    /**< ISO 639-2, "" when the disc names none */
    int              inner_track;    /**< the stream taken out of this one (DTS out of DTS-HD MA), -1 none */
    int              from_track;     /**< the stream this one is taken out of, -1 none */
    int              forced;         /**< subtitles: only the forced ones (taken out of from_track) */
    int              second_view;    /**< video: the second view of a 3-D video (from_track is the first) */
    int              second_layer;   /**< video: a second (enhancement) layer on its own PID is read with it */
    int              offset_sequence;/**< subtitles of a 3-D title: the disc's offset sequence for its depth, -1 none */
    int              roles;          /**< DISC_ROLE_*, 0 none */
    int              width, height;  /**< video: the frame; subtitles: the canvas the units are drawn on */
    /* the stream details (from the stream itself; 0 / unknown when not read) */
    AVRational       aspect;         /**< video: display aspect ratio */
    AVRational       frame_rate;     /**< video */
    int              profile;        /**< FFmpeg's profile of the codec (AV_PROFILE_*), AV_PROFILE_UNKNOWN */
    int              level;          /**< video: the codec's level (level_idc), AV_LEVEL_UNKNOWN */
    char             dolby_vision[40];/**< video: the Dolby Vision type ("dvhe.07.06 BL+FEL+RPU"), "" none */
    int              channels;       /**< audio */
    uint64_t         layout;         /**< audio: channel layout mask (FFmpeg's AV_CH_*), 0 unknown */
    int              sample_rate;    /**< audio, Hz */
    int              bits;           /**< audio: bits per sample the stream carries (0 unknown) */
    int64_t          bit_rate;       /**< bit/s (0 unknown) */
} DiscStreamInfo;

/** A chapter of a title, as the disc records it. */
typedef struct DiscChapterInfo {
    int64_t          start;          /**< AV_TIME_BASE, from the title's start */
    const char      *name;           /**< NULL when the disc names none */
} DiscChapterInfo;

/** A part of a title that plays whole stream files back to back (the
 *  episodes of a series in one title): what the option part opens on its
 *  own, as if it were a title of its own. */
typedef struct DiscPartInfo {
    int64_t          start;          /**< AV_TIME_BASE, from the title's start */
    int64_t          duration;       /**< AV_TIME_BASE */
    int              first_file;     /**< its files: files[first_file .. first_file + nb_files) of the title */
    int              nb_files;
    int              nb_chapters;    /**< its own chapters (from its own start) */
} DiscPartInfo;

/** A title of the disc's title list. */
typedef struct DiscTitleInfo {
    int              index;          /**< its number in the title list (the option title), from 0 */
    const char      *name;           /**< NULL when the disc names none */
    const char      *source;         /**< what the title is made from (e.g. "00800.mpls(1)"), NULL when not told */
    int              angle;          /**< its angle (1-based) when the disc has several, else 0 */
    char             language[4];    /**< the title's language (ISO 639-2), "" when none */
    int64_t          duration;       /**< AV_TIME_BASE, as the disc records it */
    int64_t          size;           /**< bytes of the files it plays */
    int              selected;       /**< 0: listed only (shorter than the minimum length) */
    int              nb_files;
    const char     **files;          /**< the disc's files it plays, in order (e.g. "DEL01.EVO") */
    int              nb_chapters;
    DiscChapterInfo *chapters;
    int              nb_streams;
    DiscStreamInfo  *streams;
    int              nb_parts;       /**< 0: the title is one piece */
    DiscPartInfo    *parts;
} DiscTitleInfo;

/** The disc. */
typedef struct DiscInfo {
    const char      *kind;           /**< "HD DVD", ... */
    const char      *label;          /**< the volume label */
    const char      *name;           /**< the disc's own name (e.g. a Blu-ray disc library title), NULL when none */
    const char      *filesystem;     /**< the file system read, e.g. "UDF (NetBSD)" */
    int              udf_revision;   /**< its UDF revision (0x0250 = 2.50), 0 when not UDF */
    int              encrypted;
    int              nb_titles;
    DiscTitleInfo   *titles;
} DiscInfo;

/**
 * The disc info of s, opened with the disc demuxer; NULL for any other
 * context. Valid until s is closed.
 */
const DiscInfo *disc_info(const AVFormatContext *s);

#endif /* AVFORMAT_DISCINFO_H */
