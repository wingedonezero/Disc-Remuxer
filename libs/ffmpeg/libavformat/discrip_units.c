/*
 * The shared rip core, stage 1: a track's payloads cut into units, each unit
 * with the PES time that belongs to it.
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

#include "libavcodec/avcodec.h"
#include "libavutil/avassert.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

/* A payload written with a time: the time belongs to the first unit whose
 * anchor lies in [pos, pos + span). */
typedef struct Record {
    int64_t pos, span, time;
} Record;

struct DRCutter {
    void                 *log;
    const DRCodec        *codec;
    AVCodecParserContext *parser;
    AVCodecContext       *avctx;
    DRUnitCb              cb;
    void                 *opaque;

    Record  *rec;              /* records rec[rec_head .. nb_rec) are pending */
    int      nb_rec, rec_head, rec_cap;

    int64_t  in_pos;           /* stream bytes written so far */

    /* Bytes not yet given out as part of a unit, for the check that every
     * unit the parser returns is the stream's own bytes at its offset. */
    uint8_t *win;
    int64_t  win_pos;          /* stream offset of win[0] */
    int      win_len, win_cap;

    uint8_t *feed;             /* padded copy of the payload for the parser */
    unsigned feed_cap;

    DRCutterStats st;
};

int ff_discrip_cutter_open(DRCutter **cutter, void *logctx, enum AVCodecID codec,
                           DRUnitCb cb, void *opaque)
{
    DRCutter *c;

    *cutter = NULL;
    if (!(c = av_mallocz(sizeof(*c))))
        return AVERROR(ENOMEM);
    c->log    = logctx;
    c->cb     = cb;
    c->opaque = opaque;
    c->codec  = ff_discrip_codec(codec);
    if (!c->codec) {
        av_log(logctx, AV_LOG_ERROR, "Rip core: codec %s has no entry in the codec table: "
               "its track cannot be ripped\n", avcodec_get_name(codec));
        av_free(c);
        return AVERROR(ENOSYS);
    }
    c->parser = av_parser_init(codec);
    c->avctx  = avcodec_alloc_context3(NULL);
    if (!c->parser || !c->avctx) {
        int ret = c->avctx ? AVERROR(ENOSYS) : AVERROR(ENOMEM);
        if (!c->parser)
            av_log(logctx, AV_LOG_ERROR, "Rip core: FFmpeg has no parser for %s\n", c->codec->name);
        ff_discrip_cutter_close(&c);
        return ret;
    }
    c->avctx->codec_id   = codec;
    c->avctx->codec_type = avcodec_get_type(codec);
    *cutter = c;
    return 0;
}

void ff_discrip_cutter_close(DRCutter **cutter)
{
    DRCutter *c = *cutter;

    if (!c)
        return;
    av_parser_close(c->parser);
    avcodec_free_context(&c->avctx);
    av_freep(&c->rec);
    av_freep(&c->win);
    av_freep(&c->feed);
    av_freep(cutter);
}

void ff_discrip_cutter_stats(const DRCutter *c, DRCutterStats *stats)
{
    *stats = c->st;
}

static int add_record(DRCutter *c, int64_t pos, int64_t span, int64_t time)
{
    if (c->rec_head && c->rec_head == c->nb_rec)
        c->rec_head = c->nb_rec = 0;
    if (c->nb_rec == c->rec_cap) {
        if (c->rec_head) {
            memmove(c->rec, c->rec + c->rec_head, (c->nb_rec - c->rec_head) * sizeof(*c->rec));
            c->nb_rec  -= c->rec_head;
            c->rec_head = 0;
        } else {
            Record *r = av_realloc_array(c->rec, c->rec_cap ? 2 * c->rec_cap : 16, sizeof(*r));
            if (!r)
                return AVERROR(ENOMEM);
            c->rec      = r;
            c->rec_cap  = c->rec_cap ? 2 * c->rec_cap : 16;
        }
    }
    c->rec[c->nb_rec++] = (Record){ pos, span, time };
    return 0;
}

/* Records that end at or before stream offset a: their time belongs to no
 * unit (no anchor lies in their bytes). */
static void drop_records(DRCutter *c, int64_t a)
{
    while (c->rec_head < c->nb_rec && c->rec[c->rec_head].pos + c->rec[c->rec_head].span <= a) {
        const Record *r = &c->rec[c->rec_head++];

        av_log(c->log, AV_LOG_TRACE, "Rip core: %s: time %"PRId64" of the payload at byte %"PRId64
               " (%"PRId64" bytes) belongs to no unit\n", c->codec->name, r->time, r->pos, r->span);
        c->st.records_unused++;
    }
}

/* The time for a unit whose anchor is at stream offset a: the first record
 * left after drop_records(), when a lies in it; it is used up. */
static int64_t take_time(DRCutter *c, int64_t a)
{
    drop_records(c, a);
    if (c->rec_head < c->nb_rec && c->rec[c->rec_head].pos <= a)
        return c->rec[c->rec_head++].time;
    return AV_NOPTS_VALUE;
}

static int window_add(DRCutter *c, const uint8_t *data, int size)
{
    if (c->win_len + size > c->win_cap) {
        int cap = FFMAX(2 * c->win_cap, c->win_len + size);
        uint8_t *w = av_realloc(c->win, cap);
        if (!w)
            return AVERROR(ENOMEM);
        c->win     = w;
        c->win_cap = cap;
    }
    memcpy(c->win + c->win_len, data, size);
    c->win_len += size;
    return 0;
}

/* One unit from the parser: check it, time it, give it out. */
static int unit_out(DRCutter *c, const uint8_t *data, int size)
{
    int64_t off = c->parser->frame_offset, rel = off - c->win_pos;
    int64_t time = AV_NOPTS_VALUE;
    int unit, a;

    /* Invariant: the unit is the stream's bytes at its offset, and units
     * follow each other in the stream. */
    if (rel < 0 || rel + size > c->win_len || memcmp(c->win + rel, data, size)) {
        av_log(c->log, AV_LOG_ERROR, "Rip core: %s: unit check FAIL: %d bytes at stream offset %"PRId64
               " are not the stream's bytes there (kept bytes %"PRId64"..%"PRId64")\n",
               c->codec->name, size, off, c->win_pos, c->win_pos + c->win_len);
        return AVERROR_BUG;
    }
    /* the bytes up to the end of this output are given out */
    c->win_len -= rel + size;
    memmove(c->win, c->win + rel + size, c->win_len);
    c->win_pos  = off + size;

    unit = !c->codec->check || c->codec->check(data, size);
    if (!unit) {
        av_log(c->log, AV_LOG_DEBUG, "Rip core: %s: %d bytes at stream offset %"PRId64" are not a unit: "
               "left out\n", c->codec->name, size, off);
        c->st.skipped++;
        c->st.skipped_bytes += size;
        return 0;
    }
    a = c->codec->anchor(data, size);
    if (a >= 0)
        time = take_time(c, off + a);
    else
        drop_records(c, off);

    c->st.units++;
    if (time != AV_NOPTS_VALUE)
        c->st.timed++;
    return c->cb(c->opaque, &(DRUnit){ data, size, time, off });
}

int ff_discrip_cutter_write(DRCutter *c, const uint8_t *data, int size, int64_t time)
{
    const uint8_t *p;
    int left, ret;

    if (size <= 0)
        return 0;
    if (time != AV_NOPTS_VALUE) {
        if ((ret = add_record(c, c->in_pos, size, time)) < 0)
            return ret;
        c->st.records++;
    }
    if ((ret = window_add(c, data, size)) < 0)
        return ret;
    av_fast_padded_malloc(&c->feed, &c->feed_cap, size);
    if (!c->feed)
        return AVERROR(ENOMEM);
    memcpy(c->feed, data, size);

    p    = c->feed;
    left = size;
    while (left > 0) {
        uint8_t *out;
        int out_size;
        int used = av_parser_parse2(c->parser, c->avctx, &out, &out_size, p, left,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, c->in_pos);

        c->in_pos += used;
        p         += used;
        left      -= used;
        if (out_size && (ret = unit_out(c, out, out_size)) < 0)
            return ret;
    }
    c->st.bytes += size;
    return 0;
}

int ff_discrip_cutter_flush(DRCutter *c)
{
    for (;;) {
        uint8_t *out;
        int out_size, ret;

        av_parser_parse2(c->parser, c->avctx, &out, &out_size, NULL, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, c->in_pos);
        if (!out_size)
            break;
        if ((ret = unit_out(c, out, out_size)) < 0)
            return ret;
    }
    while (c->rec_head < c->nb_rec) {
        c->st.records_unused++;
        c->rec_head++;
    }
    if (c->win_len)
        av_log(c->log, AV_LOG_DEBUG, "Rip core: %s: %d bytes at the end of the stream are in no unit\n",
               c->codec->name, c->win_len);
    av_log(c->log, AV_LOG_DEBUG, "Rip core: %s: %"PRId64" bytes, %"PRId64" timed payloads -> %"PRId64
           " units (%"PRId64" timed), %"PRId64" payload times belong to no unit\n", c->codec->name,
           c->st.bytes, c->st.records, c->st.units, c->st.timed, c->st.records_unused);
    return 0;
}
