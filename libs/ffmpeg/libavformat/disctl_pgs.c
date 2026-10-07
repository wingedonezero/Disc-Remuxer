/*
 * The title timeline: Blu-ray presentation graphics (PGS) display sets.
 *
 * A display set is a run of segments (type byte, 16-bit length, body) from a
 * presentation composition segment (0x16) to the end segment (0x80). The
 * composition segment's body: video width and height (2 + 2 bytes), frame
 * rate (1), composition number (2), composition state (1), palette update
 * flag (1), palette id (1), number of composition objects (1), then each
 * composition object: object id (2), window id (1), flags (1: 0x80 cropped,
 * 0x40 forced on), x and y (2 + 2), and when cropped 8 more bytes (the
 * cropping rectangle). The composition objects are what the set shows.
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

#include <string.h>

#include "libavutil/intreadwrite.h"

#include "disctl.h"

#define SEG_PCS 0x16
#define SEG_END 0x80
#define PCS_OBJECTS 10          /* body offset of number_of_composition_objects */

/* The size of the composition object at p (avail bytes), 0 when it does not fit. */
static int object_size(const uint8_t *p, int avail)
{
    int n;

    if (avail < 8)
        return 0;
    n = (p[3] & 0x80) ? 16 : 8;
    return n <= avail ? n : 0;
}

int ff_disctl_pgs_check(const uint8_t *d, int size)
{
    int off = 0, last = -1;

    if (size < 3 || d[0] != SEG_PCS)
        return 0;
    while (off + 3 <= size) {
        last = d[off];
        off += 3 + AV_RB16(d + off + 1);
    }
    return off == size && last == SEG_END;
}

int ff_disctl_pgs_verify(const uint8_t *d, int size)
{
    return ff_disctl_pgs_check(d, size) ? DR_UNIT_OK : DR_UNIT_BAD;
}

int ff_disctl_pgs_forced_only(const uint8_t *in, int size, uint8_t *out, int *forced)
{
    int off = 0, o = 0;

    *forced = 0;
    while (off + 3 <= size) {
        int len = AV_RB16(in + off + 1);
        const uint8_t *b = in + off + 3;

        if (off + 3 + len > size)
            return AVERROR_INVALIDDATA;
        if (in[off] != SEG_PCS || len < PCS_OBJECTS + 1) {
            memcpy(out + o, in + off, 3 + len);
            o += 3 + len;
        } else {
            int n = b[PCS_OBJECTS], p = PCS_OBJECTS + 1, kept = 0, q;

            /* the body up to the object count, then the forced objects */
            out[o] = SEG_PCS;
            memcpy(out + o + 3, b, PCS_OBJECTS);
            q = o + 3 + PCS_OBJECTS + 1;
            for (int i = 0; i < n; i++) {
                int sz = object_size(b + p, len - p);
                if (!sz)
                    return AVERROR_INVALIDDATA;
                if (b[p + 3] & 0x40) {
                    memcpy(out + q, b + p, sz);
                    q += sz;
                    kept++;
                }
                p += sz;
            }
            /* what follows the objects (none in the format) stays */
            memcpy(out + q, b + p, len - p);
            q += len - p;
            out[o + 3 + PCS_OBJECTS] = kept;
            AV_WB16(out + o + 1, q - o - 3);
            *forced += kept;
            o = q;
        }
        off += 3 + len;
    }
    return off == size ? o : AVERROR_INVALIDDATA;
}
