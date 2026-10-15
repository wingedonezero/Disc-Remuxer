/*
 * AAC ADTS: the disc reader's frame rules
 *
 * The rule the splitter engine runs (parse/split.h); it was once a disc
 * mode of ours in FFmpeg's aac_parser.c.
 */

#include <stdint.h>

#include <stddef.h>

#include "parse/rules.h"

/* ---- The disc reader's ADTS frame rules ----
 * A frame starts with the ADTS sync (12 bits) and layer 0; the reference
 * program takes only the MPEG-2 ID, here the MPEG-4 ID counts the same (its
 * frames are reviewed by the title timeline). A frame is as long as its
 * frame_length. Where no frame starts, the next one is a header whose next
 * header (frame_length on) has the same ID, layer and protection and the
 * same first four bits of byte 2 (profile, the sampling frequency index's
 * high bits); with more than 0x20000 bytes and none, the kept bytes are
 * given up. */

#define DISC_GIVE_UP 0x20000

static int disc_adts(const uint8_t *b)
{
    return b[0] == 0xff && (b[1] & 0xf6) == 0xf0;
}

static int disc_sync(void *ctx, const uint8_t *b, int len)
{
    if (len < 2)
        return 2;
    return disc_adts(b);
}

static int disc_size(void *ctx, const uint8_t *b, int len)
{
    if (len < 7)
        return -1;
    if (!disc_adts(b))
        return -2;
    return (b[3] & 3) << 11 | b[4] << 3 | b[5] >> 5;
}

static int disc_check(void *ctx, const uint8_t *b, int len)
{
    return len >= 7;
}

static int disc_resync(void *ctx, const uint8_t *b, int len)
{
    if (len < 7)
        return -2;
    for (int pos = 0; pos < len - 7; pos++) {
        int n;
        if (!disc_adts(b + pos))
            continue;
        n = (b[pos + 3] & 3) << 11 | b[pos + 4] << 3 | b[pos + 5] >> 5;
        if (len < pos + n + 7)
            break;                          /* the next header is not there yet */
        if (b[pos + n] == 0xff && b[pos + n + 1] == b[pos + 1] && (b[pos + 2] ^ b[pos + n + 2]) < 0x10)
            return pos;
    }
    return len > DISC_GIVE_UP ? -1 : -2;
}

const DrFrameRule dr_adts_frames = { disc_sync, disc_size, disc_check, disc_resync, DISC_GIVE_UP };
