/*
 * Blu-ray LPCM: the unit cut rule (a 4-byte header and the payload it states; a header that
 * states no known format, or no payload, starts no unit)
 *
 * The rule the splitter engine runs (parse/split.h); it was once a disc
 * mode of ours in FFmpeg's pcm_bluray_parser.c.
 */

#include <stdint.h>

#include "base/bytes.h"

#include "parse/rules.h"

/* Whether hdr states a format: channel assignment 1 (mono) or 3 to 11,
 * sampling frequency 1, 4 or 5 (48, 96, 192 kHz), bits per sample 1 to 3
 * (16, 20, 24). */
static int known_format(const uint8_t *hdr)
{
    int ca = hdr[2] >> 4, fs = hdr[2] & 0x0F, bits = hdr[3] >> 6;

    return (ca == 1 || (ca >= 3 && ca <= 11)) && (fs == 1 || fs == 4 || fs == 5) && bits;
}

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    int size;

    if (avail < 4)
        return 0;
    size = dr_rb16(buf);
    if (!size || !known_format(buf))
        return DR_CUT_NONE;
    return avail >= 4 + size ? 4 + size : 0;
}

/* No search for the next header: the bytes kept are given up. */
static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

const DrCutRule dr_pcm_bluray_cut = { disc_unit_size, disc_resync, 0 };
