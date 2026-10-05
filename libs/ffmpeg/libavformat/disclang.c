/*
 * Disc demuxers: language codes.
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

#include "avlanguage.h"
#include "disclang.h"

const char *ff_disc_lang_code(const char *code)
{
    static const char *const withdrawn[] = { "in", "ji", "jw", "mo", "sh" };
    char low[4];
    size_t n = strlen(code);

    if (n != 2 && n != 3)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)code[i] >= 0x80)
            return NULL;
        low[i] = code[i] >= 'A' && code[i] <= 'Z' ? code[i] - 'A' + 'a' : code[i];
    }
    low[n] = 0;
    if (!strcmp(low, "ptb"))
        return "ptb";
    if (!strcmp(low, "jp"))
        return "jpn";
    if (!strcmp(low, "iw"))
        return "heb";
    for (size_t i = 0; i < sizeof(withdrawn) / sizeof(*withdrawn); i++)
        if (!strcmp(low, withdrawn[i]))
            return NULL;
    return ff_convert_lang_to(low, AV_LANG_ISO639_2_BIBL);
}
