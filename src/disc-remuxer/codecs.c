/*
 * The names of a track's codec and its details as players and MediaInfo
 * show them.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/defs.h>
#include <libavutil/channel_layout.h>

#include "codecs.h"
#include "writer.h"

const char *codec_short(const DiscStreamInfo *k)
{
    if (k->codec == AV_CODEC_ID_TRUEHD)
        return "TrueHD";
    if (k->codec == AV_CODEC_ID_DTS && k->profile == AV_PROFILE_DTS_EXPRESS)
        return "DTS Express";
    if (k->codec == AV_CODEC_ID_H264 && k->second_view)
        return "Mpeg4-MVC-3D";          /* a 3-D title's second view (MVC) */
    return codec_label(k->codec, k->profile);
}

const char *codec_profile_level(const DiscStreamInfo *k, char *buf, size_t size)
{
    const char *p = k->profile != AV_PROFILE_UNKNOWN ? avcodec_profile_name(k->codec, k->profile) : NULL;
    size_t n = 0;

    *buf = 0;
    if (!p || (k->codec != AV_CODEC_ID_H264 && k->codec != AV_CODEC_ID_HEVC))
        return buf;
    for (; *p && n + 1 < size; p++)     /* "Main 10" -> "Main10" */
        if (*p != ' ')
            buf[n++] = *p;
    buf[n] = 0;
    if (k->level > 0 && k->level != AV_LEVEL_UNKNOWN) {
        /* level_idc: H.264 10 x the level, H.265 30 x the level */
        int major = k->codec == AV_CODEC_ID_HEVC ? k->level / 30 : k->level / 10;
        int minor = k->codec == AV_CODEC_ID_HEVC ? k->level % 30 / 3 : k->level % 10;
        snprintf(buf + n, size - n, "@L%d.%d", major, minor);
    }
    return buf;
}

const char *codec_description(const DiscStreamInfo *k, const DiscStreamInfo *base, char *buf, size_t size)
{
    char pl[48], bl[48];
    const char *name;

    codec_profile_level(k, pl, sizeof(pl));
    switch (k->codec) {
    case AV_CODEC_ID_H264:
        if (k->second_view && base && *codec_profile_level(base, bl, sizeof(bl)))
            snprintf(buf, size, "Mpeg4 MVC %s%s%s", bl, *pl ? "/" : "", pl);     /* base view / stereo */
        else
            snprintf(buf, size, "%s%s%s", k->second_view ? "Mpeg4 MVC" : "Mpeg4 AVC", *pl ? " " : "", pl);
        return buf;
    case AV_CODEC_ID_HEVC:
        snprintf(buf, size, "MpegH HEVC%s%s%s%s%s", *pl ? " " : "", pl, k->dolby_vision[0] ? " (" : "",
                 k->dolby_vision, k->dolby_vision[0] ? ")" : "");
        return buf;
    case AV_CODEC_ID_AC3:    name = "Dolby Digital"; break;
    case AV_CODEC_ID_EAC3:   name = "Dolby Digital plus"; break;
    case AV_CODEC_ID_DTS:
        name = k->profile == AV_PROFILE_DTS_HD_MA || k->profile == AV_PROFILE_DTS_HD_MA_X ||
               k->profile == AV_PROFILE_DTS_HD_MA_X_IMAX ? "DTS-HD Master Audio" :
               k->profile == AV_PROFILE_DTS_HD_HRA ? "DTS-HD High resolution audio" :
               k->profile == AV_PROFILE_DTS_EXPRESS ? "DTS Express" : "DTS";
        break;
    case AV_CODEC_ID_TRUEHD: name = k->profile == AV_PROFILE_TRUEHD_ATMOS ? "TrueHD Atmos" : "TrueHD"; break;
    case AV_CODEC_ID_MLP:    name = "MLP"; break;
    case AV_CODEC_ID_PCM_BLURAY:
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE: name = "Linear PCM"; break;
    case AV_CODEC_ID_MP1:    name = "MPEG audio layer 1"; break;
    case AV_CODEC_ID_MP2:    name = "MPEG audio layer 2"; break;
    case AV_CODEC_ID_MP3:    name = "MPEG audio layer 3"; break;
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:  name = "HDMV PGS Subtitles"; break;
    case AV_CODEC_ID_HDMV_TEXT_SUBTITLE: name = "HDMV Text Subtitles"; break;
    case AV_CODEC_ID_DVD_SUBTITLE:       name = "Dvd Subtitles"; break;
    case AV_CODEC_ID_EIA_608:            name = "Closed Captions"; break;
    default:                 name = codec_short(k);
    }
    snprintf(buf, size, "%s", name);
    return buf;
}

const char *frame_rate_text(const DiscStreamInfo *k, char *buf, size_t size)
{
    int num = k->frame_rate.num, den = k->frame_rate.den, frac;

    *buf = 0;
    if (num <= 0 || den <= 0)
        return buf;
    if (den == 1) {
        snprintf(buf, size, "%d", num);
        return buf;
    }
    frac = (int)((int64_t)(num % den) * 10000 / den);
    snprintf(buf, size, "%d.%04d", num / den, frac);
    for (size_t n = strlen(buf); n > 0 && buf[n - 1] == '0' && buf[n - 2] != '.'; n--)
        buf[n - 1] = 0;
    snprintf(buf + strlen(buf), size - strlen(buf), " (%d/%d)", num, den);
    return buf;
}

const char *aspect_text(const DiscStreamInfo *k, char *buf, size_t size)
{
    int r;

    *buf = 0;
    if (k->aspect.num <= 0 || k->aspect.den <= 0)
        return buf;
    /* 1.818 and 1.364: a whole 720-sample line of a 16:9 and of a 4:3
     * picture with horizontal overscan (pixels 16:11 / 40:33 and 12:11 /
     * 10:11, H.264 and H.265 Table E-1), which a Blu-ray clip's attributes
     * call 16:9 and 4:3 */
    r = (int)((int64_t)k->aspect.num * 1000 / k->aspect.den);
    if ((r >= 1776 && r <= 1778) || (r >= 1815 && r <= 1819))
        snprintf(buf, size, "16:9");
    else if ((r >= 1333 && r <= 1335) || (r >= 1362 && r <= 1364) || (r >= 1499 && r <= 1501))
        snprintf(buf, size, "4:3");
    else
        snprintf(buf, size, "%d:%d", k->aspect.num, k->aspect.den);
    return buf;
}

const char *bit_rate_text(int64_t b, char *buf, size_t size)
{
    *buf = 0;
    if (b <= 0)
        return buf;
    if (b < 1000)
        snprintf(buf, size, "%"PRId64" b/s", b);
    else if (b < 1000000)
        snprintf(buf, size, b % 1000 ? "%"PRId64".%"PRId64" Kb/s" : "%"PRId64" Kb/s", b / 1000, b / 100 % 10);
    else
        snprintf(buf, size, b % 1000000 ? "%"PRId64".%"PRId64" Mb/s" : "%"PRId64" Mb/s", b / 1000000,
                 b / 100000 % 10);
    return buf;
}

const char *layout_text(const DiscStreamInfo *k, char *buf, size_t size)
{
    AVChannelLayout l = { 0 };

    *buf = 0;
    if (k->layout && av_channel_layout_from_mask(&l, k->layout) >= 0)
        av_channel_layout_describe(&l, buf, size);
    else if (k->channels)
        snprintf(buf, size, "%d channels", k->channels);
    av_channel_layout_uninit(&l);
    return buf;
}

int codec_lossless(const DiscStreamInfo *k)
{
    switch (k->codec) {
    case AV_CODEC_ID_TRUEHD:
    case AV_CODEC_ID_MLP:
    case AV_CODEC_ID_PCM_BLURAY:
    case AV_CODEC_ID_PCM_DVD:
    case AV_CODEC_ID_PCM_S16LE:
    case AV_CODEC_ID_PCM_S24LE: return 1;
    case AV_CODEC_ID_DTS:       return k->profile == AV_PROFILE_DTS_HD_MA || k->profile == AV_PROFILE_DTS_HD_MA_X ||
                                       k->profile == AV_PROFILE_DTS_HD_MA_X_IMAX;
    default:                    return 0;
    }
}

const char *audio_name(const DiscStreamInfo *k, char *buf, size_t size)
{
    uint64_t lay = k->layout;
    int lfe;

    *buf = 0;
    if (k->type != AVMEDIA_TYPE_AUDIO || !k->channels)
        return buf;
    if (!lay)
        snprintf(buf, size, "%s", k->channels == 1 ? "Mono" : k->channels == 2 ? "Stereo" : "Surround");
    else if (lay == AV_CH_LAYOUT_STEREO)
        snprintf(buf, size, "Stereo");
    else if (lay == AV_CH_LAYOUT_MONO)
        snprintf(buf, size, "Mono");
    else {
        lfe = !!(lay & AV_CH_LOW_FREQUENCY) + !!(lay & AV_CH_LOW_FREQUENCY_2);
        snprintf(buf, size, "Surround %d.%d", av_popcount64(lay) - lfe, lfe);
    }
    return buf;
}
