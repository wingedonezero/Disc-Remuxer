/*
 * The title timeline, stage 2 for audio: a track's units in one segment get
 * their duration, sync-unit flag and final bytes.
 */

#include <string.h>


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

struct DRAudio {
    void               *log;
    const DRCodec      *codec;
    const DRAudioRules *rules;
    int                 flags;
    DRFrameCb           cb;
    void               *opaque;

    int                 have_header;
    DRAudioHeader       hdr;
    DRFrame            *wait;      /* units before the first one that gave the header */
    int                 nb_wait, wait_cap;

    int64_t             expect;    /* end of the previous unit (continuity), 0 = none */
    unsigned            reviewed;  /* review kinds already reported */
    DRCoreFilter        core;      /* the rules' core filter for this track, NULL: none */
    void               *priv;      /* rule state */
    void               *state;     /* codec state owned by the caller */
    DRAudioStats        st;
};

void ff_disctl_frame_unref(DRFrame *f)
{
    dr_buf_unref(&f->buf);
    memset(f, 0, sizeof(*f));
    f->time = DR_NOPTS_VALUE;
}

int ff_disctl_audio_open(DRAudio **audio, void *logctx, DRCodecID codec, int flags,
                          DRFrameCb cb, void *opaque)
{
    const DRCodec *c = ff_disctl_codec(codec);
    DRAudio *a;

    *audio = NULL;
    if (!c || !c->audio) {
        dr_vlog(logctx, DR_VLOG_ERROR, "Title timeline: %s has no audio rules in the codec table: "
               "its track cannot be ripped\n", ff_disctl_codec_id_name(codec));
        return DR_ERR(ENOSYS);
    }
    if (!(a = dr_mallocz(sizeof(*a))))
        return DR_ERR(ENOMEM);
    a->log    = logctx;
    a->codec  = c;
    a->rules  = c->audio;
    a->flags  = flags;
    a->cb     = cb;
    a->opaque = opaque;
    if (a->rules->priv_size && !(a->priv = dr_mallocz(a->rules->priv_size))) {
        dr_free(a);
        return DR_ERR(ENOMEM);
    }
    if (a->rules->filter)
        a->core = a->rules->filter(flags);
    *audio    = a;
    return 0;
}

void ff_disctl_audio_close(DRAudio **audio)
{
    DRAudio *a = *audio;

    if (!a)
        return;
    for (int i = 0; i < a->nb_wait; i++)
        ff_disctl_frame_unref(&a->wait[i]);
    dr_freep(&a->wait);
    if (a->priv && a->rules->close)
        a->rules->close(a->priv);
    dr_freep(&a->priv);
    dr_freep(audio);
}

void ff_disctl_audio_stats(const DRAudio *a, DRAudioStats *stats)
{
    *stats = a->st;
}

const DRAudioHeader *ff_disctl_audio_header(const DRAudio *a)
{
    return &a->hdr;
}

int ff_disctl_audio_waiting(const DRAudio *a)
{
    return a->nb_wait;
}

int ff_disctl_audio_flags(const DRAudio *a)
{
    return a->flags;
}

void ff_disctl_audio_set_state(DRAudio *a, void *state)
{
    a->state = state;
}

void *ff_disctl_audio_state(const DRAudio *a)
{
    return a->state;
}

void *ff_disctl_audio_priv(DRAudio *a)
{
    return a->priv;
}

void *ff_disctl_audio_log(const DRAudio *a)
{
    return a->log;
}

void ff_disctl_audio_review(DRAudio *a, unsigned kind, const char *what)
{
    /* each kind once per track: said and counted */
    if (kind < 32 && (a->reviewed & (1u << kind)))
        return;
    if (kind < 32)
        a->reviewed |= 1u << kind;
    a->st.review++;
    dr_vlog(a->log, DR_VLOG_WARNING, "Title timeline: %s track: %s [untested on real discs]\n", a->codec->name, what);
}

/* A unit through the track's core filter: one unit in, its final bytes out
 * (the same buffer). */
static int filter(DRAudio *a, DRFrame *f)
{
    int off, size, ret = a->core(f->data, f->size, &off, &size);

    if (ret < 0)
        return DR_ERR_INVALID;
    if (size <= 0 || off < 0 || off + size > f->size) {
        dr_vlog(a->log, DR_VLOG_ERROR, "Title timeline: %s: the core filter gave no bytes of the unit at stream "
               "offset %"PRId64"\n", a->codec->name, f->pos);
        return DR_ERR_BUG;
    }
    f->data += off;
    f->size  = size;
    return 0;
}

/* Duration, final bytes, continuity; then hand the frame on. */
static int unit_done(DRAudio *a, DRFrame *f)
{
    int ret;

    if (a->rules->sync && a->rules->sync(f->data, f->size))
        f->flags |= DR_F_SYNC;
    if (!a->rules->key_on_sync || (f->flags & DR_F_SYNC))
        f->flags |= DR_F_KEY;
    if (a->rules->duration) {
        int size = f->size;
        if ((ret = a->rules->duration(a, f)) < 0) {
            dr_vlog(a->log, DR_VLOG_ERROR, "Title timeline: %s: no duration for the %d-byte unit at stream "
                   "offset %"PRId64"\n", a->codec->name, size, f->pos);
            ff_disctl_frame_unref(f);
            return ret;
        }
        if (a->core && (ret = filter(a, f)) < 0) {
            ff_disctl_frame_unref(f);
            return ret;
        }
        a->st.cut_bytes += size - f->size;
    } else {
        f->dur = (int64_t)((uint64_t)a->hdr.samples * DR_TICKS_PER_SECOND / (uint64_t)a->hdr.rate);
    }
    if (a->rules->inspect)
        a->rules->inspect(a, f);

    /* Continuity (log only, the times are not changed): a unit time that is
     * not the previous unit's end. */
    if (f->time != DR_NOPTS_VALUE && f->time > 0) {
        if (a->expect && f->time != a->expect) {
            a->st.continuity++;
            dr_vlog(a->log, DR_VLOG_DEBUG, "Title timeline: %s: unit at %"PRId64" ticks, the previous one "
                   "ended at %"PRId64" (%s of %"PRId64" ticks)\n", a->codec->name, f->time, a->expect,
                   f->time < a->expect ? "overlap" : "gap", DR_ABS(f->time - a->expect));
        }
        a->expect = f->time;
    }
    if (a->expect)
        a->expect += f->dur;

    a->st.frames++;
    return a->cb(a->opaque, f);
}

int ff_disctl_audio_unit(void *opaque, const DRUnit *u)
{
    DRAudio *a = opaque;
    DRFrame f = { .time = u->time, .pos = u->pos, .samples = u->samples, .rate = u->rate };
    int ret;

    if (!(f.buf = dr_buf_alloc(u->size + DR_BUF_PADDING)))
        return DR_ERR(ENOMEM);
    memcpy(dr_buf_data(f.buf), u->data, u->size);
    memset(dr_buf_data(f.buf) + u->size, 0, DR_BUF_PADDING);
    f.data = dr_buf_data(f.buf);
    f.size = u->size;
    a->st.units++;

    if (a->have_header)
        return unit_done(a, &f);

    /* The stream's values come from its first sync unit that gives them;
     * units before it wait and then get the same values. */
    if ((!a->rules->sync || a->rules->sync(f.data, f.size)) &&
        a->rules->header(a, &f, &a->hdr) >= 0) {
        if (a->hdr.rate <= 0 || a->hdr.samples <= 0) {
            dr_vlog(a->log, DR_VLOG_ERROR, "Title timeline: %s: unusable stream values (rate %d, %d samples)\n",
                   a->codec->name, a->hdr.rate, a->hdr.samples);
            ff_disctl_frame_unref(&f);
            return DR_ERR_INVALID;
        }
        a->have_header = 1;
        a->st.header   = a->hdr;
        dr_vlog(a->log, DR_VLOG_DEBUG, "Title timeline: %s: %d Hz, %d samples per unit (from the unit at stream "
               "offset %"PRId64"; %d units before it)\n", a->codec->name, a->hdr.rate, a->hdr.samples,
               f.pos, a->nb_wait);
        for (int i = 0; i < a->nb_wait; i++) {
            ret = unit_done(a, &a->wait[i]);
            if (ret < 0) {
                for (i++; i < a->nb_wait; i++)
                    ff_disctl_frame_unref(&a->wait[i]);
                a->nb_wait = 0;
                ff_disctl_frame_unref(&f);
                return ret;
            }
        }
        a->nb_wait = 0;
        return unit_done(a, &f);
    }
    if (a->nb_wait == a->wait_cap) {
        DRFrame *w = dr_realloc_array(a->wait, a->wait_cap ? 2 * a->wait_cap : 8, sizeof(*w));
        if (!w) {
            ff_disctl_frame_unref(&f);
            return DR_ERR(ENOMEM);
        }
        a->wait     = w;
        a->wait_cap = a->wait_cap ? 2 * a->wait_cap : 8;
    }
    a->wait[a->nb_wait++] = f;
    return 0;
}

int ff_disctl_audio_marker(DRAudio *a, int64_t time)
{
    DRFrame f = { .time = time, .flags = DR_F_KEY | DR_F_MARKER, .pos = -1 };

    a->st.markers++;
    a->st.frames++;
    return a->cb(a->opaque, &f);
}

int ff_disctl_audio_flush(DRAudio *a)
{
    if (!a->have_header && a->nb_wait) {
        dr_vlog(a->log, DR_VLOG_ERROR, "Title timeline: %s: none of the segment's %d units gives the stream's "
               "values (rate, samples per unit): the track cannot be timed\n", a->codec->name, a->nb_wait);
        return DR_ERR_INVALID;
    }
    dr_vlog(a->log, DR_VLOG_DEBUG, "Title timeline: %s: %"PRId64" units, %"PRId64" frames, %"PRId64" markers, %"PRId64
           " bytes cut, %"PRId64" continuity breaks\n", a->codec->name, a->st.units, a->st.frames,
           a->st.markers, a->st.cut_bytes, a->st.continuity);
    return 0;
}
