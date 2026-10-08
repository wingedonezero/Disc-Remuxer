/*
 * DVD-Video: the disc's structure. VIDEO_TS.IFO (the VMG) is loaded first
 * (dvd_ifo.c); a VMG that cannot be read is no DVD. Then:
 *
 * - the title table (TT_SRPT, at most 101 entries read): each entry with a
 *   title set, a title number in it and chapters must name a title set the
 *   disc has (1..99 and at most vmg_nr_of_title_sets, else the disc is
 *   refused); entries that give one title set different start sectors are
 *   reported;
 * - the text data (TXTDT_MG), when the VMG has one: a tree of item nodes
 *   (kinds below 0x30) with their texts (kinds 0x30 and up), which gives
 *   the disc's name and later its titles' and chapters' names. Texts are
 *   kept for the character sets 0x01 and 0x11 only, and of each text only
 *   the bytes 0x20-0x7A (others are dropped, and logged); any other
 *   character set, or a structure that does not hold together, leaves the
 *   disc without text data;
 * - more than 99 title sets: the disc is refused;
 * - the names: the volume label (an image), else (a folder) the VMG's
 *   provider identifier; the text data's name (item 0x30, else 0x39, 0x38,
 *   0x3A, 0x3B of its first node);
 * - every title set in order. A title set that cannot be loaded stays a
 *   placeholder (the disc goes on without it). Where each title set starts
 *   (in an image its IFO's position, in a folder the sum of the sets before
 *   it by their headers) is checked against the title table.
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

#include <inttypes.h>
#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

#define TT_SRPT_MAX     101     /* title table entries read */
#define TEXT_MAX        0x400   /* bytes of one text read (it ends at a TAB) */

/* ---- the title table ---- */

static uint32_t len_tt_srpt(const uint8_t *first, uint32_t field)
{
    unsigned n = AV_RB16(first);
    return FFMIN(n, TT_SRPT_MAX) * 12 + 8;
}

/* A title table field, 0 when it lies outside the table. */
static unsigned tt8(const DVDDisc *d, uint32_t o)  { return o + 1 <= d->tt_srpt.size ? d->tt_srpt.data[o] : 0; }
static unsigned tt16(const DVDDisc *d, uint32_t o) { return o + 2 <= d->tt_srpt.size ? AV_RB16(d->tt_srpt.data + o) : 0; }
static uint32_t tt32(const DVDDisc *d, uint32_t o) { return o + 4 <= d->tt_srpt.size ? AV_RB32(d->tt_srpt.data + o) : 0; }

/* An entry without a title set, a title number in it or chapters. */
static int tt_broken(const DVDDisc *d, int i)
{
    return !tt8(d, 8 + 12 * i + 6) || !tt8(d, 8 + 12 * i + 7) || !tt16(d, 8 + 12 * i + 2);
}

static int check_title_table(DVDDisc *d)
{
    int n = tt16(d, 0);

    for (int i = 0; i < n; i++) {
        unsigned tsn = tt8(d, 8 + 12 * i + 6);
        uint32_t sec = tt32(d, 8 + 12 * i + 8);
        if (tt_broken(d, i))
            continue;
        if (tsn > DVD_MAX_TITLE_SETS || tsn > d->nr_of_title_sets) {
            av_log(d->log, AV_LOG_ERROR, "DVD: title %d of the title table is in title set %u, which the disc "
                   "does not have (it has %d)\n", i + 1, tsn, d->nr_of_title_sets);
            return AVERROR_INVALIDDATA;
        }
        if (!d->title_set_start[tsn - 1])
            d->title_set_start[tsn - 1] = sec;
        else if (d->title_set_start[tsn - 1] != sec)
            av_log(d->log, AV_LOG_WARNING, "DVD: the title table starts title set %u at sector %"PRIu32" (title %d) "
                   "and at %"PRIu32"\n", tsn, sec, i + 1, d->title_set_start[tsn - 1]);
    }
    return 0;
}

/* ---- text data ---- */

static void text_node_free(DVDTextNode *n)
{
    for (int i = 0; i < n->nb_texts; i++)
        av_free(n->texts[i]);
    av_free(n->texts);
    av_free(n->text_kinds);
    av_free(n->children);
    av_free(n);
}

static void text_free(DVDText **pt)
{
    DVDText *t = *pt;
    if (!t)
        return;
    for (int i = 0; i < t->nb_nodes; i++)
        text_node_free(t->nodes[i]);
    av_free(t->nodes);
    av_freep(pt);
}

/* A text of kind under node n (the first of a kind is kept): 1, 0 when the
 * character set is not read (the text data is dropped), < 0 an error. */
static int text_add(DVDDisc *d, DVDTextNode *n, uint8_t kind, uint8_t charset, const uint8_t *raw)
{
    const uint8_t *tab = memchr(raw, 0x09, TEXT_MAX + 1);
    int len = tab - raw, k = 0, dropped = 0, pos;
    char *s;

    if (len && (charset | 0x10) != 0x11) {
        av_log(d->log, AV_LOG_WARNING, "DVD: text data in character set 0x%02x is not read\n", charset);
        return 0;
    }
    if (!(s = av_malloc(len + 1)))
        return AVERROR(ENOMEM);
    for (int i = 0; i < len; i++) {
        if (raw[i] >= 0x20 && raw[i] <= 0x7a)
            s[k++] = raw[i];
        else
            dropped++;
    }
    s[k] = 0;
    if (dropped)
        av_log(d->log, AV_LOG_WARNING, "DVD: text data item 0x%02x: %d byte(s) outside 0x20-0x7A dropped (kept: "
               "\"%s\")\n", kind, dropped, s);
    for (pos = 0; pos < n->nb_texts && n->text_kinds[pos] < kind; pos++)
        ;
    if (pos < n->nb_texts && n->text_kinds[pos] == kind) {
        av_free(s);                     /* the first text of a kind is kept */
        return 1;
    }
    if (av_reallocp_array(&n->texts, n->nb_texts + 1, sizeof(*n->texts)) < 0 ||
        av_reallocp_array(&n->text_kinds, n->nb_texts + 1, 1) < 0) {
        av_free(s);
        n->nb_texts = 0;
        return AVERROR(ENOMEM);
    }
    memmove(n->texts + pos + 1, n->texts + pos, (n->nb_texts - pos) * sizeof(*n->texts));
    memmove(n->text_kinds + pos + 1, n->text_kinds + pos, n->nb_texts - pos);
    n->texts[pos]      = s;
    n->text_kinds[pos] = kind;
    n->nb_texts++;
    return 1;
}

static const char *text_of(const DVDTextNode *n, uint8_t kind)
{
    for (int i = 0; n && i < n->nb_texts; i++)
        if (n->text_kinds[i] == kind)
            return n->texts[i];
    return NULL;
}

/* A field of the text data table, 0 when outside it. */
static unsigned tx8(const DVDTable *b, uint64_t o)  { return o + 1 <= b->size ? b->data[o] : 0; }
static unsigned tx16(const DVDTable *b, uint64_t o) { return o + 2 <= b->size ? AV_RB16(b->data + o) : 0; }

/* The text data tree of table b; NULL (and 0) when it does not hold
 * together. */
static int parse_text(DVDDisc *d, const DVDTable *b, DVDText **out)
{
    DVDTextNode root = { 0 }, *cur = &root;
    DVDText *t;
    uint32_t L = b->size, X, cnt;
    uint8_t charset, raw[TEXT_MAX + 1];
    const char *why = NULL;
    int ret = 0;

    *out = NULL;
    if (L < 0x10 || !AV_RN16(b->data + 0x0e))
        return 0;
    charset = tx8(b, 0x17);
    X = L >= 0x1c ? AV_RB32(b->data + 0x18) : 0;
    if (X >= L) {
        av_log(d->log, AV_LOG_WARNING, "DVD: text data: its language unit (byte %"PRIu32") lies outside it\n", X);
        return 0;
    }
    if ((uint64_t)tx16(b, (uint64_t)X + 2) + 1 + X > L) {
        av_log(d->log, AV_LOG_WARNING, "DVD: text data: its language unit runs past its end\n");
        return 0;
    }
    if (!(t = av_mallocz(sizeof(*t))))
        return AVERROR(ENOMEM);
    t->language[0] = tx8(b, 0x14);
    t->language[1] = tx8(b, 0x15);
    t->language[2] = tx8(b, 0x16);
    for (int k = 0; k < 99; k++)
        t->title_offsets[k] = tx16(b, (uint64_t)X + 6 + 2 * k);
    if ((uint64_t)X + 0xce > L || !(cnt = AV_RB16(b->data + X + 0xcc)))
        goto drop;
    if ((uint64_t)X + 0xd1 > L || b->data[X + 0xd0] != 1) {
        why = "its first entry is not the disc's";
        goto drop;
    }
    for (uint32_t i = 0; i != cnt; i++) {
        uint64_t e = (uint64_t)X + 0xd0 + 4 * i;
        uint8_t kind = e + 1 <= L ? b->data[e] : 0;

        if (!kind) {                    /* the end, when its offset is 0 */
            if (tx16(b, e + 2))
                av_log(d->log, AV_LOG_VERBOSE, "DVD: text data: entry %"PRIu32" of kind 0 is not an end\n", i);
            break;
        }
        if (kind < 0x30) {
            DVDTextNode *n = av_mallocz(sizeof(*n));
            if (!n || av_dynarray_add_nofree(&t->nodes, &t->nb_nodes, n) < 0) {
                av_free(n);
                ret = AVERROR(ENOMEM);
                goto drop;
            }
            n->offset = 0xd0 + 4 * i;
            n->kind   = kind;
            while (kind <= cur->kind)
                cur = cur->parent;
            n->parent = cur;
            if ((ret = av_dynarray_add_nofree(&cur->children, &cur->nb_children, n)) < 0)
                goto drop;
            cur = n;
        } else {
            uint64_t s = (uint64_t)tx16(b, e + 2) + X + 0xcc;
            uint64_t len;
            if (s >= L) {
                why = "a text lies outside it";
                goto drop;
            }
            len = FFMIN(L - s, TEXT_MAX + 1);
            memcpy(raw, b->data + s, len);
            raw[TEXT_MAX] = 0x09;
            if (len < TEXT_MAX)
                memset(raw + len, 0x09, TEXT_MAX - len);
            if ((ret = text_add(d, cur, kind, charset, raw)) <= 0)
                goto drop;
        }
    }
    if (root.nb_children != 1) {
        why = "it has not exactly one disc entry";
        goto drop;
    }
    t->root = root.children[0];
    t->root->parent = NULL;
    av_free(root.children);
    *out = t;
    return 0;
drop:
    if (why)
        av_log(d->log, AV_LOG_WARNING, "DVD: text data: %s: it is not used\n", why);
    av_free(root.children);
    text_free(&t);
    return ret < 0 ? ret : 0;
}

/* ---- the disc ---- */

int ff_dvd_disc_open(void *log, DiscIOFS *fs, const char *dir, int folder, const char *label, DVDDisc *d)
{
    static const uint8_t name_items[] = { 0x30, 0x39, 0x38, 0x3a, 0x3b };
    DVDIfo *vmg = NULL;
    uint32_t next, txt;
    int ret;

    memset(d, 0, sizeof(*d));
    d->log    = log;
    d->fs     = fs;
    d->dir    = dir;
    d->folder = folder;

    /* the VMG */
    ret = ff_dvd_ifo_load(d, 0, &vmg);
    if (!vmg)
        return AVERROR(ENOMEM);
    if (av_dynarray_add_nofree(&d->ifos, &d->nb_ifos, vmg) < 0) {
        ff_dvd_ifo_free(&vmg);
        return AVERROR(ENOMEM);
    }
    if (ret < 0) {
        av_log(log, AV_LOG_ERROR, "DVD: VIDEO_TS.IFO cannot be read: no DVD-Video\n");
        return ret;
    }
    d->nr_of_title_sets = AV_RB16(vmg->header + DVD_NR_OF_TITLE_SETS);
    if (!(d->title_set_start = av_calloc(d->nr_of_title_sets + 1, sizeof(*d->title_set_start))))
        return AVERROR(ENOMEM);

    /* the title table */
    if ((ret = ff_dvd_ifo_table(d, vmg, ff_dvd_header32(vmg, DVD_TT_SRPT), &d->tt_srpt, len_tt_srpt, 0)) < 0) {
        av_log(log, AV_LOG_ERROR, "DVD: the title table (TT_SRPT) cannot be read\n");
        return ret;
    }
    if (d->tt_srpt.size >= 8 && AV_RB16(d->tt_srpt.data) > TT_SRPT_MAX)
        av_log(log, AV_LOG_WARNING, "DVD: the title table has %u entries: the first %d are read\n",
               AV_RB16(d->tt_srpt.data), TT_SRPT_MAX);
    if ((ret = check_title_table(d)) < 0)
        return ret;

    /* the text data */
    if ((txt = ff_dvd_header32(vmg, DVD_TXTDT_MGI))) {
        DVDTable b = { 0 };
        if (ff_dvd_ifo_table(d, vmg, txt, &b, ff_dvd_table_len_last_byte, 0x10) < 0)
            av_log(log, AV_LOG_WARNING, "DVD: the text data at sector %"PRIu32" cannot be read\n", txt);
        else
            ret = parse_text(d, &b, &d->text);
        ff_dvd_table_free(&b);
        if (ret < 0)
            return ret;
    }

    if (d->nr_of_title_sets > DVD_MAX_TITLE_SETS) {
        av_log(log, AV_LOG_ERROR, "DVD: the disc has %d title sets, more than %d\n", d->nr_of_title_sets,
               DVD_MAX_TITLE_SETS);
        return AVERROR_INVALIDDATA;
    }

    /* the names */
    if (label)
        d->volume_name = av_strdup(label);
    if (d->text)
        for (int i = 0; i < FF_ARRAY_ELEMS(name_items) && !d->text_name; i++) {
            const char *s = text_of(d->text->root, name_items[i]);
            if (s && !(d->text_name = av_strdup(s)))
                return AVERROR(ENOMEM);
        }
    else {
        DiscIOFile *f = NULL;
        if (fs->ops->open_file(fs, "/DVDROM/iTunesInfo.xml", &f) >= 0)
            av_log(log, AV_LOG_WARNING, "DVD: /DVDROM/iTunesInfo.xml is present: the name in it is not read "
                   "[untested on real discs]\n");
        ff_discio_file_free(&f);
    }
    if (!d->volume_name)
        d->volume_name = av_strndup((const char *)vmg->header + DVD_PROVIDER_ID, 32);
    if (!d->volume_name)
        return AVERROR(ENOMEM);

    /* every title set */
    next = ff_dvd_header32(vmg, DVD_LAST_SECTOR) + 1;
    for (int i = 1; i <= d->nr_of_title_sets; i++) {
        DVDIfo *f = NULL;
        ret = ff_dvd_ifo_load(d, i, &f);
        if (!f || av_dynarray_add_nofree(&d->ifos, &d->nb_ifos, f) < 0) {
            ff_dvd_ifo_free(&f);
            return AVERROR(ENOMEM);
        }
        if (ret == AVERROR(ENOMEM))
            return ret;
        if (ret >= 0) {
            if (!folder && f->ifo_sector && vmg->ifo_sector)
                next = f->ifo_sector - vmg->ifo_sector;
            if (d->title_set_start[i - 1] && next != d->title_set_start[i - 1])
                av_log(log, AV_LOG_WARNING, "DVD: title set %d starts at sector %"PRIu32" by %s, at %"PRIu32" by the "
                       "title table\n", i, next, folder ? "the headers of the sets before it" : "its IFO's position",
                       d->title_set_start[i - 1]);
            next += ff_dvd_header32(f, DVD_LAST_SECTOR) + 1;
        } else {
            av_log(log, AV_LOG_WARNING, "DVD: title set %d cannot be read: the disc is read without it\n", i);
            if (d->title_set_start[i])
                next = d->title_set_start[i];
        }
    }
    return 0;
}

void ff_dvd_disc_free(DVDDisc *d)
{
    for (int i = 0; i < d->nb_ifos; i++)
        ff_dvd_ifo_free(&d->ifos[i]);
    av_freep(&d->ifos);
    d->nb_ifos = 0;
    ff_dvd_table_free(&d->tt_srpt);
    av_freep(&d->title_set_start);
    text_free(&d->text);
    av_freep(&d->volume_name);
    av_freep(&d->text_name);
}

static void dump_text(const DVDTextNode *n, int depth, AVBPrint *bp)
{
    av_bprintf(bp, "%*s  node kind 0x%02x at %u\n", 2 * depth, "", n->kind, n->offset);
    for (int i = 0; i < n->nb_texts; i++)
        av_bprintf(bp, "%*s    text 0x%02x \"%s\"\n", 2 * depth, "", n->text_kinds[i], n->texts[i]);
    for (int i = 0; i < n->nb_children; i++)
        dump_text(n->children[i], depth + 1, bp);
}

void ff_dvd_disc_dump(const DVDDisc *d, AVBPrint *bp)
{
    av_bprintf(bp, "volume name \"%s\", text name \"%s\", %d title set(s), %u title table entries\n",
               d->volume_name ? d->volume_name : "", d->text_name ? d->text_name : "", d->nr_of_title_sets,
               d->tt_srpt.size >= 2 ? AV_RB16(d->tt_srpt.data) : 0);
    for (int i = 0; i < d->nb_ifos; i++) {
        const DVDIfo *f = d->ifos[i];
        av_bprintf(bp, "%s%02d: %s", f->index ? "VTS_" : "VIDEO_TS ", f->index, f->failed ? "not read\n" : "");
        if (f->failed)
            continue;
        av_bprintf(bp, "last sector %"PRIu32", IFO last sector %"PRIu32", %s%s, title VOBs %"PRIu32" sectors",
                   ff_dvd_header32(f, DVD_LAST_SECTOR), ff_dvd_header32(f, DVD_I_LAST_SECTOR),
                   f->cache ? "read whole" : "read in pieces", f->bup_only ? " (from the BUP)" : "",
                   f->title_vob_blocks);
        if (!d->folder)
            av_bprintf(bp, ", IFO at %"PRId64", BUP at %"PRId64", title VOBs at %"PRIu32, f->ifo_sector,
                       f->bup_sector, f->title_vob_start);
        av_bprintf(bp, ", %d program chain(s), %d VOBU start(s)%s%s\n", f->nb_pgcs, f->nb_vobu_map,
                   f->vobu_map_distrusted ? ", VOBU map not trusted" : "", f->no_pgcit ? ", no VTS_PGCIT" : "");
        for (int p = 0; p < f->nb_pgcs; p++) {
            const DVDPgc *g = f->pgcs[p];
            if (!g) {
                av_bprintf(bp, "  PGC %d: left out%s\n", p + 1, f->pgc_same_as[p] >= 0 ? " (same start as an "
                           "earlier one)" : "");
                continue;
            }
            av_bprintf(bp, "  PGC %d: %u program(s), %u cell(s), %u+%u+%u commands\n", p + 1, g->nr_of_programs,
                       g->nr_of_cells, g->nr_of_pre, g->nr_of_post, g->nr_of_cell);
            for (int c = 0; c < g->nr_of_cells; c++)
                av_bprintf(bp, "    cell %d: vob %u cell %u, sectors %"PRIu32"-%"PRIu32" (last VOBU %"PRIu32")\n",
                           c + 1, g->cells[c].vob_id_nr, g->cells[c].cell_nr, g->cells[c].first_sector,
                           g->cells[c].last_sector, g->cells[c].last_vobu_start_sector);
        }
    }
    if (d->text) {
        av_bprintf(bp, "text data, language \"%.3s\":\n", d->text->language);
        dump_text(d->text->root, 0, bp);
    }
}
