/*
 * The shared rip core, stage 2 for audio: a track's units in one segment get
 * their duration, sync-unit flag and final bytes.
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
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

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
    void               *priv;      /* rule state */
    DRAudioStats        st;
};

void ff_discrip_frame_unref(DRFrame *f)
{
    av_buffer_unref(&f->buf);
    memset(f, 0, sizeof(*f));
    f->time = AV_NOPTS_VALUE;
}

int ff_discrip_audio_open(DRAudio **audio, void *logctx, enum AVCodecID codec, int flags,
                          DRFrameCb cb, void *opaque)
{
    const DRCodec *c = ff_discrip_codec(codec);
    DRAudio *a;

    *audio = NULL;
    if (!c || !c->audio) {
        av_log(logctx, AV_LOG_ERROR, "Rip core: %s has no audio rules in the codec table: "
               "its track cannot be ripped\n", avcodec_get_name(codec));
        return AVERROR(ENOSYS);
    }
    if (!(a = av_mallocz(sizeof(*a))))
        return AVERROR(ENOMEM);
    a->log    = logctx;
    a->codec  = c;
    a->rules  = c->audio;
    a->flags  = flags;
    a->cb     = cb;
    a->opaque = opaque;
    if (a->rules->priv_size && !(a->priv = av_mallocz(a->rules->priv_size))) {
        av_free(a);
        return AVERROR(ENOMEM);
    }
    *audio    = a;
    return 0;
}

void ff_discrip_audio_close(DRAudio **audio)
{
    DRAudio *a = *audio;

    if (!a)
        return;
    for (int i = 0; i < a->nb_wait; i++)
        ff_discrip_frame_unref(&a->wait[i]);
    av_freep(&a->wait);
    if (a->priv && a->rules->close)
        a->rules->close(a->priv);
    av_freep(&a->priv);
    av_freep(audio);
}

void ff_discrip_audio_stats(const DRAudio *a, DRAudioStats *stats)
{
    *stats = a->st;
}

const DRAudioHeader *ff_discrip_audio_header(const DRAudio *a)
{
    return &a->hdr;
}

int ff_discrip_audio_flags(const DRAudio *a)
{
    return a->flags;
}

void *ff_discrip_audio_priv(DRAudio *a)
{
    return a->priv;
}

void *ff_discrip_audio_log(const DRAudio *a)
{
    return a->log;
}

void ff_discrip_audio_review(DRAudio *a, unsigned kind, const char *what)
{
    a->st.review++;
    if (kind < 32 && (a->reviewed & (1u << kind)))
        return;
    if (kind < 32)
        a->reviewed |= 1u << kind;
    av_log(a->log, AV_LOG_ERROR, "Rip core: %s track: %s [untested on real discs]: "
           "the job is marked failed, look at this track\n", a->codec->name, what);
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
            av_log(a->log, AV_LOG_ERROR, "Rip core: %s: no duration for the %d-byte unit at stream "
                   "offset %"PRId64"\n", a->codec->name, size, f->pos);
            ff_discrip_frame_unref(f);
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
    if (f->time != AV_NOPTS_VALUE && f->time > 0) {
        if (a->expect && f->time != a->expect) {
            a->st.continuity++;
            av_log(a->log, AV_LOG_DEBUG, "Rip core: %s: unit at %"PRId64" ticks, the previous one "
                   "ended at %"PRId64" (%s of %"PRId64" ticks)\n", a->codec->name, f->time, a->expect,
                   f->time < a->expect ? "overlap" : "gap", FFABS(f->time - a->expect));
        }
        a->expect = f->time;
    }
    if (a->expect)
        a->expect += f->dur;

    a->st.frames++;
    return a->cb(a->opaque, f);
}

int ff_discrip_audio_unit(void *opaque, const DRUnit *u)
{
    DRAudio *a = opaque;
    DRFrame f = { .time = u->time, .pos = u->pos, .samples = u->samples, .rate = u->rate };
    int ret;

    if (!(f.buf = av_buffer_alloc(u->size + AV_INPUT_BUFFER_PADDING_SIZE)))
        return AVERROR(ENOMEM);
    memcpy(f.buf->data, u->data, u->size);
    memset(f.buf->data + u->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    f.data = f.buf->data;
    f.size = u->size;
    a->st.units++;

    if (a->have_header)
        return unit_done(a, &f);

    /* The stream's values come from its first sync unit that gives them;
     * units before it wait and then get the same values. */
    if ((!a->rules->sync || a->rules->sync(f.data, f.size)) &&
        a->rules->header(a, &f, &a->hdr) >= 0) {
        if (a->hdr.rate <= 0 || a->hdr.samples <= 0) {
            av_log(a->log, AV_LOG_ERROR, "Rip core: %s: unusable stream values (rate %d, %d samples)\n",
                   a->codec->name, a->hdr.rate, a->hdr.samples);
            ff_discrip_frame_unref(&f);
            return AVERROR_INVALIDDATA;
        }
        a->have_header = 1;
        a->st.header   = a->hdr;
        av_log(a->log, AV_LOG_DEBUG, "Rip core: %s: %d Hz, %d samples per unit (from the unit at stream "
               "offset %"PRId64"; %d units before it)\n", a->codec->name, a->hdr.rate, a->hdr.samples,
               f.pos, a->nb_wait);
        for (int i = 0; i < a->nb_wait; i++) {
            ret = unit_done(a, &a->wait[i]);
            if (ret < 0) {
                for (i++; i < a->nb_wait; i++)
                    ff_discrip_frame_unref(&a->wait[i]);
                a->nb_wait = 0;
                ff_discrip_frame_unref(&f);
                return ret;
            }
        }
        a->nb_wait = 0;
        return unit_done(a, &f);
    }
    if (a->nb_wait == a->wait_cap) {
        DRFrame *w = av_realloc_array(a->wait, a->wait_cap ? 2 * a->wait_cap : 8, sizeof(*w));
        if (!w) {
            ff_discrip_frame_unref(&f);
            return AVERROR(ENOMEM);
        }
        a->wait     = w;
        a->wait_cap = a->wait_cap ? 2 * a->wait_cap : 8;
    }
    a->wait[a->nb_wait++] = f;
    return 0;
}

int ff_discrip_audio_marker(DRAudio *a, int64_t time)
{
    DRFrame f = { .time = time, .flags = DR_F_KEY | DR_F_MARKER, .pos = -1 };

    a->st.markers++;
    a->st.frames++;
    return a->cb(a->opaque, &f);
}

int ff_discrip_audio_flush(DRAudio *a)
{
    if (!a->have_header && a->nb_wait) {
        av_log(a->log, AV_LOG_ERROR, "Rip core: %s: none of the segment's %d units gives the stream's "
               "values (rate, samples per unit): the track cannot be timed\n", a->codec->name, a->nb_wait);
        return AVERROR_INVALIDDATA;
    }
    av_log(a->log, AV_LOG_DEBUG, "Rip core: %s: %"PRId64" units, %"PRId64" frames, %"PRId64" markers, %"PRId64
           " bytes cut, %"PRId64" continuity breaks\n", a->codec->name, a->st.units, a->st.frames,
           a->st.markers, a->st.cut_bytes, a->st.continuity);
    return 0;
}
