/*
 * Blu-ray: the index table (BDMV/index.bdmv) and the disc id
 * (CERTIFICATE/id.bdmv)
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

/*
 * index.bdmv as the reference program reads it: type indicator "INDX", the
 * Indexes block (its start must not be 0) with the First Playback and Top
 * Menu entries and the title entries (12 bytes each; the count is not
 * checked against the block length, an entry needs only 8 bytes left in
 * the file), then the extension data. An entry's object type is byte 0 bits
 * 7-6: 1 = an HDMV movie object (its id: bytes 6-7), 2 = a BD-J object (its
 * number: the 5 digits at bytes 6-10). id.bdmv: type indicator "BDID", the
 * organisation id and the 16-byte disc id at 0x28.
 */

#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "bluray_internal.h"

#define RD(off, len, buf) do { if (!ff_bluray_read(f, (off), (buf), (len))) return 0; } while (0)

static void entry(const uint8_t e[12], BlurayIndexEntry *out)
{
    out->object_type   = e[0] >> 6;
    out->access_type   = (e[0] >> 4) & 3;
    out->playback_type = e[4] >> 6;
    if (out->object_type == 2) {
        char d[6];
        memcpy(d, e + 6, 5);
        d[5] = 0;
        out->ref = strtoul(d, NULL, 10);
    } else if (out->object_type == 1) {
        out->ref = dr_rb16(e + 6);
    }
}

/* An entry needs 8 bytes in the file and is read as 12. */
static int entry_read(const BlurayFile *f, uint32_t off, BlurayIndexEntry *out)
{
    uint8_t e[12];

    if ((uint64_t)off + 8 > f->size)
        return 0;
    RD(off, 12, e);
    entry(e, out);
    return 1;
}

int ff_bluray_index_parse(void *log, const BlurayFile *f, BlurayIndex *ix)
{
    uint8_t h[0x20], l[4], n[2];
    uint32_t X, E, end, pos;
    int N, ret;

    RD(0, 0x20, h);
    ff_bluray_header(h, &ix->hdr);
    if (ix->hdr.type != DR_MKBETAG('I', 'N', 'D', 'X'))
        return 0;
    X = dr_rb32(h + 8);
    E = dr_rb32(h + 12);
    if (!X || f->size < (uint64_t)X + 8)
        return 0;
    RD(X, 4, l);
    end = dr_rb32(l) + X + 4;
    if (end > f->size)
        end = f->size;
    if (f->size < (uint64_t)X + 0xc || !entry_read(f, X + 4, &ix->first_play) ||
        !entry_read(f, X + 0x10, &ix->top_menu))
        return 0;
    if (X + 0x1e > end)
        return 0;
    RD(X + 0x1c, 2, n);
    N = dr_rb16(n);
    if (N && !(ix->titles = dr_calloc(N, sizeof(*ix->titles))))
        return DR_ERR(ENOMEM);
    pos = X + 0x1e;
    for (int i = 0; i < N; i++, pos += 12) {
        if (!entry_read(f, pos, &ix->titles[i]))
            return 0;
        ix->nb_titles = i + 1;
    }
    if (E && (ret = ff_bluray_ext_data(log, f, E, NULL, &ix->ext, &ix->nb_ext)) <= 0) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: extension data at %u cannot be read\n", f->path, E);
        return ret;
    }
    return 1;
}

void ff_bluray_index_free(BlurayIndex *ix)
{
    dr_freep(&ix->titles);
    ix->nb_titles = 0;
    ff_bluray_ext_free(&ix->ext, &ix->nb_ext);
}

int ff_bluray_id_parse(void *log, const BlurayFile *f, BlurayId *id)
{
    uint8_t h[8], b[0x14];

    RD(0, 8, h);
    ff_bluray_header(h, &id->hdr);
    if (id->hdr.type != DR_MKBETAG('B', 'D', 'I', 'D'))
        return 0;
    RD(0x28, 0x14, b);
    id->org_id = dr_rb32(b);
    memcpy(id->disc_id, b + 4, 16);
    return 1;
}
