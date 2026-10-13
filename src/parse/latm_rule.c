/*
 * AAC LATM / LOAS: the disc reader's frame rules
 *
 * The rule the generic disc engine runs (today FFmpeg's parser.c disc
 * engine; the native splitter later). Moved out of our FFmpeg's
 * latm_parser.c disc mode.
 */

#include <stdint.h>

#include <stddef.h>

#include "parse/rules.h"

/* ---- The disc reader's LOAS frame rules ----
 * A frame starts with the LOAS sync (11 bits, 0x2B7) and is as long as its
 * audioMuxLengthBytes + 3. Where no frame starts, the next one is a sync
 * whose next sync (its length on) is there; with more than 0x20000 bytes
 * and none, the kept bytes are given up. (The reference program also reads
 * each frame's AudioMuxElement; not done here: the title timeline's codec
 * rules take the stream values from FFmpeg's LATM decoder.) */

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
