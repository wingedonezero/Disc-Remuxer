/*
 * The names of a track's codec and its details as players and MediaInfo
 * show them.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "codecs.h"
#include "timeline/disctl.h"
#include "writer.h"

/* ---- the names the players and MediaInfo use (the processing library's
 * wording, so the details read as they always did) ---- */

typedef struct ProfileName {
    int         profile;
    const char *name;
} ProfileName;

static const ProfileName h264_profiles[] = {
    { DISC_PROFILE_H264_BASELINE,                                     "Baseline" },
    { DISC_PROFILE_H264_BASELINE | DISC_PROFILE_H264_CONSTRAINED,     "Constrained Baseline" },
    { DISC_PROFILE_H264_MAIN,                                         "Main" },
    { DISC_PROFILE_H264_EXTENDED,                                     "Extended" },
    { DISC_PROFILE_H264_HIGH,                                         "High" },
    { DISC_PROFILE_H264_HIGH_10,                                      "High 10" },
    { DISC_PROFILE_H264_HIGH_10 | DISC_PROFILE_H264_INTRA,            "High 10 Intra" },
    { DISC_PROFILE_H264_HIGH_422,                                     "High 4:2:2" },
    { DISC_PROFILE_H264_HIGH_422 | DISC_PROFILE_H264_INTRA,           "High 4:2:2 Intra" },
    { DISC_PROFILE_H264_HIGH_444,                                     "High 4:4:4" },
    { DISC_PROFILE_H264_HIGH_444_PREDICTIVE,                          "High 4:4:4 Predictive" },
    { DISC_PROFILE_H264_HIGH_444_PREDICTIVE | DISC_PROFILE_H264_INTRA, "High 4:4:4 Intra" },
    { DISC_PROFILE_H264_CAVLC_444,                                    "CAVLC 4:4:4" },
    { DISC_PROFILE_H264_MULTIVIEW_HIGH,                               "Multiview High" },
    { DISC_PROFILE_H264_STEREO_HIGH,                                  "Stereo High" },
    { 0, NULL },
};

static const ProfileName hevc_profiles[] = {
    { DISC_PROFILE_HEVC_MAIN,               "Main" },
    { DISC_PROFILE_HEVC_MAIN_10,            "Main 10" },
    { DISC_PROFILE_HEVC_MAIN_STILL_PICTURE, "Main Still Picture" },
    { DISC_PROFILE_HEVC_REXT,               "Rext" },
    { DISC_PROFILE_HEVC_MULTIVIEW_MAIN,     "Multiview Main" },
    { DISC_PROFILE_HEVC_SCC,                "Scc" },
    { 0, NULL },
};

/* An H.264 / H.265 profile's name, NULL for other codecs or one not named. */
static const char *profile_name(int codec, int profile)
{
    const ProfileName *p = codec == DR_CODEC_H264 ? h264_profiles : codec == DR_CODEC_HEVC ? hevc_profiles : NULL;

    for (; p && p->name; p++)
        if (p->profile == profile)
            return p->name;
    return NULL;
}

/* The speakers by their bit in a layout mask (DiscStreamInfo.layout). */
static const char *const speakers[64] = {
    [0]  = "FL",  [1]  = "FR",  [2]  = "FC",  [3]  = "LFE", [4]  = "BL",  [5]  = "BR",  [6]  = "FLC",
    [7]  = "FRC", [8]  = "BC",  [9]  = "SL",  [10] = "SR",  [11] = "TC",  [12] = "TFL", [13] = "TFC",
    [14] = "TFR", [15] = "TBL", [16] = "TBC", [17] = "TBR", [29] = "DL",  [30] = "DR",  [31] = "WL",
    [32] = "WR",  [33] = "SDL", [34] = "SDR", [35] = "LFE2", [36] = "TSL", [37] = "TSR", [38] = "BFC",
    [39] = "BFL", [40] = "BFR", [41] = "SSL", [42] = "SSR", [43] = "TTL", [44] = "TTR", [61] = "BIL",
    [62] = "BIR",
};

/* The layouts with a name (the first one a mask equals names it). */
static const struct { uint64_t mask; const char *name; } layouts[] = {
    { 0x0000000000000004ULL, "mono" },
    { 0x0000000000000003ULL, "stereo" },
    { 0x000000000000000BULL, "2.1" },
    { 0x0000000000000007ULL, "3.0" },
    { 0x0000000000000103ULL, "3.0(back)" },
    { 0x0000000000000107ULL, "4.0" },
    { 0x0000000000000033ULL, "quad" },
    { 0x0000000000000603ULL, "quad(side)" },
    { 0x000000000000000FULL, "3.1" },
    { 0x0000000000000037ULL, "5.0" },
    { 0x0000000000000607ULL, "5.0(side)" },
    { 0x000000000000010FULL, "4.1" },
    { 0x000000000000003FULL, "5.1" },
    { 0x000000000000060FULL, "5.1(side)" },
    { 0x0000000000000707ULL, "6.0" },
    { 0x00000000000006C3ULL, "6.0(front)" },
    { 0x000000000000500FULL, "3.1.2" },
    { 0x0000000000000137ULL, "hexagonal" },
    { 0x000000000000070FULL, "6.1" },
    { 0x000000000000013FULL, "6.1(back)" },
    { 0x00000000000006CBULL, "6.1(front)" },
    { 0x0000000000000637ULL, "7.0" },
    { 0x00000000000006C7ULL, "7.0(front)" },
    { 0x000000000000063FULL, "7.1" },
    { 0x00000000000000FFULL, "7.1(wide)" },
    { 0x00000000000006CFULL, "7.1(wide-side)" },
    { 0x000000000000560FULL, "5.1.2" },
    { 0x000000000000503FULL, "5.1.2(back)" },
    { 0x0000000000000737ULL, "octagonal" },
    { 0x000000000002D033ULL, "cube" },
    { 0x000000000002D60FULL, "5.1.4" },
    { 0x000000000000563FULL, "7.1.2" },
    { 0x000000000002D63FULL, "7.1.4" },
    { 0x000000080001563FULL, "7.2.3" },
    { 0x000000000002D6FFULL, "9.1.4" },
    { 0x000000300002D6FFULL, "9.1.6" },
    { 0x000000018003F737ULL, "hexadecagonal" },
    { 0x6000000000000000ULL, "binaural" },
    { 0x0000000060000000ULL, "downmix" },
    { 0x000001F80003FFFFULL, "22.2" },
};

const char *codec_id_name(int id)
{
    switch (id) {
    case DR_CODEC_NONE:               return "none";
    case DR_CODEC_PCM_S16LE:          return "pcm_s16le";
    case DR_CODEC_PCM_S24LE:          return "pcm_s24le";
    case DR_CODEC_EIA_608:            return "eia_608";
    default: {
        const char *n = ff_disctl_codec_id_name(id);
        return strcmp(n, "?") ? n : "unknown_codec";
    }
    }
}

const char *codec_short(const DiscStreamInfo *k)
{
    if (k->codec == DR_CODEC_TRUEHD)
        return "TrueHD";
    if (k->codec == DR_CODEC_DTS && k->profile == DISC_PROFILE_DTS_EXPRESS)
        return "DTS Express";
    if (k->codec == DR_CODEC_H264 && k->second_view)
        return "Mpeg4-MVC-3D";          /* a 3-D title's second view (MVC) */
    return codec_label(k->codec, k->profile);
}

const char *codec_profile_level(const DiscStreamInfo *k, char *buf, size_t size)
{
    const char *p = k->profile != DISC_PROFILE_UNKNOWN ? profile_name(k->codec, k->profile) : NULL;
    size_t n = 0;

    *buf = 0;
    if (!p || (k->codec != DR_CODEC_H264 && k->codec != DR_CODEC_HEVC))
        return buf;
    for (; *p && n + 1 < size; p++)     /* "Main 10" -> "Main10" */
        if (*p != ' ')
            buf[n++] = *p;
    buf[n] = 0;
    if (k->level > 0 && k->level != DISC_LEVEL_UNKNOWN) {
        /* level_idc: H.264 10 x the level, H.265 30 x the level */
        int major = k->codec == DR_CODEC_HEVC ? k->level / 30 : k->level / 10;
        int minor = k->codec == DR_CODEC_HEVC ? k->level % 30 / 3 : k->level % 10;
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
    case DR_CODEC_H264:
        if (k->second_view && base && *codec_profile_level(base, bl, sizeof(bl)))
            snprintf(buf, size, "Mpeg4 MVC %s%s%s", bl, *pl ? "/" : "", pl);     /* base view / stereo */
        else
            snprintf(buf, size, "%s%s%s", k->second_view ? "Mpeg4 MVC" : "Mpeg4 AVC", *pl ? " " : "", pl);
        return buf;
    case DR_CODEC_HEVC:
        snprintf(buf, size, "MpegH HEVC%s%s%s%s%s", *pl ? " " : "", pl, k->dolby_vision[0] ? " (" : "",
                 k->dolby_vision, k->dolby_vision[0] ? ")" : "");
        return buf;
    case DR_CODEC_AC3:    name = "Dolby Digital"; break;
    case DR_CODEC_EAC3:
        name = k->profile == DISC_PROFILE_EAC3_DDP_ATMOS ? "Dolby Digital plus + Dolby Atmos" : "Dolby Digital plus";
        break;
    case DR_CODEC_DTS:
        /* DTS:X: DTS-HD MA with the DTS:X extension after its lossless data */
        name = k->profile == DISC_PROFILE_DTS_HD_MA_X ? "DTS-HD Master Audio + DTS:X" :
               k->profile == DISC_PROFILE_DTS_HD_MA_X_IMAX ? "DTS-HD Master Audio + DTS:X IMAX" :
               k->profile == DISC_PROFILE_DTS_HD_MA ? "DTS-HD Master Audio" :
               k->profile == DISC_PROFILE_DTS_HD_HRA ? "DTS-HD High resolution audio" :
               k->profile == DISC_PROFILE_DTS_EXPRESS ? "DTS Express" : "DTS";
        break;
    case DR_CODEC_TRUEHD: name = k->profile == DISC_PROFILE_TRUEHD_ATMOS ? "TrueHD Atmos" : "TrueHD"; break;
    case DR_CODEC_MLP:    name = "MLP"; break;
    case DR_CODEC_PCM_BLURAY:
    case DR_CODEC_PCM_DVD:
    case DR_CODEC_PCM_S16LE:
    case DR_CODEC_PCM_S24LE: name = "Linear PCM"; break;
    case DR_CODEC_MP1:    name = "MPEG audio layer 1"; break;
    case DR_CODEC_MP2:    name = "MPEG audio layer 2"; break;
    case DR_CODEC_MP3:    name = "MPEG audio layer 3"; break;
    case DR_CODEC_HDMV_PGS_SUBTITLE:  name = "HDMV PGS Subtitles"; break;
    case DR_CODEC_HDMV_TEXT_SUBTITLE: name = "HDMV Text Subtitles"; break;
    case DR_CODEC_DVD_SUBTITLE:       name = "Dvd Subtitles"; break;
    case DR_CODEC_EIA_608:            name = "Closed Captions"; break;
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
    uint64_t m = k->layout;
    size_t n = 0;

    *buf = 0;
    if (!m) {
        if (k->channels)
            snprintf(buf, size, "%d channels", k->channels);
        return buf;
    }
    for (size_t i = 0; i < sizeof(layouts) / sizeof(*layouts); i++)
        if (layouts[i].mask == m) {
            snprintf(buf, size, "%s", layouts[i].name);
            return buf;
        }
    /* no name: "3 channels (FL+FR+LFE2)", speakers without a name USR<bit> */
    snprintf(buf, size, "%d channels (", __builtin_popcountll(m));
    for (int b = 0; b < 64; b++) {
        if (!(m >> b & 1))
            continue;
        n = strlen(buf);
        if (speakers[b])
            snprintf(buf + n, size - n, "%s%s", n && buf[n - 1] != '(' ? "+" : "", speakers[b]);
        else
            snprintf(buf + n, size - n, "%sUSR%d", n && buf[n - 1] != '(' ? "+" : "", b);
    }
    n = strlen(buf);
    snprintf(buf + n, size - n, ")");
    return buf;
}

int codec_lossless(const DiscStreamInfo *k)
{
    switch (k->codec) {
    case DR_CODEC_TRUEHD:
    case DR_CODEC_MLP:
    case DR_CODEC_PCM_BLURAY:
    case DR_CODEC_PCM_DVD:
    case DR_CODEC_PCM_S16LE:
    case DR_CODEC_PCM_S24LE: return 1;
    case DR_CODEC_DTS:       return k->profile == DISC_PROFILE_DTS_HD_MA || k->profile == DISC_PROFILE_DTS_HD_MA_X ||
                                       k->profile == DISC_PROFILE_DTS_HD_MA_X_IMAX;
    default:                    return 0;
    }
}

const char *audio_name(const DiscStreamInfo *k, char *buf, size_t size)
{
    uint64_t lay = k->layout;
    int lfe;

    *buf = 0;
    if (k->type != DISC_MEDIA_AUDIO || !k->channels)
        return buf;
    if (!lay)
        snprintf(buf, size, "%s", k->channels == 1 ? "Mono" : k->channels == 2 ? "Stereo" : "Surround");
    else if (lay == DISC_CH_LAYOUT_STEREO)
        snprintf(buf, size, "Stereo");
    else if (lay == DISC_CH_LAYOUT_MONO)
        snprintf(buf, size, "Mono");
    else {
        lfe = !!(lay & DISC_CH_LOW_FREQUENCY) + !!(lay & DISC_CH_LOW_FREQUENCY_2);
        snprintf(buf, size, "Surround %d.%d", __builtin_popcountll(lay) - lfe, lfe);
    }
    return buf;
}

/* The Matroska codec IDs of the codecs that leave a disc (the values the
 * library's Matroska table gives them). */
const char *disc_mkv_codec_id(const DiscStreamInfo *k)
{
    if (k->second_view)
        return "V_MPEG4/ISO/MVC";
    switch (k->codec) {
    case DR_CODEC_PCM_DVD:
    case DR_CODEC_PCM_BLURAY:   return "A_PCM/INT/LIT";    /* written as PCM */
    case DR_CODEC_EIA_608:      return "S_TEXT/UTF8";      /* made into text */
    case DR_CODEC_MPEG1VIDEO:   return "V_MPEG1";
    case DR_CODEC_MPEG2VIDEO:   return "V_MPEG2";
    case DR_CODEC_H264:         return "V_MPEG4/ISO/AVC";
    case DR_CODEC_HEVC:         return "V_MPEGH/ISO/HEVC";
    case DR_CODEC_AC3:          return "A_AC3";
    case DR_CODEC_EAC3:         return "A_EAC3";
    case DR_CODEC_TRUEHD:       return "A_TRUEHD";
    case DR_CODEC_MLP:          return "A_MLP";
    case DR_CODEC_DTS:          return "A_DTS";
    case DR_CODEC_MP1:          return "A_MPEG/L1";
    case DR_CODEC_MP2:          return "A_MPEG/L2";
    case DR_CODEC_MP3:          return "A_MPEG/L3";
    case DR_CODEC_AAC:          return "A_AAC";
    case DR_CODEC_PCM_S16LE:
    case DR_CODEC_PCM_S24LE:    return "A_PCM/INT/LIT";
    case DR_CODEC_DVD_SUBTITLE: return "S_VOBSUB";
    case DR_CODEC_HDMV_PGS_SUBTITLE:  return "S_HDMV/PGS";
    case DR_CODEC_HDMV_TEXT_SUBTITLE: return "S_HDMV/TEXTST";
    case DR_CODEC_SUBRIP:       return "S_TEXT/UTF8";
    default:                       return NULL;                /* Matroska has none (VC-1) */
    }
}
