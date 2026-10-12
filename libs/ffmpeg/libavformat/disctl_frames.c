/*
 * The title timeline, stage 1 (the frame step): FFmpeg's parser finds the
 * frames ("units") of a track's payloads, in disc mode where the parser has
 * the disc reader's rule; each unit gets the PES time that belongs to it (the
 * first unit whose anchor lies in a payload takes its time, ISO/IEC 13818-1
 * 2.4.3.7); units that do not belong to the track (DRCodec.check) and bytes
 * the parser says are no unit are left out, each run of left-out bytes said
 * with what it is, where it lies on the disc and what is done.
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

#include "disctl.h"

/* A payload written with a time: the time belongs to the first unit whose
 * anchor lies in [pos, pos + span). */
typedef struct Record {
    int64_t pos, span, time;
} Record;

struct DRFrames {
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

    /* where payloads came from: (stream offset, block) per payload whose
     * block is known, for the place of left-out bytes */
    int64_t (*blk)[2];
    int      nb_blk, blk_cap;

    char     label[48];        /* "Track 1" */
    char     part[32];         /* "title", "segment 2" */
    int      seg;
    DRPlaceCb place;
    void    *place_opaque;

    DRFramesStats st;
    int64_t  split_units;      /* H.264 / HEVC: frames whose first bytes lie in an earlier payload than their
                                  anchor (a delimiter or SEI muxed into the packet before) */
};

int ff_disctl_frames_open(DRFrames **frames, void *logctx, enum AVCodecID codec,
                           DRUnitCb cb, void *opaque)
{
    DRFrames *c;

    *frames = NULL;
    if (!(c = av_mallocz(sizeof(*c))))
        return AVERROR(ENOMEM);
    c->log    = logctx;
    c->cb     = cb;
    c->opaque = opaque;
    c->codec  = ff_disctl_codec(codec);
    if (c->codec) {
        snprintf(c->label, sizeof(c->label), "Title timeline: %s", c->codec->name);
        snprintf(c->part, sizeof(c->part), "title");
    }
    if (!c->codec) {
        av_log(logctx, AV_LOG_ERROR, "Title timeline: codec %s has no entry in the codec table: "
               "its track cannot be ripped\n", avcodec_get_name(codec));
        av_free(c);
        return AVERROR(ENOSYS);
    }
    c->parser = av_parser_init(codec);
    c->avctx  = avcodec_alloc_context3(NULL);
    if (c->parser && c->codec->disc_parser)
        c->parser->flags |= PARSER_FLAG_DISC;
    if (!c->parser || !c->avctx) {
        int ret = c->avctx ? AVERROR(ENOSYS) : AVERROR(ENOMEM);
        if (!c->parser)
            av_log(logctx, AV_LOG_ERROR, "Title timeline: FFmpeg has no parser for %s\n", c->codec->name);
        ff_disctl_frames_close(&c);
        return ret;
    }
    c->avctx->codec_id   = codec;
    c->avctx->codec_type = avcodec_get_type(codec);
    *frames = c;
    return 0;
}

void ff_disctl_frames_close(DRFrames **frames)
{
    DRFrames *c = *frames;

    if (!c)
        return;
    if (c->split_units)
        av_log(c->log, AV_LOG_WARNING, "Title timeline: %s: %"PRId64" frames begin in the packet before the one "
               "with their picture (a badly muxed stream?): each takes the time of the packet with its picture\n",
               c->codec->name, c->split_units);
    av_parser_close(c->parser);
    avcodec_free_context(&c->avctx);
    av_freep(&c->rec);
    av_freep(&c->blk);
    av_freep(&c->win);
    av_freep(&c->feed);
    av_freep(frames);
}

void ff_disctl_frames_stats(const DRFrames *c, DRFramesStats *stats)
{
    *stats = c->st;
}

void ff_disctl_frames_set_report(DRFrames *c, const char *label, const char *part, int seg, DRPlaceCb place,
                                 void *opaque)
{
    snprintf(c->label, sizeof(c->label), "%s", label);
    snprintf(c->part, sizeof(c->part), "%s", part);
    c->seg          = seg;
    c->place        = place;
    c->place_opaque = opaque;
}

void ff_disctl_frames_take_any(DRFrames *c)
{
    if (c->parser)
        c->parser->flags |= PARSER_FLAG_DISC_ANY;
}

void ff_disctl_frames_dvd(DRFrames *c)
{
    if (c->parser)
        c->parser->flags |= PARSER_FLAG_DISC_DVD;
}

void ff_disctl_frames_set_pcm_format(DRFrames *c, int channels, int bits, int samples_per_frame)
{
    c->avctx->ch_layout.nb_channels = channels;
    c->avctx->bits_per_coded_sample = bits;
    c->avctx->frame_size            = samples_per_frame;
}

/* What a run of left-out bytes is. */
enum {
    LEFT_START,     /* the segment's first bytes, before its first frame start */
    LEFT_SKIP,      /* bytes that do not start a frame (skipped to the next frame start) */
    LEFT_REFUSED,   /* a frame that fails its check */
    LEFT_NO_START,  /* bytes in which no frame starts */
    LEFT_END,       /* the start of a frame the segment does not finish */
    LEFT_REST,      /* the segment's last bytes, in no whole frame */
};

/* The block that stream offset off came from, in words (into buf); "" when
 * not known. Blocks of payloads before it are not needed again. */
static void place_of(DRFrames *c, int64_t off, char *buf, int size)
{
    int i = c->nb_blk - 1;

    buf[0] = '\0';
    while (i >= 0 && c->blk[i][0] > off)
        i--;
    if (i > 0) {
        memmove(c->blk, c->blk + i, (c->nb_blk - i) * sizeof(*c->blk));
        c->nb_blk -= i;
        i = 0;
    }
    if (i < 0 || !c->place || c->place(c->place_opaque, c->seg, c->blk[i][1], buf, size) < 0)
        buf[0] = '\0';
}

/* k bytes at stream offset off are left out: said every time, with what
 * they are, where they lie and what is done. */
static void left_out(DRFrames *c, int64_t off, int k, int what)
{
    char where[256], at[272] = "";

    place_of(c, off, where, sizeof(where));
    if (where[0])
        snprintf(at, sizeof(at), " (%s)", where);
    switch (what) {
    case LEFT_START:
        av_log(c->log, AV_LOG_INFO, "%s: the first %d bytes of the %s come before its first frame start: left "
               "out%s; the track starts with its first whole frame\n", c->label, k, c->part, at);
        break;
    case LEFT_SKIP:
        av_log(c->log, AV_LOG_WARNING, "%s: %d bytes at stream offset %"PRId64" do not start a frame (damaged or "
               "cut data): left out%s; the track goes on at the next frame start\n", c->label, k, off, at);
        break;
    case LEFT_REFUSED:
        av_log(c->log, AV_LOG_WARNING, "%s: a frame of %d bytes at stream offset %"PRId64" fails its check "
               "(damaged data): left out%s\n", c->label, k, off, at);
        break;
    case LEFT_NO_START:
        av_log(c->log, AV_LOG_WARNING, "%s: %d bytes at stream offset %"PRId64" with no frame start in them "
               "(damaged or cut data): left out%s\n", c->label, k, off, at);
        break;
    case LEFT_END:
        av_log(c->log, AV_LOG_INFO, "%s: the %s ends inside a frame: its last %d bytes are the start of a frame it "
               "does not finish: left out%s; the track ends with the frame before\n", c->label, c->part, k, at);
        break;
    default:
        av_log(c->log, AV_LOG_INFO, "%s: the last %d bytes of the %s are in no whole frame: left out%s\n",
               c->label, k, c->part, at);
    }
    c->st.skipped++;
    c->st.skipped_bytes += k;
}

static int add_record(DRFrames *c, int64_t pos, int64_t span, int64_t time)
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
static void drop_records(DRFrames *c, int64_t a)
{
    while (c->rec_head < c->nb_rec && c->rec[c->rec_head].pos + c->rec[c->rec_head].span <= a) {
        const Record *r = &c->rec[c->rec_head++];

        av_log(c->log, AV_LOG_TRACE, "Title timeline: %s: time %"PRId64" of the payload at byte %"PRId64
               " (%"PRId64" bytes) belongs to no unit\n", c->codec->name, r->time, r->pos, r->span);
        c->st.records_unused++;
    }
}

/* H.264 / HEVC: the unit's first NAL unit (a delimiter, SEI, ...) at stream
 * offset off + its start code lies in an earlier payload than its anchor
 * at stream offset a, the first unit that takes a payload's time (H.222.0
 * 2.4.3.7 gives a payload's time to the access unit whose first byte it
 * carries; the anchor rule takes the time of the payload with the picture
 * and so keeps the time a muxer meant when it put the delimiter into the
 * packet before): logged. */
static void split_check(DRFrames *c, const uint8_t *data, int64_t off, int64_t a)
{
    int64_t first = off;
    int k;

    for (k = 0; k + 2 < (int)(a - off) && !(!data[k] && !data[k + 1] && data[k + 2] == 1); k++)
        ;
    first = off + k;
    for (int i = c->rec_head; i < c->nb_rec && c->rec[i].pos <= a; i++)
        if (c->rec[i].pos + c->rec[i].span > a) {
            if (c->rec[i].pos > first && c->split_units++ < 5)
                av_log(c->log, AV_LOG_WARNING, "Title timeline: %s: a frame at stream offset %"PRId64" begins in "
                       "the packet before the one with its picture (a badly muxed stream?): it takes the time of "
                       "the packet with its picture\n", c->codec->name, first);
            break;
        }
}

/* The time for a unit whose anchor is at stream offset a: the first record
 * left after drop_records(), when a lies in it; it is used up. */
static int64_t take_time(DRFrames *c, int64_t a)
{
    drop_records(c, a);
    if (c->rec_head < c->nb_rec && c->rec[c->rec_head].pos <= a)
        return c->rec[c->rec_head++].time;
    return AV_NOPTS_VALUE;
}

static int window_add(DRFrames *c, const uint8_t *data, int size)
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

/* The picture values a disc-mode parser gives the unit it just cut. */
static void parser_picture(const DRFrames *c, DRPicture *pic)
{
    const AVCodecParserContext *s = c->parser;

    memset(pic, 0, sizeof(*pic));
    switch (s->pict_type) {
    case AV_PICTURE_TYPE_I:
    case AV_PICTURE_TYPE_SI: pic->type = DR_PIC_I; break;
    case AV_PICTURE_TYPE_P:
    case AV_PICTURE_TYPE_SP: pic->type = DR_PIC_P; break;
    case AV_PICTURE_TYPE_B:  pic->type = DR_PIC_B; break;
    default:                 pic->type = DR_PIC_OTHER;
    }
    pic->order    = s->output_picture_number;
    pic->fields   = s->disc_fields;
    pic->key      = s->key_frame > 0;
    pic->rate_num = c->avctx->framerate.num;
    pic->rate_den = c->avctx->framerate.den;
}

/* AC-3 and E-AC-3 share one stream: a unit one track refuses that is a
 * valid frame of the other kind is that track's, not damage. */
static int other_kind(const DRFrames *c, const uint8_t *data, int size)
{
    const DRCodec *o = c->codec->id == AV_CODEC_ID_AC3  ? ff_disctl_codec(AV_CODEC_ID_EAC3) :
                       c->codec->id == AV_CODEC_ID_EAC3 ? ff_disctl_codec(AV_CODEC_ID_AC3)  : NULL;
    return o && o->check && o->check(data, size);
}

/* One unit at stream offset off: check it, time it, give it out. */
static int unit_take(DRFrames *c, const uint8_t *data, int size, int64_t off)
{
    int samples = c->parser && !c->codec->disc_parser ? c->parser->duration : 0;
    int rate    = c->parser && !c->codec->disc_parser ? c->avctx->sample_rate : 0;
    int64_t time = AV_NOPTS_VALUE;
    DRPicture pic;
    int unit, a, ret;

    unit = !c->codec->check || c->codec->check(data, size);
    if (!unit && other_kind(c, data, size)) {
        /* a frame of the stream's other kind (an AC-3 core track's E-AC-3
         * frames, the reverse): not the track's own data */
        av_log(c->log, AV_LOG_DEBUG, "%s: a frame of %d bytes at stream offset %"PRId64" is not this track's "
               "kind: not taken\n", c->label, size, off);
        c->st.skipped++;
        c->st.skipped_bytes += size;
        return 0;
    }
    if (!unit) {
        /* FFmpeg's own parsers (no disc rule) give the bytes before the
         * first sync word as a unit of their own */
        left_out(c, off, size, !off && !c->codec->disc_parser ? LEFT_START : LEFT_REFUSED);
        return 0;
    }
    a = c->codec->anchor ? c->codec->anchor(data, size) : 0;
    if (a > 0 && c->codec->parser_picture)
        split_check(c, data, off, off + a);
    if (a >= 0)
        time = take_time(c, off + a);
    else
        drop_records(c, off);

    c->st.units++;
    if (time != AV_NOPTS_VALUE)
        c->st.timed++;
    if (c->codec->parser_picture)
        parser_picture(c, &pic);
    ret = c->cb(c->opaque, &(DRUnit){ data, size, time, off, samples, rate,
                                      c->codec->parser_picture ? &pic : NULL });
    if (ret < 0 && ret != AVERROR(ENOMEM) && ret != AVERROR(EAGAIN)) {
        /* the stage said why (the line before): here where it is */
        char where[256];
        place_of(c, off, where, sizeof(where));
        av_log(c->log, AV_LOG_ERROR, "%s: the unit at stream offset %"PRId64" of the %s%s%s stops the track\n",
               c->label, off, c->part, where[0] ? " " : "", where);
    }
    return ret;
}

/* The bytes up to stream offset end are given out. */
static void window_consume(DRFrames *c, int64_t end)
{
    int n = end - c->win_pos;

    c->win_len -= n;
    memmove(c->win, c->win + n, c->win_len);
    c->win_pos  = end;
}

/* Bytes that are no unit (the parser's disc_status, PARSER_DISC_*). */
static void not_a_unit(DRFrames *c, int k, int status)
{
    int what = status == PARSER_DISC_REFUSED  ? LEFT_REFUSED  :
               status == PARSER_DISC_NO_START ? LEFT_NO_START :
               status == PARSER_DISC_END      ? LEFT_END      :
               !c->win_pos                    ? LEFT_START    : LEFT_SKIP;
    left_out(c, c->win_pos, k, what);
}

/* One chunk from FFmpeg's parser: a unit, or (disc mode, disc_status 1)
 * bytes that are no unit; disc_status < 0: the stream cannot be cut. */
static int unit_out(DRFrames *c, const uint8_t *data, int size)
{
    int64_t off = c->parser->frame_offset, rel = off - c->win_pos;

    /* Invariant: the unit is the stream's bytes at its offset, and units
     * follow each other in the stream. */
    if (rel < 0 || rel + size > c->win_len || memcmp(c->win + rel, data, size)) {
        av_log(c->log, AV_LOG_ERROR, "Title timeline: %s: unit check FAIL: %d bytes at stream offset %"PRId64
               " are not the stream's bytes there (kept bytes %"PRId64"..%"PRId64")\n",
               c->codec->name, size, off, c->win_pos, c->win_pos + c->win_len);
        return AVERROR_BUG;
    }
    if (c->codec->disc_parser && c->parser->disc_status > 0) {
        not_a_unit(c, size, c->parser->disc_status);
        window_consume(c, off + size);
        return 0;
    }
    window_consume(c, off + size);
    return unit_take(c, data, size, off);
}

/* After a call of a disc-mode parser: the stream cannot be cut. */
static int cut_failed(DRFrames *c)
{
    int err = c->codec->disc_parser ? c->parser->disc_status : 0;

    if (err < 0)
        av_log(c->log, AV_LOG_ERROR, "Title timeline: %s: the stream cannot be cut into units at stream offset %"
               PRId64"\n", c->codec->name, c->win_pos);
    return err < 0 ? err : 0;
}

int ff_disctl_frames_write(DRFrames *c, const uint8_t *data, int size, int64_t time)
{
    return ff_disctl_frames_write_at(c, data, size, time, -1);
}

int ff_disctl_frames_write_at(DRFrames *c, const uint8_t *data, int size, int64_t time, int64_t block)
{
    const uint8_t *p;
    int left, ret;

    if (size <= 0)
        return 0;
    if (block >= 0 && (!c->nb_blk || c->blk[c->nb_blk - 1][1] != block)) {
        if (c->nb_blk == c->blk_cap) {
            int64_t (*b)[2] = av_realloc_array(c->blk, c->blk_cap ? 2 * c->blk_cap : 16, sizeof(*b));
            if (!b)
                return AVERROR(ENOMEM);
            c->blk     = b;
            c->blk_cap = c->blk_cap ? 2 * c->blk_cap : 16;
        }
        c->blk[c->nb_blk][0] = c->in_pos;
        c->blk[c->nb_blk][1] = block;
        c->nb_blk++;
    }
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
        if ((ret = cut_failed(c)) < 0 || (out_size && (ret = unit_out(c, out, out_size)) < 0))
            return ret;
    }
    c->st.bytes += size;
    return 0;
}

int ff_disctl_frames_flush(DRFrames *c)
{
    while (c->parser) {
        uint8_t *out;
        int out_size, ret;

        av_parser_parse2(c->parser, c->avctx, &out, &out_size, NULL, 0,
                         AV_NOPTS_VALUE, AV_NOPTS_VALUE, c->in_pos);
        if ((ret = cut_failed(c)) < 0)
            return ret;
        if (!out_size)
            break;
        if ((ret = unit_out(c, out, out_size)) < 0)
            return ret;
    }
    while (c->rec_head < c->nb_rec) {
        c->st.records_unused++;
        c->rec_head++;
    }
    if (c->win_len) {
        left_out(c, c->win_pos, c->win_len, LEFT_REST);
        window_consume(c, c->win_pos + c->win_len);
    }
    av_log(c->log, AV_LOG_DEBUG, "Title timeline: %s: %"PRId64" bytes, %"PRId64" timed payloads -> %"PRId64
           " units (%"PRId64" timed), %"PRId64" payload times belong to no unit\n", c->codec->name,
           c->st.bytes, c->st.records, c->st.units, c->st.timed, c->st.records_unused);
    return 0;
}
