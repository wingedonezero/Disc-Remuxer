/*
 * DVD / HD DVD sub-pictures: the unit cut rule
 *
 * The rule the splitter engine runs (parse/split.h); it was once a disc
 * mode of ours in FFmpeg's dvdsub_parser.c.
 */

#include <stdint.h>

#include "base/bytes.h"

#include "parse/rules.h"

/* The disc reader's rule: the stream is cut into units by their own
 * headers, whatever the packets it came in: a unit's header is its size and
 * the offset of its first display control sequence, 16-bit each, or (size
 * field 0: an HD DVD unit) 32-bit each after it. With 10 bytes or more, a
 * unit starts only where the header states a size of at least 9 and a first
 * control sequence that leaves 5 bytes; the unit is as long as its size.
 * Where no unit starts, the bytes kept so far are no unit. */
static int disc_header(const uint8_t *b, int avail, uint32_t *size, uint32_t *dcsq)
{
    if (avail < 4)
        return 0;
    if (dr_rb16(b)) {
        *size = dr_rb16(b);
        *dcsq = dr_rb16(b + 2);
        return 1;
    }
    if (avail < 10)
        return 0;
    *size = dr_rb32(b + 2);
    *dcsq = dr_rb32(b + 6);
    return 1;
}

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    uint32_t size, dcsq;

    if (avail >= 10) {
        disc_header(buf, avail, &size, &dcsq);
        if (size < 9 || dcsq > size - 5)
            return DR_CUT_NONE;
    }
    if (!disc_header(buf, avail, &size, &dcsq) || size > avail)
        return 0;
    return size;
}

static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

const DrCutRule dr_dvdsub_cut = { disc_unit_size, disc_resync, 0 };
