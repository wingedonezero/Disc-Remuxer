/*
 * The disc demuxer's calls on an opened disc (the model itself is
 * disc/info.h): the disc info, opening and closing a title, language and
 * Matroska codec names.
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

#include "disc/info.h"

#include "avformat.h"

/**
 * The disc info of s, opened with the disc demuxer; NULL for any other
 * context. Valid until s is closed.
 */
const DiscInfo *disc_info(const AVFormatContext *s);

/**
 * Open title `title` of the disc s (opened with the disc demuxer, option
 * title -1: the disc info only) for reading: the title before it is closed
 * first. The disc stays loaded: its title list and each title's stream
 * pass are not done again. part >= 0: only that part of the title
 * (DiscTitleInfo.parts); second_view: a 3-D title's second view is read
 * too. keep: NULL every track, else keep[i] 0 leaves track i of the title's
 * streams (DiscTitleInfo.streams) out: not read, no stream (track 0, the
 * video, is always read; the second view only when one of its tracks is
 * kept). Its streams, chapters (at the end) and s->duration are the title's;
 * av_read_frame() gives its packets. Returns 0 or an AVERROR (the title
 * is then closed).
 */
int disc_title_open(AVFormatContext *s, int title, int part, int second_view, const uint8_t *keep);

/**
 * Close the title opened on s: its streams, chapters and results are
 * removed; the disc stays loaded. Nothing to do when none is open.
 */
void disc_title_close(AVFormatContext *s);

/**
 * The ISO 639-2 bibliographic code the disc info uses for a language code
 * (2 or 3 letters, either ISO 639-2 form), NULL when not known.
 */
const char *disc_language(const char *code);

/**
 * The English name of a language code ("eng": "English"), NULL when not
 * known.
 */
const char *disc_language_name(const char *code);

/**
 * The Matroska codec ID a stream is stored with in a Matroska file
 * ("V_MPEG2", "A_AC3", "S_HDMV/PGS", ...): LPCM as PCM ("A_PCM/INT/LIT"),
 * closed captions as text ("S_TEXT/UTF8"), a 3-D second view as
 * "V_MPEG4/ISO/MVC"; NULL when Matroska has none.
 */
const char *disc_mkv_codec_id(const DiscStreamInfo *k);

#endif /* AVFORMAT_DISCINFO_H */
