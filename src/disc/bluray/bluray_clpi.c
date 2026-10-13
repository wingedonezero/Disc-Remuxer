/*
 * Blu-ray: clip information (BDMV/CLIPINF/NNNNN.clpi, BDAV/CLIPINF/NNNNN.clpi)
 * and the clips' stream files
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
 * Rules kept from the reference program's parser:
 * - type indicator "HDMV" (BDMV clips) or "M2TS" (BDAV clips: no extension
 *   data; a version below 0200 or a BDAV clip counts as Clip_stream_type 1,
 *   application_type 1);
 * - ClipInfo: a TS_type_info block of length 4 or less refuses the clip;
 * - SequenceInfo: no ATC sequence, or one without an STC sequence, refuses
 *   the clip; on a BDMV clip only the first ATC sequence is kept (more are
 *   logged and dropped); the first ATC sequence's first STC start packet and
 *   offset_STC_id are set to 0 when they are not (logged); a further ATC
 *   sequence whose first STC starts after it starts at the ATC start; every
 *   STC start packet number not a multiple of 32 (an aligned unit of 32
 *   source packets) is rounded down to one, with a warning;
 * - ProgramInfo: the streams of the first program only; a stream coding
 *   info of length 0 refuses the clip; at most 32 bytes kept after the
 *   coding type;
 * - CPI: its type (byte 5, low 4 bits) when its length is not 0;
 * - extension data 2/4 (extent start points: length must be 4 x count + 4),
 *   2/5 (a second ProgramInfo, replacing the first), 2/6 (a second CPI head).
 */

#include <stdio.h>
#include <string.h>

#include "base/bytes.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "bluray_internal.h"

#define RD(off, len, buf) do { if (!ff_bluray_read(f, (off), (buf), (len))) return 0; } while (0)

#define TYPE_HDMV DR_MKBETAG('H', 'D', 'M', 'V')
#define TYPE_M2TS DR_MKBETAG('M', '2', 'T', 'S')

static int clip_info(void *log, const BlurayFile *f, BlurayClip *c)
{
    uint8_t b[0xb4], t[2], r[14];
    uint32_t L, pos;

    RD(0x28, 0xb4, b);
    c->stream_type = b[6];
    c->app_type    = b[7];
    if (c->hdr.version < 0x200 || c->hdr.type != TYPE_HDMV)
        c->stream_type = c->app_type = 1;
    c->atc_delta = b[0x0b] != 0;
    c->rate      = dr_rb32(b + 0x0c);
    c->packets   = dr_rb32(b + 0x10);
    L = dr_rb16(b + 0x94);
    if (L <= 4) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: TS_type_info length %u\n", f->path, L);
        return 0;
    }
    c->ts_validity = b[0x96];
    memcpy(c->format_id, b + 0x97, 4);
    if (!c->atc_delta)
        return 1;
    RD(0xbe + L, 2, t);
    if (t[1] && !(c->deltas = dr_calloc(t[1], sizeof(*c->deltas))))
        return DR_ERR(ENOMEM);
    c->nb_deltas = t[1];
    pos = 0xc0 + L;
    for (int i = 0; i < t[1]; i++, pos += 14) {
        RD(pos, 14, r);
        c->deltas[i].delta = dr_rb32(r);
        memcpy(c->deltas[i].clip, r + 4, 5);
        memcpy(c->deltas[i].codec, r + 9, 4);
    }
    return 1;
}

static int sequence_info(void *log, const BlurayFile *f, BlurayClip *c, uint32_t off)
{
    uint8_t h[6], a[6], b[14];
    uint32_t pos;
    int n;

    RD(off, 6, h);
    if (!(n = h[5])) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: no ATC sequence\n", f->path);
        return 0;
    }
    if (!(c->atc = dr_calloc(n, sizeof(*c->atc))))
        return DR_ERR(ENOMEM);
    pos = off + 6;
    for (int k = 0; k < n; k++) {
        BlurayAtc *A = &c->atc[k];
        int m;

        RD(pos, 6, a);
        A->spn_atc_start = dr_rb32(a);
        A->offset_stc_id = a[5];
        if (!(m = a[4])) {
            dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: ATC sequence %d without STC sequence\n", f->path, k);
            return 0;
        }
        if (!(A->stc = dr_calloc(m, sizeof(*A->stc))))
            return DR_ERR(ENOMEM);
        c->nb_atc = k + 1;
        for (int j = 0; j < m; j++) {
            BlurayStc *T = &A->stc[j];
            RD(pos + 6 + 14 * j, 14, b);
            T->pcr_pid       = dr_rb16(b);
            T->spn_stc_start = dr_rb32(b + 2);
            T->start         = dr_rb32(b + 6);
            T->end           = dr_rb32(b + 10);
            A->nb_stc        = j + 1;
        }
        pos += 6 + 14 * m;
    }
    return 1;
}

/* The corrections the reference program makes to SequenceInfo. */
static void fix_sequences(void *log, const BlurayFile *f, BlurayClip *c)
{
    BlurayAtc *A0 = &c->atc[0];

    if (A0->stc[0].spn_stc_start) {
        dr_vlog(log, DR_VLOG_INFO, "Blu-ray: %s: its first STC sequence starts at source packet %u: taken as 0 (the "
               "clip's start)\n", f->path, A0->stc[0].spn_stc_start);
        A0->stc[0].spn_stc_start = 0;
    }
    if (A0->offset_stc_id) {
        dr_vlog(log, DR_VLOG_INFO, "Blu-ray: %s: its first ATC sequence's offset_STC_id %u: taken as 0\n", f->path,
               A0->offset_stc_id);
        A0->offset_stc_id = 0;
    }
    for (int k = 1; k < c->nb_atc; k++) {
        BlurayAtc *A = &c->atc[k];
        if (A->spn_atc_start < A->stc[0].spn_stc_start) {
            dr_vlog(log, DR_VLOG_INFO, "Blu-ray: %s: ATC sequence %d starts at source packet %u, its first STC "
                   "sequence at %u: moved to its ATC sequence's start\n", f->path, k, A->spn_atc_start,
                   A->stc[0].spn_stc_start);
            A->stc[0].spn_stc_start = A->spn_atc_start;
        } else if (A->spn_atc_start > A->stc[0].spn_stc_start) {
            dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: %s: ATC sequence %d starts at source packet %u, after its first STC "
                   "sequence (%u): kept as it is\n", f->path, k, A->spn_atc_start, A->stc[0].spn_stc_start);
        }
    }
    for (int k = 0; k < c->nb_atc; k++)
        for (int j = 0; j < c->atc[k].nb_stc; j++) {
            BlurayStc *T = &c->atc[k].stc[j];
            if (T->spn_stc_start & 0x1f) {
                dr_vlog(log, DR_VLOG_WARNING, "Blu-ray: clip %05u.m2ts: STC sequence start 0x%08x is not on an "
                       "aligned unit: moved back to 0x%08x\n", c->number, T->spn_stc_start,
                       T->spn_stc_start & ~0x1fu);
                T->spn_stc_start &= ~0x1fu;
            }
        }
}

/* ProgramInfo at *off: the first program's streams (replacing any read
 * before); *off moves past them. */
static int program_info(const BlurayFile *f, BlurayClip *c, uint32_t *off)
{
    uint8_t h[14], a[4], b[2];
    uint32_t o = *off;
    BlurayProgram *p = &c->program;
    int n;

    RD(o, 14, h);
    dr_freep(&p->streams);
    p->nb_streams = 0;
    p->spn_start  = dr_rb32(h + 6);
    p->pmt_pid    = dr_rb16(h + 10);
    n = h[12];
    if (n && !(p->streams = dr_calloc(n, sizeof(*p->streams))))
        return DR_ERR(ENOMEM);
    o += 14;
    for (int i = 0; i < n; i++) {
        BlurayClipStream *s = &p->streams[i];
        RD(o, 4, a);
        s->pid = dr_rb16(a);
        RD(o + 2, 2, b);
        s->coding_type = b[1];
        if (!b[0])
            return 0;
        s->info_len = b[0] - 1;
        RD(o + 4, DR_MIN(b[0] - 1, 32), s->info);
        p->nb_streams = i + 1;
        o += 4 + (a[2] - 1);
    }
    *off = o;
    return 1;
}

static int cpi_head(const BlurayFile *f, BlurayClip *c, uint32_t off)
{
    uint8_t h[6];

    RD(off, 6, h);
    if (dr_rb32(h))
        c->cpi_type = h[5] & 0xf;
    return 1;
}

/* The extension entries this parser reads itself. */
static int clpi_parsed_directly(uint16_t id1, uint16_t id2)
{
    return id1 == 2 && (uint16_t)(id2 - 4) < 3;
}

int ff_bluray_clpi_parse(void *log, const BlurayFile *f, BlurayClip *c)
{
    uint8_t h[0x28];
    uint32_t seq, prog, cpi, ext;
    int ret;

    RD(0, 8, h);
    ff_bluray_header(h, &c->hdr);
    if (c->hdr.type != TYPE_HDMV && c->hdr.type != TYPE_M2TS) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: not clip information\n", f->path);
        return 0;
    }
    RD(0, 0x28, h);
    seq  = dr_rb32(h + 0x08);
    prog = dr_rb32(h + 0x0c);
    cpi  = dr_rb32(h + 0x10);
    ext  = dr_rb32(h + 0x18);
    if ((ret = clip_info(log, f, c)) <= 0 || (ret = sequence_info(log, f, c, seq)) <= 0)
        return ret;
    if (c->hdr.type == TYPE_HDMV && c->nb_atc > 1) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: %d further ATC sequences dropped\n", f->path, c->nb_atc - 1);
        for (int k = 1; k < c->nb_atc; k++)
            dr_freep(&c->atc[k].stc);
        c->nb_atc = 1;
    }
    fix_sequences(log, f, c);
    if ((ret = program_info(f, c, &prog)) <= 0 || (ret = cpi_head(f, c, cpi)) <= 0)
        return ret;
    if (c->hdr.type != TYPE_HDMV || !ext)
        return 1;
    if ((ret = ff_bluray_ext_data(log, f, ext, clpi_parsed_directly, &c->ext, &c->nb_ext)) <= 0)
        return ret;
    for (int i = 0; i < c->nb_ext; i++) {
        const BlurayExtEntry *e = &c->ext[i];
        uint8_t b[8];
        uint32_t p;

        if (e->id1 != 2)
            continue;
        if (e->id2 == 4) {
            uint32_t n;
            RD(e->start, 8, b);
            n = dr_rb32(b + 4);
            if (dr_rb32(b) != n * 4 + 4) {
                dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s: extent start points: length %u for %u entries\n",
                       f->path, dr_rb32(b), n);
                return 0;
            }
            dr_freep(&c->ss_extents);
            c->nb_ss_extents = 0;
            if (n && !(c->ss_extents = dr_malloc_array(n, sizeof(*c->ss_extents))))
                return DR_ERR(ENOMEM);
            for (uint32_t k = 0; k < n; k++) {
                RD(e->start + 8 + 4 * k, 4, b);
                c->ss_extents[k] = dr_rb32(b);
            }
            c->nb_ss_extents = n;
        } else if (e->id2 == 5) {
            p = e->start;
            if ((ret = program_info(f, c, &p)) <= 0)
                return ret;
        } else if (e->id2 == 6) {
            if (!cpi_head(f, c, e->start))
                return 0;
        }
    }
    return 1;
}

int ff_bluray_clip_stream_file(void *log, DiscIOFS *fs, BlurayClip *c)
{
    const char *root = c->hdr.type == TYPE_M2TS ? "BDAV" : "BDMV";
    const char *ext  = c->stream_type == 2 ? "fmts" : "m2ts";
    DiscIOFile *file = NULL;
    char path[48];
    int64_t expected = (int64_t)c->packets * 192;
    int ret;

    snprintf(path, sizeof(path), "/%s/STREAM/%05u.%s", root, c->number, ext);
    ret = fs->ops->open_file(fs, path, &file);
    if (ret < 0) {
        snprintf(path, sizeof(path), "/%s/STREAM/%05u.MTS", root, c->number);
        ret = fs->ops->open_file(fs, path, &file);
    }
    if (ret < 0) {
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: clip %05u has no stream file\n", c->number);
        c->stream_path[0] = 0;
        return 0;
    }
    snprintf(c->stream_path, sizeof(c->stream_path), "%s", path);
    c->stream_size = file->size;
    if (file->size != expected)
        dr_vlog(log, DR_VLOG_DEBUG, "Blu-ray: %s is %"PRId64" sectors, its clip information says %"PRId64"\n",
               path, file->size >> 11, expected >> 11);
    ff_discio_file_free(&file);
    return 1;
}

void ff_bluray_clip_free(BlurayClip *c)
{
    for (int k = 0; k < c->nb_atc; k++)
        dr_freep(&c->atc[k].stc);
    dr_freep(&c->atc);
    dr_freep(&c->deltas);
    dr_freep(&c->program.streams);
    dr_freep(&c->ss_extents);
    ff_bluray_ext_free(&c->ext, &c->nb_ext);
    dr_freep(&c->pmt);
    c->pmt_len = c->pmt_tried = 0;
    c->nb_atc = c->nb_deltas = c->program.nb_streams = c->nb_ss_extents = 0;
}
