/*
 * The title timeline: cuts (disctl.h). One part of a title cut at places
 * that are no clean break of the disc: of the title's frames (in the
 * timeline's order) the stage keeps the part's and puts them on the part's
 * clock. The master video, in decoding order: everything before the part's
 * key frame belongs to the part before; from the key frame on the part's
 * own, but for its leading pictures that do not decode without the group
 * before it (ISO/IEC 13818-2 closed_gop / broken_link, H.264 IDR and
 * recovery points, H.265 7.4.2.2 RADL / RASL pictures, SMPTE 421M entry
 * points) where leaving them out leaves a stream the codec allows
 * (ff_disctl_leading_kept()); from the next cut's key frame on the next
 * part's. Audio by time, as stream-copy cutting does: a frame that starts
 * before the part's 0 is the part before's, so a part's audio starts 0 to
 * one frame after its 0; PCM between two samples, at the first one at or
 * after the part's 0.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/err.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/vlog.h"

#include "timeline/disctl.h"

enum { BEFORE, IN, AFTER };

typedef struct CutTrack {
    int        kind;         /* DRCutTrack */
    int        rate, bpf;    /* PCM: samples per second, bytes per sample frame */
    int64_t    frames;       /* given */
    int64_t    first;        /* the first given one's time (part clock), INT64_MIN none */
    int64_t    left;         /* DR_CUT_TRACK_NONE: frames left out */
} CutTrack;

struct DRCut {
    void      *log;
    CutTrack  *t;
    int        nb_tracks;
    DRCutPlace from, to;     /* the part's cut and the next one's (has_from / has_to) */
    int        has_from, has_to;
    int        part, nb_parts;
    int        codec;        /* DRCutCodec */
    int        phase;
    int        leading;      /* still after the part's key frame, before the next key frame */
    int        seen_to;
    DRCutResult r;
};

int ff_disctl_leading_decodes(int start, int codec, int rasl)
{
    return start == DR_START_CLEAN || (codec == DR_CUT_HEVC && !rasl);
}

int ff_disctl_leading_kept(int start, int codec, int rasl, int ref)
{
    if (ff_disctl_leading_decodes(start, codec, rasl))
        return 1;
    return !((codec == DR_CUT_HEVC && rasl) || (codec == DR_CUT_H264 && !ref));
}

int ff_disctl_cut_open(DRCut **pc, void *log, const DRCutPlace *cuts, int nb_cuts, int part, int codec,
                       const int *kinds, int nb_tracks)
{
    DRCut *c;

    *pc = NULL;
    if (part < 0 || part > nb_cuts)
        return DR_ERR(EINVAL);
    for (int i = 1; i < nb_cuts; i++)
        if (cuts[i].zero <= cuts[i - 1].zero)
            return DR_ERR(EINVAL);
    if (!(c = dr_mallocz(sizeof(*c))) || !(c->t = dr_calloc(nb_tracks ? nb_tracks : 1, sizeof(*c->t)))) {
        if (c)
            dr_free(c);
        return DR_ERR(ENOMEM);
    }
    c->nb_tracks = nb_tracks;
    for (int i = 0; i < nb_tracks; i++) {
        c->t[i].kind  = kinds[i];
        c->t[i].first = INT64_MIN;
    }
    c->log      = log;
    c->part     = part;
    c->nb_parts = nb_cuts + 1;
    c->codec    = codec;
    if ((c->has_from = part > 0))
        c->from = cuts[part - 1];
    if ((c->has_to = part < nb_cuts))
        c->to = cuts[part];
    c->phase   = c->has_from ? BEFORE : IN;
    c->r.first = INT64_MAX;
    *pc = c;
    return 0;
}

static int start_of(unsigned flags)
{
    return flags & DR_F_START_CLEAN  ? DR_START_CLEAN  :
           flags & DR_F_START_BROKEN ? DR_START_BROKEN :
           flags & DR_F_START_OPEN   ? DR_START_OPEN   : DR_START_UNKNOWN;
}

void ff_disctl_cut_pcm(DRCut *c, int track, int rate, int bytes_per_sample_frame)
{
    if (track >= 0 && track < c->nb_tracks) {
        c->t[track].rate = rate;
        c->t[track].bpf  = bytes_per_sample_frame;
    }
}

/* The first sample at or after time `at` of a PCM frame that starts at t0
 * (its number from 0; samples at t0 + n / rate). */
static int64_t sample_at(int64_t t0, int64_t at, int rate)
{
    int64_t num = (at - t0) * rate;

    return num <= 0 ? 0 : (num + DR_TICKS_PER_SECOND - 1) / DR_TICKS_PER_SECOND;
}

/* A PCM frame cut down to the part's samples: 1 when some are left, 0 when
 * none, < 0 on error. */
static int cut_pcm(DRCut *c, CutTrack *t, DRFrame *f)
{
    int64_t n, from, to;

    if (t->rate <= 0 || t->bpf <= 0 || f->size % t->bpf)
        return DR_ERR_INVALID;
    n    = f->size / t->bpf;
    from = c->has_from ? DR_MIN(sample_at(f->time, c->from.zero, t->rate), n) : 0;
    to   = c->has_to ? DR_MIN(sample_at(f->time, c->to.zero, t->rate), n) : n;
    if (from >= to)
        return 0;
    f->data += from * t->bpf;
    f->size  = (int)((to - from) * t->bpf);
    f->dur   = (to - from) * DR_TICKS_PER_SECOND / t->rate;
    f->time += from * DR_TICKS_PER_SECOND / t->rate;
    return 1;
}

/* The master video in decoding order: 1 the part's, 0 not. */
static int cut_video(DRCut *c, DRFrame *f)
{
    int key = !!(f->flags & DR_F_KEY);

    if (f->time == DR_NOPTS_VALUE)
        return c->phase == IN;
    switch (c->phase) {
    case BEFORE:
        if (!key || f->time != c->from.key_time)
            return 0;
        c->phase   = IN;
        c->leading = 1;
        c->r.start = start_of(f->flags);
        return 1;
    case IN:
        if (c->has_to && key && f->time == c->to.key_time) {
            c->phase   = AFTER;
            c->seen_to = 1;
            return 0;
        }
        if (c->leading && key)
            c->leading = 0;
        else if (c->leading && f->time < c->from.key_time) {
            int rasl = !!(f->flags & DR_F_RASL), ref = !!(f->flags & DR_F_REF);
            if (!ff_disctl_leading_kept(c->r.start, c->codec, rasl, ref)) {
                c->r.left_out++;
                return 0;
            }
            c->r.undecodable += !ff_disctl_leading_decodes(c->r.start, c->codec, rasl);
        }
        return 1;
    default:
        return 0;
    }
}

int ff_disctl_cut_frame(DRCut *c, int track, DRFrame *f)
{
    CutTrack *t = track >= 0 && track < c->nb_tracks ? &c->t[track] : NULL;
    int ret;

    if (!t || t->kind == DR_CUT_TRACK_NONE) {
        if (t)
            t->left++;
        return 0;
    }
    if (t->kind == DR_CUT_TRACK_VIDEO)
        ret = cut_video(c, f);
    else if (f->time == DR_NOPTS_VALUE)
        ret = 0;                        /* audio without a time has no place in a part */
    else if (t->kind == DR_CUT_TRACK_PCM)
        ret = cut_pcm(c, t, f);
    else
        ret = (!c->has_from || f->time >= c->from.zero) && (!c->has_to || f->time < c->to.zero);
    if (ret <= 0)
        return ret;
    if (c->has_from && f->time != DR_NOPTS_VALUE)
        f->time -= c->from.zero;
    t->frames++;
    if (f->time != DR_NOPTS_VALUE && (t->first == INT64_MIN || f->time < t->first))
        t->first = f->time;
    if (t->kind == DR_CUT_TRACK_VIDEO) {
        c->r.pictures++;
        c->r.first = DR_MIN(c->r.first, f->time);
    }
    return 1;
}

void ff_disctl_cut_track(const DRCut *c, int track, int64_t *frames, int64_t *first)
{
    *frames = track >= 0 && track < c->nb_tracks ? c->t[track].frames : 0;
    *first  = track >= 0 && track < c->nb_tracks ? c->t[track].first : INT64_MIN;
}

static const char *start_name(int start)
{
    return start == DR_START_CLEAN ? "clean" : start == DR_START_OPEN ? "open" :
           start == DR_START_BROKEN ? "a broken link" : "kind not told";
}

int ff_disctl_cut_finish(DRCut *c)
{
    int64_t ns = 0;

    if (c->r.first == INT64_MAX)
        c->r.first = 0;
    if (c->has_from && c->phase == BEFORE) {
        ns = c->from.key_time * 25 / 27;
        dr_vlog(c->log, DR_VLOG_ERROR, "Cut part %d/%d: its key frame (at %"PRId64" ns on the title timeline) was not "
                "met: the cuts do not belong to this reading of the title\n", c->part + 1, c->nb_parts, ns);
        return DR_ERR_INVALID;
    }
    if (c->has_to && !c->seen_to) {
        ns = c->to.key_time * 25 / 27;
        dr_vlog(c->log, DR_VLOG_ERROR, "Cut part %d/%d: the next part's key frame (at %"PRId64" ns on the title "
                "timeline) was not met: the cuts do not belong to this reading of the title\n", c->part + 1,
                c->nb_parts, ns);
        return DR_ERR_INVALID;
    }
    if (c->r.first) {
        dr_vlog(c->log, DR_VLOG_ERROR, "Cut part %d/%d: its first picture is at %+"PRId64" ticks on the part's clock, "
                "not at 0: the cut's place does not match the title's pictures\n", c->part + 1, c->nb_parts,
                c->r.first);
        return DR_ERR_INVALID;
    }
    for (int i = 0; i < c->nb_tracks; i++)
        if (c->t[i].left)
            dr_vlog(c->log, DR_VLOG_WARNING, "Cut part %d/%d: stream %d: %"PRId64" frame(s) left out: a cut part does "
                    "not carry this track yet\n", c->part + 1, c->nb_parts, i, c->t[i].left);
    if (c->has_from)
        dr_vlog(c->log, DR_VLOG_VERBOSE, "Cut part %d/%d: its key frame met (%s), %"PRId64" picture(s) given, %"PRId64
                " left out, %"PRId64" kept that cannot be decoded\n", c->part + 1, c->nb_parts, start_name(c->r.start),
                c->r.pictures, c->r.left_out, c->r.undecodable);
    return 0;
}

void ff_disctl_cut_result(const DRCut *c, DRCutResult *r)
{
    *r = c->r;
    if (r->first == INT64_MAX)
        r->first = 0;
}

void ff_disctl_cut_close(DRCut **c)
{
    if (*c)
        dr_freep(&(*c)->t);
    dr_freep(c);
}
