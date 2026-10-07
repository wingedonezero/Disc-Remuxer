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
typedef struct DiscStreamInfo {
    enum AVMediaType type;
    enum AVCodecID   codec;          /**< AV_CODEC_ID_NONE: no codec for the disc's stream */
    char             language[4];    /**< ISO 639-2, "" when the disc names none */
    int              inner_track;    /**< the stream taken out of this one (DTS out of DTS-HD MA), -1 none */
    int              from_track;     /**< the stream this one is taken out of, -1 none */
    int              width, height;  /**< video: the frame; subtitles: the canvas the units are drawn on */
    /* the stream details (not filled yet) */
    AVRational       aspect;         /**< video: display aspect ratio */
    AVRational       frame_rate;     /**< video */
    int              channels;       /**< audio */
    int              sample_rate;    /**< audio, Hz */
    int              bits;           /**< audio: bits per sample (PCM) */
    int64_t          bit_rate;       /**< bit/s */
} DiscStreamInfo;

/** A chapter of a title, as the disc records it. */
typedef struct DiscChapterInfo {
    int64_t          start;          /**< AV_TIME_BASE, from the title's start */
    const char      *name;           /**< NULL when the disc names none */
} DiscChapterInfo;

/** A title of the disc's title list. */
typedef struct DiscTitleInfo {
    int              index;          /**< its number in the title list (the option title), from 0 */
    const char      *name;           /**< NULL when the disc names none */
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
} DiscTitleInfo;

/** The disc. */
typedef struct DiscInfo {
    const char      *kind;           /**< "HD DVD", ... */
    const char      *label;          /**< the volume label */
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
