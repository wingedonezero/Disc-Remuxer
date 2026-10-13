/*
 * TrueHD / MLP: the access-unit cut rule
 *
 * The rule the generic disc engine runs (today FFmpeg's parser.c disc
 * engine; the native splitter later). Moved out of our FFmpeg's
 * mlp_parser.c disc mode.
 */

#include <stdint.h>


#include "parse/rules.h"

/* Disc mode (PARSER_FLAG_DISC): an access unit is as long as its length
 * field says (12 bits, in 16-bit words, after the check nibble), at least 8
 * bytes; where none starts, the next one is looked for at a major sync (its
 * sync word F8 72 6F BA / BB 4 bytes in). Without disc mode an AU whose
 * parity or checksum fails drops every AU up to the next major sync. */
static int disc_major_sync(const uint8_t *p, int n)
{
    return n >= 8 && p[4] == 0xF8 && p[5] == 0x72 && p[6] == 0x6F && (p[7] & 0xFE) == 0xBA;
}

static int disc_unit_size(void *logctx, const uint8_t *p, int avail, int final, void *state)
{
    int len;

    if (avail < 8)
        return 0;
    len = (((p[0] & 0xF) << 8) | p[1]) * 2;
    if (len < 8)
        return DR_CUT_NONE;
    return len <= avail ? len : 0;
}

static int disc_resync(const uint8_t *p, int avail)
{
    for (int i = 0; i + 8 <= avail; i++)
        if (disc_major_sync(p + i, avail - i))
            return i;
    return -1;
}

const DrCutRule dr_mlp_cut = { disc_unit_size, disc_resync, 0 };
