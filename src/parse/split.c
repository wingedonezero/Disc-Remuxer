/*
 * The splitter (see split.h): the window of bytes not yet decided, the cut
 * and frame engines over it, and each codec's hookup — its rule, the rule's
 * own state, and for H.264 / HEVC the picture pass on each unit. The engine
 * halves keep the decisions of the parser-host engines they replace, step
 * for step, so every cut and every "no unit" run stays byte-identical.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "base/err.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "parse/h264.h"
#include "parse/hevc.h"
#include "parse/rules.h"
#include "parse/split.h"

struct DrSplit {
    void *log;
    enum DrSplitCodec codec;
    const DrCutRule   *cut;
    const DrFrameRule *frames;
    void *state;                /* the cut rule's state (owned) */
    void *fctx;                 /* the frame rule's context (into actx), or NULL */
    union {
        DrAc3RuleCtx ac3;
        DrDcaRuleCtx dca;
        DrMpaRuleCtx mpa;
    } actx;
    DrH264 *h264;
    DrHevc *hevc;
    int take_any, dvd;
    int started;                /* the first bytes came: the options are fixed */

    uint8_t *win;               /* the bytes not yet decided */
    int      len, cap;
    int64_t  pos;               /* stream offset of win[0] */
    uint8_t *out;               /* the chunk given out */
    int      out_cap;
    int final;                  /* no more bytes come */
    int synced;                 /* frame rules: the kept bytes begin with a frame start */
    int over;                   /* frame rules: past the limit: the kept bytes are given up */
    int failed;                 /* the stream cannot be cut (sticky) */
    DrSplitPicture pic;
};

DrSplit *dr_split_new(enum DrSplitCodec codec, void *logctx)
{
    DrSplit *s = dr_mallocz(sizeof(*s));

    if (!s)
        return NULL;
    s->log   = logctx;
    s->codec = codec;
    switch (codec) {
    case DR_SPLIT_VC1:        s->cut = &dr_vc1_cut;        break;
    case DR_SPLIT_H264:       s->cut = &dr_h264_cut;       break;
    case DR_SPLIT_HEVC:       s->cut = &dr_hevc_cut;       break;
    case DR_SPLIT_MLP:        s->cut = &dr_mlp_cut;        break;
    case DR_SPLIT_DVDSUB:     s->cut = &dr_dvdsub_cut;     break;
    case DR_SPLIT_PGS:        s->cut = &dr_pgs_cut;        break;
    case DR_SPLIT_PCM_BLURAY: s->cut = &dr_pcm_bluray_cut; break;
    case DR_SPLIT_PCM_DVD:    s->cut = &dr_pcm_dvd_cut;    break;
    case DR_SPLIT_TEXTST:     s->frames = &dr_textst_frames; break;
    case DR_SPLIT_AC3:        s->frames = &dr_ac3_frames;  s->fctx = &s->actx.ac3; break;
    case DR_SPLIT_DTS:        s->frames = &dr_dts_frames;  s->fctx = &s->actx.dca; break;
    case DR_SPLIT_MPA:        s->frames = &dr_mpa_frames;  s->fctx = &s->actx.mpa; break;
    case DR_SPLIT_ADTS:       s->frames = &dr_adts_frames; break;
    case DR_SPLIT_LATM:       s->frames = &dr_latm_frames; break;
    default:
        dr_free(s);
        return NULL;
    }
    if (codec == DR_SPLIT_H264) {
        if (!(s->h264 = dr_h264_new()) || !(s->state = dr_malloc(sizeof(DrH264Cut)))) {
            dr_split_free(&s);
            return NULL;
        }
        dr_h264_cut_init(s->state, s->h264);
    } else if (codec == DR_SPLIT_HEVC) {
        if (!(s->hevc = dr_hevc_new()) || !(s->state = dr_malloc(sizeof(DrHevcCut)))) {
            dr_split_free(&s);
            return NULL;
        }
        dr_hevc_cut_init(s->state, s->hevc);
    } else if (s->cut && s->cut->state_size) {
        if (!(s->state = dr_mallocz(s->cut->state_size))) {
            dr_split_free(&s);
            return NULL;
        }
    }
    return s;
}

void dr_split_free(DrSplit **ps)
{
    DrSplit *s = *ps;

    if (!s)
        return;
    if (s->h264) {
        dr_h264_close(s->h264, NULL);
        dr_h264_free(&s->h264);
        if (s->state)
            dr_h264_cut_end(s->state);
    }
    if (s->hevc) {
        dr_hevc_free(&s->hevc);
        if (s->state)
            dr_hevc_cut_end(s->state);
    }
    dr_free(s->state);
    dr_free(s->win);
    dr_free(s->out);
    dr_freep(ps);
}

void dr_split_take_any(DrSplit *s)
{
    s->take_any = 1;
}

void dr_split_dvd(DrSplit *s)
{
    s->dvd = 1;
}

void dr_split_pcm_format(DrSplit *s, int channels, int bits, int samples_per_frame)
{
    if (s->codec == DR_SPLIT_PCM_DVD && s->state)
        *(DrPcmDvdCut *)s->state = (DrPcmDvdCut){ channels, bits, samples_per_frame };
}

/* The options are fixed with the first bytes. */
static void start(DrSplit *s)
{
    if (s->started)
        return;
    s->started = 1;
    switch (s->codec) {
    case DR_SPLIT_AC3:
        s->actx.ac3.kind = s->take_any ? 0 : 1;
        break;
    case DR_SPLIT_DTS:
        s->actx.dca.undecided = 1;
        if (s->take_any)
            s->frames = &dr_dts_hd_frames;
        break;
    case DR_SPLIT_MPA:
        s->actx.mpa.dvd = s->dvd;
        break;
    default:
        break;
    }
}

int dr_split_write(DrSplit *s, const uint8_t *data, int size)
{
    if (size <= 0)
        return 0;
    start(s);
    /* frame rules: too many bytes without a frame — the kept bytes and the
     * new ones are given up as one run */
    if (s->frames && s->frames->limit - s->len < size)
        s->over = 1;
    if (s->len + size > s->cap) {
        int cap = 2 * s->cap > s->len + size ? 2 * s->cap : s->len + size;
        uint8_t *w = dr_realloc(s->win, cap);
        if (!w)
            return DR_ERR(ENOMEM);
        s->win = w;
        s->cap = cap;
    }
    memcpy(s->win + s->len, data, size);
    s->len += size;
    return 0;
}

void dr_split_end(DrSplit *s)
{
    start(s);
    s->final = 1;
}

int64_t dr_split_pos(const DrSplit *s)
{
    return s->pos;
}

/* The first k kept bytes are one chunk. */
static int give(DrSplit *s, DrSplitChunk *c, int k, int status)
{
    if (k > s->out_cap) {
        uint8_t *o = dr_realloc(s->out, k);
        if (!o)
            return DR_ERR(ENOMEM);
        s->out     = o;
        s->out_cap = k;
    }
    memcpy(s->out, s->win, k);
    c->data   = s->out;
    c->size   = k;
    c->offset = s->pos;
    c->status = status;
    s->len -= k;
    memmove(s->win, s->win + k, s->len);
    s->pos += k;
    return 1;
}

/* A unit of a cut rule: the codec's own afterword (H.264 / HEVC: no-frame
 * chunks and the picture's values; PGS: no-set chunks). */
static int cut_after(DrSplit *s, DrSplitChunk *c)
{
    if (s->codec == DR_SPLIT_H264) {
        const DrH264Cut *st = s->state;
        DrH264Picture pic;
        int ret;

        if (st->no_unit) {
            c->status = DR_SPLIT_SKIP;
            c->note   = st->why;
            return 1;
        }
        if ((ret = dr_h264_picture(s->h264, s->log, c->data, c->size, &pic)) < 0)
            return s->failed = 1, ret;
        s->pic = (DrSplitPicture){ pic.type, pic.key, pic.order, pic.fields,
                                   pic.rate_num, pic.rate_den };
        c->pic  = &s->pic;
        c->note = pic.note;
        return 1;
    }
    if (s->codec == DR_SPLIT_HEVC) {
        const DrHevcCut *st = s->state;
        DrHevcPicture pic;
        int ret;

        if (st->no_unit) {
            c->status = DR_SPLIT_SKIP;
            return 1;
        }
        if ((ret = dr_hevc_picture(s->hevc, s->log, c->data, c->size, &pic)) < 0)
            return s->failed = 1, ret;
        s->pic = (DrSplitPicture){ pic.type, pic.key, pic.order, pic.fields,
                                   pic.rate_num, pic.rate_den };
        c->pic = &s->pic;
        return 1;
    }
    if (s->codec == DR_SPLIT_PGS && ((PGSDiscCut *)s->state)->no_unit)
        c->status = DR_SPLIT_SKIP;
    return 1;
}

/* The cut engine: units found by walking the stream from a known start. */
static int cut_next(DrSplit *s, DrSplitChunk *c)
{
    int r, k, status = DR_SPLIT_UNIT, ret;

    if (s->codec == DR_SPLIT_H264) {
        DrH264Cut *st = s->state;
        st->no_unit = 0;
        st->why     = NULL;
    } else if (s->codec == DR_SPLIT_HEVC) {
        ((DrHevcCut *)s->state)->no_unit = 0;
    }
    r = s->cut->unit_size(s->log, s->win, s->len, s->final, s->state);
    if (r < 0 && r != DR_CUT_NONE)
        return s->failed = 1, r;            /* the stream cannot be cut */
    if (r > 0) {
        k = r;
    } else if (!r && !s->final) {
        return 0;                           /* the unit goes on in the bytes to come */
    } else if (!r) {
        k      = s->len;
        status = DR_SPLIT_END;              /* the unit is not finished when the stream ends */
    } else {
        int at = s->cut->resync(s->win + 1, s->len - 1);
        if (at >= 0) {
            k      = at + 1;
            status = DR_SPLIT_SKIP;
        } else if (s->final) {
            k      = s->len;
            status = DR_SPLIT_NO_START;
        } else {
            return 0;                       /* a unit may start in the bytes to come */
        }
    }
    if ((ret = give(s, c, k, status)) < 0)
        return ret;
    return status == DR_SPLIT_UNIT ? cut_after(s, c) : 1;
}

/* The frame engine: the disc reader's four steps (sync, size, check,
 * resync) for the codecs whose frames a sync word finds. */
static int frames_next(DrSplit *s, DrSplitChunk *c)
{
    int r, k;

    if (s->over) {                          /* too many bytes without a frame */
        int started = s->synced;            /* a frame start whose frame never ends: refused */
        s->synced = 0;
        s->over   = 0;
        return give(s, c, s->len, started ? DR_SPLIT_REFUSED : DR_SPLIT_NO_START);
    }
    if (!s->synced) {
        r = s->frames->sync(s->fctx, s->win, s->len);
        if (!r) {
            k = s->frames->resync(s->fctx, s->win, s->len);
            if (k == -2)
                goto incomplete;            /* more bytes to tell */
            if (k == -1)
                return give(s, c, s->len, DR_SPLIT_NO_START);
            if (!k)
                k = 1;
            return give(s, c, k, DR_SPLIT_SKIP);
        }
        if (r == 1)
            s->synced = 1;
    }
    k = s->frames->size(s->fctx, s->win, s->len);
    if (k <= 0 || k > s->len)
        goto incomplete;                    /* the frame goes on in the bytes to come */
    s->synced = 0;
    return give(s, c, k, s->frames->check(s->fctx, s->win, k) == 1 ? DR_SPLIT_UNIT : DR_SPLIT_REFUSED);

incomplete:
    if (!s->final)
        return 0;
    {   /* the end: the kept bytes are no frame; beginning with a frame
         * start: a frame the stream does not finish */
        int started = s->synced;
        s->synced = 0;
        return give(s, c, s->len, started ? DR_SPLIT_END : DR_SPLIT_NO_START);
    }
}

int dr_split_next(DrSplit *s, DrSplitChunk *c)
{
    memset(c, 0, sizeof(*c));
    if (s->failed)
        return DR_ERR_INVALID;
    if (!s->len)
        return 0;
    start(s);
    return s->cut ? cut_next(s, c) : frames_next(s, c);
}
