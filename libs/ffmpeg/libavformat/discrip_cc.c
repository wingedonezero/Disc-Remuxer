/*
 * The shared rip core: DVD-Video line-21 closed captions (CEA-608 byte
 * pairs in MPEG-2 GOP user data).
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

#include "discrip.h"

/* The DVD caption user data: user_data_start_code, 'C' 'C', 0x01, 0xF8,
 * then one byte: bit 7 the first entry is the odd field, bit 6 0, bits 0-5
 * the number of 3-byte entries (a marker byte and a CEA-608 byte pair). */
static const uint8_t header[8] = { 0x00, 0x00, 0x01, 0xB2, 'C', 'C', 0x01, 0xF8 };

int ff_discrip_cc_check(const uint8_t *data, int size)
{
    int need;

    if (size < 9 || memcmp(data, header, 8) || (data[8] & 0x40))
        return 0;
    need = 9 + 3 * (data[8] & 0x3F);
    return size < need ? 0 : need;
}

/* The two entries of a pair (6 bytes) get the field markers of the
 * caption decoder's input (4 = field 1, 5 = field 2) from the two marker
 * bytes and the odd-field-first bit; any other marker pattern ends the
 * block. A last single entry likewise. */
int ff_discrip_cc_triplets(const uint8_t *data, int size, uint8_t *out)
{
    int b = data[8], n = (b & 0x3F) - (b & 1), pairs = n >> 1, o = 0;
    int odd_first = b & 0x80;

    if (ff_discrip_cc_check(data, size) <= 0)
        return 0;
    for (int g = 0; n > 1 && g < pairs; g++) {
        const uint8_t *s = data + 9 + 6 * g;
        int key = (s[0] << 8) | s[3], first;

        if (!odd_first && (key == 0xFEFE || key == 0xFEFF || key == 0xFFFF))
            first = 5;
        else if (!odd_first && key == 0xFFFE)
            first = 4;
        else if (odd_first && (key == 0xFEFE || key == 0xFFFE || key == 0xFFFF))
            first = 4;
        else if (odd_first && key == 0xFEFF)
            first = 5;
        else
            return o / 3;
        memcpy(out + o, s, 6);
        out[o]     = first;
        out[o + 3] = first ^ 1;
        o += 6;
    }
    if (b & 1) {
        const uint8_t *s = data + 9 + 6 * pairs;
        int m;

        if (s[0] == 0xFE || s[0] == 0xFF)
            m = odd_first && s[0] == 0xFF ? 4 : 5;
        else
            return o / 3;
        memcpy(out + o, s, 3);
        out[o] = m;
        o += 3;
    }
    return o / 3;
}
