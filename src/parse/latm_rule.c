/*
 * AAC LATM / LOAS: the disc reader's frame rules
 *
 * The rule the splitter engine runs (parse/split.h); it was once a disc
 * mode of ours in FFmpeg's latm_parser.c.
 */

#include <stdint.h>

#include <stddef.h>

#include "base/err.h"

#include "parse/bits.h"
#include "parse/rules.h"

/* ---- The disc reader's LOAS frame rules ----
 * A frame starts with the LOAS sync (11 bits, 0x2B7) and is as long as its
 * audioMuxLengthBytes + 3. Where no frame starts, the next one is a sync
 * whose next sync (its length on) is there; with more than 0x20000 bytes
 * and none, the kept bytes are given up. (The reference program also reads
 * each frame's AudioMuxElement; not done here: the frame's stream values
 * come from dr_latm_header below.) */

#define DISC_GIVE_UP 0x20000

static int disc_loas(const uint8_t *b)
{
    return b[0] == 0x56 && (b[1] & 0xe0) == 0xe0;
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    if (len < 3)
        return 2;
    return disc_loas(b);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    if (len < 3)
        return -1;
    if (!disc_loas(b))
        return -2;
    return ((b[1] & 0x1f) << 8 | b[2]) + 3;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    return 1;
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 3)
        return -2;
    for (int pos = 0; pos < len - 3; pos++) {
        int n;
        if (!disc_loas(b + pos))
            continue;
        n = (b[pos + 1] & 0x1f) << 8 | b[pos + 2];
        if (len < pos + n + 6)
            break;                          /* the next sync is not there yet */
        if (disc_loas(b + pos + n + 3))
            return pos;
    }
    return len > DISC_GIVE_UP ? -1 : -2;
}

const DrFrameRule dr_latm_frames = { disc_sync, disc_size, disc_check, disc_resync, DISC_GIVE_UP };

/* ---- The stream values of a frame with a StreamMuxConfig ----
 * Ported from FFmpeg 9.0.2's libavcodec/aac/aacdec_latm.h
 * (read_stream_mux_config, latm_get_value) and mpeg4audio.c
 * (ff_mpeg4audio_get_config_gb, get_object_type, get_sample_rate): the
 * AudioSpecificConfig (ISO/IEC 14496-3 1.6.2.1) as far as it decides the
 * output sample rate and the samples per frame. */

enum {
    AOT_AAC_MAIN = 1, AOT_AAC_LC, AOT_AAC_SSR, AOT_AAC_LTP, AOT_SBR, AOT_ER_AAC_LC = 17, AOT_ER_AAC_LD = 23,
    AOT_PS = 29, AOT_ESCAPE = 31, AOT_ER_AAC_ELD = 39, AOT_USAC = 42,
};

static const int mpeg4audio_rates[16] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

static int object_type(DrBits *g)
{
    int t = dr_bits_get(g, 5);
    return t == AOT_ESCAPE ? 32 + (int)dr_bits_get(g, 6) : t;
}

static int sample_rate(DrBits *g)
{
    int i = dr_bits_get(g, 4);
    return i == 0x0f ? (int)dr_bits_get(g, 24) : mpeg4audio_rates[i];
}

static uint32_t latm_value(DrBits *g)
{
    int bytes = dr_bits_get(g, 2) + 1;
    return dr_bits_get(g, 8 * bytes);
}

static unsigned peek(const DrBits *g, int n)
{
    DrBits c = *g;
    return dr_bits_get(&c, n);
}

int dr_latm_header(const uint8_t *buf, int size, int *rate, int *samples)
{
    DrBits g;
    int version, asc_len = 0, end, aot, core, sbr = -1, ext = 0, ext_rate = 0, frame;

    if (size < 4 || !disc_loas(buf))
        return DR_ERR_INVALID;
    size = ((buf[1] & 0x1f) << 8 | buf[2]) + 3 <= size ? ((buf[1] & 0x1f) << 8 | buf[2]) + 3 : size;
    if (dr_bits_init8(&g, buf + 3, size - 3) < 0 || dr_bits_get1(&g))   /* useSameStreamMux */
        return DR_ERR_INVALID;
    /* StreamMuxConfig */
    version = dr_bits_get1(&g);
    if (version && dr_bits_get1(&g))                    /* audioMuxVersionA */
        return DR_ERR_INVALID;
    if (version)
        latm_value(&g);                                 /* taraBufferFullness */
    dr_bits_skip(&g, 1 + 6);                            /* allStreamsSameTimeFraming, numSubFrames */
    if (dr_bits_get(&g, 4) || dr_bits_get(&g, 3))       /* numProgram, numLayer: one each */
        return DR_ERR_INVALID;
    if (version)
        asc_len = latm_value(&g);
    end = asc_len > 0 && g.pos + asc_len < g.size_bits ? g.pos + asc_len : g.size_bits;

    /* AudioSpecificConfig */
    aot  = object_type(&g);
    core = sample_rate(&g);
    if (dr_bits_get(&g, 4) > 7)                         /* channelConfiguration */
        return DR_ERR_INVALID;
    if (aot == AOT_SBR || (aot == AOT_PS && !(peek(&g, 3) & 0x03 && !(peek(&g, 9) & 0x3f)))) {
        ext      = AOT_SBR;
        sbr      = 1;
        ext_rate = sample_rate(&g);
        aot      = object_type(&g);
    }
    switch (aot) {
    case AOT_AAC_MAIN: case AOT_AAC_LC: case AOT_AAC_SSR: case AOT_AAC_LTP: case AOT_ER_AAC_LC:
        frame = dr_bits_get1(&g) ? 960 : 1024;
        break;
    case AOT_ER_AAC_LD: case AOT_ER_AAC_ELD:
        frame = dr_bits_get1(&g) ? 480 : 512;
        break;
    default:
        return DR_ERR_INVALID;                          /* a kind whose frames this does not time */
    }
    /* a backward-compatible SBR signal after the specific config: only when
     * the config's length is stated (the rest of its specific config is not
     * read: looked for bit by bit, as the library does) */
    if (ext != AOT_SBR && asc_len > 0) {
        while (end - g.pos > 15) {
            if (peek(&g, 11) == 0x2b7) {
                dr_bits_skip(&g, 11);
                if (object_type(&g) == AOT_SBR && (sbr = dr_bits_get1(&g)) == 1) {
                    ext_rate = sample_rate(&g);
                    if (ext_rate == core)
                        sbr = -1;
                }
                break;
            }
            dr_bits_skip(&g, 1);
        }
    }
    if (dr_bits_left(&g) < 0 || core <= 0)
        return DR_ERR_INVALID;
    /* SBR at twice the core rate: twice the samples at that rate (the same
     * duration as the core's frame) */
    if (sbr == 1 && ext_rate == 2 * core) {
        *rate    = ext_rate;
        *samples = 2 * frame;
    } else {
        *rate    = core;
        *samples = frame;
    }
    return 0;
}
