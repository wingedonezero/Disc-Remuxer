/*
 * The title timeline, stage 1 (the frame step): the splitter (src/parse,
 * parse/split.h) cuts a track's payloads into the disc reader's units; each
 * unit gets the PES time that belongs to it (the first unit whose anchor
 * lies in a payload takes its time, ISO/IEC 13818-1 2.4.3.7); units that do
 * not belong to the track (DRCodec.check) and bytes the splitter says are no
 * unit are left out, each run of left-out bytes said with what it is, where
 * it lies on the disc and what is done. MPEG video and E-AC-3, which have
 * no disc rule (DRCodec.split 0), are cut by the stock cutters
 * (parse/stock.h: FFmpeg's parsers' cut, ported).
 */

#include <string.h>


#include "base/err.h"
#include "parse/h264.h"
#include "parse/split.h"
#include "parse/stock.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/bytes.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "timeline/disctl.h"

/* A payload written with a time: the time belongs to the first unit whose
 * anchor lies in [pos, pos + span). */
typedef struct Record {
    int64_t pos, span, time;
} Record;

struct DRFrames {
    void                 *log;
    const DRCodec        *codec;
    DrSplit              *split;   /* the codec's own rule (DRCodec.split), or */
    DrStock              *hp;      /* the stock cutter */
    int64_t               hp_frame_offset;  /* of the chunk it last gave */
    int                   hp_duration, hp_rate;
    DRUnitCb              cb;
    void                 *opaque;

    Record  *rec;              /* records rec[rec_head .. nb_rec) are pending */
    int      nb_rec, rec_head, rec_cap;

    int64_t  in_pos;           /* stream bytes written so far */

    int      rate_num, rate_den;   /* the last frame rate a unit's stream said */

    /* The stock cutter's path: bytes not yet given out as part of a unit,
     * for the check that every unit the parser returns is the stream's own
     * bytes at its offset. */
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

int ff_disctl_frames_open(DRFrames **frames, void *logctx, DRCodecID codec,
                           DRUnitCb cb, void *opaque)
{
    DRFrames *c;

    *frames = NULL;
    if (!(c = dr_mallocz(sizeof(*c))))
        return DR_ERR(ENOMEM);
    c->log    = logctx;
    c->cb     = cb;
    c->opaque = opaque;
    c->codec  = ff_disctl_codec(codec);
    if (c->codec) {
        snprintf(c->label, sizeof(c->label), "Title timeline: %s", c->codec->name);
        snprintf(c->part, sizeof(c->part), "title");
    }
    if (!c->codec) {
        dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: codec %s has no entry in the codec table: "
               "its track cannot be ripped\n", ff_disctl_codec_id_name(codec));
        dr_free(c);
        return DR_ERR(ENOSYS);
    }
    c->rate_den = 1;
    if (c->codec->split) {
        if (!(c->split = dr_split_new(c->codec->split, logctx))) {
            ff_disctl_frames_close(&c);
            return DR_ERR(ENOMEM);
        }
        *frames = c;
        return 0;
    }
    if (codec != DR_CODEC_MPEG1VIDEO && codec != DR_CODEC_MPEG2VIDEO && codec != DR_CODEC_EAC3) {
        dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: %s has no frame cutter\n", c->codec->name);
        ff_disctl_frames_close(&c);
        return DR_ERR(ENOSYS);
    }
    if (!(c->hp = dr_stock_new(codec == DR_CODEC_EAC3 ? DR_STOCK_AC3 : DR_STOCK_MPEGVIDEO))) {
        ff_disctl_frames_close(&c);
        return DR_ERR(ENOMEM);
    }
    *frames = c;
    return 0;
}

void ff_disctl_frames_close(DRFrames **frames)
{
    DRFrames *c = *frames;

    if (!c)
        return;
    if (c->split_units)
        dr_vlog(c->log, DR_VLOG_WARNING, "Title timeline: %s: %"PRId64" frames begin in the packet before the one "
               "with their picture (a badly muxed stream?): each takes the time of the packet with its picture\n",
               c->codec->name, c->split_units);
    dr_split_free(&c->split);
    dr_stock_free(&c->hp);
    dr_freep(&c->rec);
    dr_freep(&c->blk);
    dr_freep(&c->win);
    dr_freep(&c->feed);
    dr_freep(frames);
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
    if (c->split)
        dr_split_take_any(c->split);
}

void ff_disctl_frames_dvd(DRFrames *c)
{
    if (c->split)
        dr_split_dvd(c->split);
}

void ff_disctl_frames_set_pcm_format(DRFrames *c, int channels, int bits, int samples_per_frame)
{
    if (c->split)
        dr_split_pcm_format(c->split, channels, bits, samples_per_frame);
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
        dr_vlog(c->log, DR_VLOG_INFO, "%s: the first %d bytes of the %s come before its first frame start: left "
               "out%s; the track starts with its first whole frame\n", c->label, k, c->part, at);
        break;
    case LEFT_SKIP:
        dr_vlog(c->log, DR_VLOG_WARNING, "%s: %d bytes at stream offset %"PRId64" do not start a frame (damaged or "
               "cut data): left out%s; the track goes on at the next frame start\n", c->label, k, off, at);
        break;
    case LEFT_REFUSED:
        dr_vlog(c->log, DR_VLOG_WARNING, "%s: a frame of %d bytes at stream offset %"PRId64" fails its check "
               "(damaged data): left out%s\n", c->label, k, off, at);
        break;
    case LEFT_NO_START:
        dr_vlog(c->log, DR_VLOG_WARNING, "%s: %d bytes at stream offset %"PRId64" with no frame start in them "
               "(damaged or cut data): left out%s\n", c->label, k, off, at);
        break;
    case LEFT_END:
        dr_vlog(c->log, DR_VLOG_INFO, "%s: the %s ends inside a frame: its last %d bytes are the start of a frame it "
               "does not finish: left out%s; the track ends with the frame before\n", c->label, c->part, k, at);
        break;
    default:
        dr_vlog(c->log, DR_VLOG_INFO, "%s: the last %d bytes of the %s are in no whole frame: left out%s\n",
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
            Record *r = dr_realloc_array(c->rec, c->rec_cap ? 2 * c->rec_cap : 16, sizeof(*r));
            if (!r)
                return DR_ERR(ENOMEM);
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

        dr_vlog(c->log, DR_VLOG_TRACE, "Title timeline: %s: time %"PRId64" of the payload at byte %"PRId64
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
                dr_vlog(c->log, DR_VLOG_WARNING, "Title timeline: %s: a frame at stream offset %"PRId64" begins in "
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
    return DR_NOPTS_VALUE;
}

static int window_add(DRFrames *c, const uint8_t *data, int size)
{
    if (c->win_len + size > c->win_cap) {
        int cap = DR_MAX(2 * c->win_cap, c->win_len + size);
        uint8_t *w = dr_realloc(c->win, cap);
        if (!w)
            return DR_ERR(ENOMEM);
        c->win     = w;
        c->win_cap = cap;
    }
    memcpy(c->win + c->win_len, data, size);
    c->win_len += size;
    return 0;
}

/* The picture values the splitter gives a video unit; the frame rate stays
 * the last one a unit's stream said. */
static void chunk_picture(DRFrames *c, const DrSplitChunk *ch, DRPicture *pic)
{
    memset(pic, 0, sizeof(*pic));
    switch (ch->pic->type) {
    case DR_H26X_PIC_I:
    case DR_H26X_PIC_SI: pic->type = DR_PIC_I; break;
    case DR_H26X_PIC_P:
    case DR_H26X_PIC_SP: pic->type = DR_PIC_P; break;
    case DR_H26X_PIC_B:  pic->type = DR_PIC_B; break;
    default:             pic->type = DR_PIC_OTHER;
    }
    pic->order  = ch->pic->order;
    pic->fields = ch->pic->fields;
    pic->key    = ch->pic->key > 0;
    if (ch->pic->rate_den) {
        c->rate_num = ch->pic->rate_num;
        c->rate_den = ch->pic->rate_den;
    }
    pic->rate_num = c->rate_num;
    pic->rate_den = c->rate_den;
}

/* AC-3 and E-AC-3 share one stream: a unit one track refuses that is a
 * valid frame of the other kind is that track's, not damage. */
static int other_kind(const DRFrames *c, const uint8_t *data, int size)
{
    const DRCodec *o = c->codec->id == DR_CODEC_AC3  ? ff_disctl_codec(DR_CODEC_EAC3) :
                       c->codec->id == DR_CODEC_EAC3 ? ff_disctl_codec(DR_CODEC_AC3)  : NULL;
    return o && o->check && o->check(data, size);
}

/* One unit at stream offset off: check it, time it, give it out. pic: the
 * picture values the splitter gave; samples / rate: what the stock cutter
 * read (0 otherwise). */
static int unit_take(DRFrames *c, const uint8_t *data, int size, int64_t off,
                     const DRPicture *pic, int samples, int rate)
{
    int64_t time = DR_NOPTS_VALUE;
    int unit, a, ret;

    unit = !c->codec->check || c->codec->check(data, size);
    if (!unit && other_kind(c, data, size)) {
        /* a frame of the stream's other kind (an AC-3 core track's E-AC-3
         * frames, the reverse): not the track's own data */
        dr_vlog(c->log, DR_VLOG_DEBUG, "%s: a frame of %d bytes at stream offset %"PRId64" is not this track's "
               "kind: not taken\n", c->label, size, off);
        c->st.skipped++;
        c->st.skipped_bytes += size;
        return 0;
    }
    if (!unit) {
        /* FFmpeg's own parsers (no disc rule) give the bytes before the
         * first sync word as a unit of their own */
        left_out(c, off, size, !off && !c->codec->split ? LEFT_START : LEFT_REFUSED);
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
    if (time != DR_NOPTS_VALUE)
        c->st.timed++;
    ret = c->cb(c->opaque, &(DRUnit){ data, size, time, off, samples, rate, pic });
    if (ret < 0 && ret != DR_ERR(ENOMEM) && ret != DR_ERR(EAGAIN)) {
        /* the stage said why (the line before): here where it is */
        char where[256];
        place_of(c, off, where, sizeof(where));
        dr_vlog(c->log, DR_VLOG_ERROR, "%s: the unit at stream offset %"PRId64" of the %s%s%s stops the track\n",
               c->label, off, c->part, where[0] ? " " : "", where);
    }
    return ret;
}

/* Bytes that are no unit; the splitter's note says why when it tells. */
static void not_a_unit(DRFrames *c, const DrSplitChunk *ch)
{
    int what;

    if (ch->note) {
        char where[256], at[272] = "";
        place_of(c, ch->offset, where, sizeof(where));
        if (where[0])
            snprintf(at, sizeof(at), " (%s)", where);
        dr_vlog(c->log, DR_VLOG_WARNING, "%s: %d bytes at stream offset %"PRId64"%s: %s\n", c->label, ch->size,
               ch->offset, at, ch->note);
        c->st.skipped++;
        c->st.skipped_bytes += ch->size;
        return;
    }
    what = ch->status == DR_SPLIT_REFUSED  ? LEFT_REFUSED  :
           ch->status == DR_SPLIT_NO_START ? LEFT_NO_START :
           ch->status == DR_SPLIT_END      ? LEFT_END      :
           !ch->offset                     ? LEFT_START    : LEFT_SKIP;
    left_out(c, ch->offset, ch->size, what);
}

/* Every chunk the splitter has: units out, runs of left-out bytes said. */
static int drain(DRFrames *c)
{
    DrSplitChunk ch;
    int r, ret;

    while ((r = dr_split_next(c->split, &ch)) > 0) {
        DRPicture pic;

        if (ch.status) {
            not_a_unit(c, &ch);
            continue;
        }
        /* what the splitter found about the frame, said with its place */
        if (ch.note) {
            char where[256], at[272] = "";
            place_of(c, ch.offset, where, sizeof(where));
            if (where[0])
                snprintf(at, sizeof(at), " (%s)", where);
            dr_vlog(c->log, DR_VLOG_WARNING, "%s: the frame at stream offset %"PRId64"%s: %s\n", c->label,
                   ch.offset, at, ch.note);
        }
        if (ch.pic)
            chunk_picture(c, &ch, &pic);
        if ((ret = unit_take(c, ch.data, ch.size, ch.offset, ch.pic ? &pic : NULL, 0, 0)) < 0)
            return ret;
    }
    if (r < 0) {
        dr_vlog(c->log, DR_VLOG_ERROR, "Title timeline: %s: the stream cannot be cut into units at stream offset %"
               PRId64"\n", c->codec->name, dr_split_pos(c->split));
        return r;
    }
    return 0;
}

/* The bytes up to stream offset end are given out (the stock cutter's
 * window). */
static void window_consume(DRFrames *c, int64_t end)
{
    int n = end - c->win_pos;

    c->win_len -= n;
    memmove(c->win, c->win + n, c->win_len);
    c->win_pos  = end;
}

/* One chunk from the stock cutter (parse/stock.h). */
static int unit_out(DRFrames *c, const uint8_t *data, int size)
{
    int64_t off = c->hp_frame_offset, rel = off - c->win_pos;

    /* Invariant: the unit is the stream's bytes at its offset, and units
     * follow each other in the stream. */
    if (rel < 0 || rel + size > c->win_len || memcmp(c->win + rel, data, size)) {
        dr_vlog(c->log, DR_VLOG_ERROR, "Title timeline: %s: unit check FAIL: %d bytes at stream offset %"PRId64
               " are not the stream's bytes there (kept bytes %"PRId64"..%"PRId64")\n",
               c->codec->name, size, off, c->win_pos, c->win_pos + c->win_len);
        return DR_ERR_BUG;
    }
    window_consume(c, off + size);
    return unit_take(c, data, size, off, NULL, c->hp_duration, c->hp_rate);
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
            int64_t (*b)[2] = dr_realloc_array(c->blk, c->blk_cap ? 2 * c->blk_cap : 16, sizeof(*b));
            if (!b)
                return DR_ERR(ENOMEM);
            c->blk     = b;
            c->blk_cap = c->blk_cap ? 2 * c->blk_cap : 16;
        }
        c->blk[c->nb_blk][0] = c->in_pos;
        c->blk[c->nb_blk][1] = block;
        c->nb_blk++;
    }
    if (time != DR_NOPTS_VALUE) {
        if ((ret = add_record(c, c->in_pos, size, time)) < 0)
            return ret;
        c->st.records++;
    }
    if (c->split) {
        c->in_pos += size;
        if ((ret = dr_split_write(c->split, data, size)) < 0)
            return ret;
        if ((ret = drain(c)) < 0)
            return ret;
        c->st.bytes += size;
        return 0;
    }
    if ((ret = window_add(c, data, size)) < 0)
        return ret;
    if ((unsigned)size + DR_BUF_PADDING > c->feed_cap) {
        uint8_t *f = dr_realloc(c->feed, size + DR_BUF_PADDING);
        if (!f)
            return DR_ERR(ENOMEM);
        c->feed     = f;
        c->feed_cap = size + DR_BUF_PADDING;
    }
    memcpy(c->feed, data, size);
    memset(c->feed + size, 0, DR_BUF_PADDING);

    p    = c->feed;
    left = size;
    while (left > 0) {
        const uint8_t *out;
        int out_size;
        int used = dr_stock_parse(c->hp, p, left, c->in_pos, &out, &out_size,
                                  &c->hp_frame_offset, &c->hp_duration, &c->hp_rate);

        if (used < 0)
            return used;

        c->in_pos += used;
        p         += used;
        left      -= used;
        if (out_size && (ret = unit_out(c, out, out_size)) < 0)
            return ret;
    }
    c->st.bytes += size;
    return 0;
}

int ff_disctl_frames_flush(DRFrames *c)
{
    int ret;

    if (c->split) {
        dr_split_end(c->split);
        if ((ret = drain(c)) < 0)
            return ret;
    }
    while (!c->split && c->hp) {
        const uint8_t *out;
        int out_size;

        if ((ret = dr_stock_parse(c->hp, NULL, 0, c->in_pos, &out, &out_size,
                                  &c->hp_frame_offset, &c->hp_duration, &c->hp_rate)) < 0)
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
    dr_vlog(c->log, DR_VLOG_DEBUG, "Title timeline: %s: %"PRId64" bytes, %"PRId64" timed payloads -> %"PRId64
           " units (%"PRId64" timed), %"PRId64" payload times belong to no unit\n", c->codec->name,
           c->st.bytes, c->st.records, c->st.units, c->st.timed, c->st.records_unused);
    return 0;
}
