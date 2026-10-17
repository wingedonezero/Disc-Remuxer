/*
 * The names of a track's codec and its details as players and MediaInfo
 * show them (Mpeg4 AVC High@L4.1, DTS-HD Master Audio, 23.976 (24000/1001),
 * 5.1(side), 640 Kb/s), from the title list's stream details.
 */
#ifndef DISC_REMUXER_CODECS_H
#define DISC_REMUXER_CODECS_H

#include <stddef.h>

#include "disc/info.h"

/** The codec's full name with its profile, level and Dolby Vision type (base: a
 *  3-D second view's base view, else NULL). */
const char *codec_description(const DiscStreamInfo *k, const DiscStreamInfo *base, char *buf, size_t size);
/** A codec id's name as the processing library spells it ("ac3",
 *  "pcm_s24le", "hdmv_pgs_subtitle", ...). */
const char *codec_id_name(int id);
/** The codec's short name (DTS-HD MA, DTS:X, DD+ Atmos, TrueHD, MpegH, ...). */
const char *codec_short(const DiscStreamInfo *k);
/** "High@L4.1" / "Main10@L5.1", "" when not known. */
const char *codec_profile_level(const DiscStreamInfo *k, char *buf, size_t size);
/** "23.976 (24000/1001)", "" when not known. */
const char *frame_rate_text(const DiscStreamInfo *k, char *buf, size_t size);
/** "16:9", "4:3", else the ratio; "" when not known. */
const char *aspect_text(const DiscStreamInfo *k, char *buf, size_t size);
/** "640 Kb/s", "1.5 Mb/s", "" when not known. */
const char *bit_rate_text(int64_t b, char *buf, size_t size);
/** "5.1(side)", "stereo", "" when not known. */
const char *layout_text(const DiscStreamInfo *k, char *buf, size_t size);
/** Whether the codec keeps every sample (its bit depth is worth showing). */
int codec_lossless(const DiscStreamInfo *k);
/* An audio track's name by its channels, as the reference names them:
 * "Mono", "Stereo", "Surround 5.1" (the speakers but the LFE ones, then
 * the LFE ones); "" for other tracks. */
const char *audio_name(const DiscStreamInfo *k, char *buf, size_t size);

#endif
