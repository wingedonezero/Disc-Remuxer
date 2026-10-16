/*
 * The stream pass of a title (the disc layer), the same for every format:
 * the title's tracks (DiscTrack, from its format part) checked against the
 * stream data of its largest segment, read the way the title reader reads
 * (the format part's block reader through the clip demuxer, each payload to
 * the track of its id or PID and keys).
 *
 * - Duplicates: an audio or subtitle track with the same codec, the same
 *   data (id, or PID and keys) and the same kind (taken out of another
 *   track or not, forced subtitles or not) as one before it is left out.
 * - Details (when asked for): each video and audio track's payloads go
 *   through its codec's frame rule and FFmpeg's decoder (src/analyze) until
 *   one frame is decoded, within the first 64 MiB: picture size, aspect, frame rate,
 *   profile and level, the MPEG-1/2 sequence header's bit rate; audio
 *   channels, layout, sample rate, bits and bit rate. VC-1 opens its
 *   decoder with its sequence header and entry point; a 3-D second view
 *   gives its stereo profile and level (its subset SPS) and shows the base
 *   view's picture; a Dolby Vision enhancement layer gives the type (FEL /
 *   MEL from its metadata, the level by the base layer's picture rate). A
 *   caption track carried in the video has the video's bit rate.
 * - Data: reading goes on past the details while a video or audio track has
 *   had no packet (to the segment's end: a stream that never shows up is
 *   known to have none only there), and, when the format asks for it, a
 *   subtitle track too. A title whose video has no data cannot be used; an
 *   audio track without data is left out; a subtitle track without data
 *   keeps its (empty) track (a subtitle stream is described without data).
 * Results are kept per segment and track data: titles that share a segment
 * read it once.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "base/mem.h"
#include "base/err.h"
#include "base/macros.h"
#include "base/vlog.h"
#include "base/mem.h"

#include "base/err.h"
#include "disc/model.h"
#include "disc/probe.h"

#include "disc/clip_demux.h"
#include "timeline/disctl.h"


#define DETAILS_LIMIT (64LL << 20)
#define DISC_BLOCK    2048
#define TS_UNIT       6144

enum { P_TRACK, P_MVC, P_LAYER };       /* a track's own data, a 3-D second view, a Dolby Vision layer */

/* One probe: a track's data (or its Dolby Vision enhancement layer); the
 * details themselves are the analysis module's (disc/probe.h). */
typedef struct Probe {
    DiscTrack            *k;
    int                   kind;         /* P_* */
    int                   ps_id;        /* program stream: the id */
    DiscTsFeed            feed;         /* transport stream: PID and keys */
    DRCodecID        codec;
    const DRCodec        *tc;           /* its codec table entry, or NULL */
    int                   seen;         /* a packet of it was read */
    int                   done;         /* its details are known (or cannot be) */
    int                   cached;       /* taken from the cache */
    DiscDetailsProbe     *dp;           /* the analysis module's probe, or NULL */
} Probe;

/* The details probe the analysis module registered (disc/probe.h). */
static const DiscProbeOps *probe_ops;

void ff_disc_set_probe(const DiscProbeOps *ops)
{
    probe_ops = ops;
}


/* A finished probe, kept per segment and data. */
typedef struct Cached {
    uint64_t       seg;
    int            kind, ps_id, details, seen;
    DiscTsFeed     feed;
    DiscStreamInfo ds;
} Cached;

struct DiscPassCache {
    Cached *c;
    int     n, cap;
};

void ff_disc_pass_cache_free(DiscPassCache **pc)
{
    if (*pc)
        dr_freep(&(*pc)->c);
    dr_freep(pc);
}

static int same_feed(const DiscTsFeed *a, const DiscTsFeed *b)
{
    return a->pid == b->pid && a->nb_keys == b->nb_keys && a->key[0] == b->key[0] && a->key[1] == b->key[1];
}

static Cached *cache_find(DiscPassCache *c, uint64_t seg, const Probe *p, int details)
{
    for (int i = 0; seg && c && i < c->n; i++)
        if (c->c[i].seg == seg && c->c[i].kind == p->kind && c->c[i].ps_id == p->ps_id &&
            same_feed(&c->c[i].feed, &p->feed) && (c->c[i].details || !details))
            return &c->c[i];
    return NULL;
}

static int cache_add(DiscPassCache *c, uint64_t seg, const Probe *p, int details)
{
    if (c->n == c->cap) {
        int cap = c->cap ? 2 * c->cap : 64;
        Cached *n = dr_realloc_array(c->c, cap, sizeof(*n));
        if (!n)
            return DR_ERR(ENOMEM);
        c->c   = n;
        c->cap = cap;
    }
    c->c[c->n++] = (Cached){ seg, p->kind, p->ps_id, details, p->seen, p->feed, p->k->info };
    return 0;
}

/* What of a cached result belongs to the probe's track. */
static void cache_take(const Cached *k, Probe *p, int details)
{
    DiscStreamInfo *ds = &p->k->info;

    p->seen = k->seen;
    if (!details)
        return;
    if (p->kind == P_LAYER) {
        memcpy(ds->dolby_vision, k->ds.dolby_vision, sizeof(ds->dolby_vision));
        return;
    }
    ds->profile = k->ds.profile;
    ds->level   = k->ds.level;
    if (p->kind == P_MVC)
        return;
    ds->aspect      = k->ds.aspect;
    ds->frame_rate  = k->ds.frame_rate;
    ds->channels    = k->ds.channels;
    ds->layout      = k->ds.layout;
    ds->sample_rate = k->ds.sample_rate;
    ds->bits        = k->ds.bits;
    ds->bit_rate    = k->ds.bit_rate;
    if (ds->type == DISC_MEDIA_VIDEO) {
        ds->width    = k->ds.width;
        ds->height   = k->ds.height;
        ds->max_cll  = k->ds.max_cll;
        ds->max_fall = k->ds.max_fall;
        memcpy(ds->hdr, k->ds.hdr, sizeof(ds->hdr));
        memcpy(ds->colour, k->ds.colour, sizeof(ds->colour));
        ds->full_range = k->ds.full_range;
        memcpy(ds->mastering, k->ds.mastering, sizeof(ds->mastering));
    }
}

/* The 3-D second view: its subset SPS (nal_unit_type 15) gives the stereo
 * profile and level (its first bytes: profile_idc, constraint flags,
 * level_idc). */
static void mvc_bytes(Probe *p, const uint8_t *data, int size)
{
    for (int i = 0; i + 6 < size; i++)
        if (!data[i] && !data[i + 1] && data[i + 2] == 1 && (data[i + 3] & 0x1f) == 15) {
            p->k->info.profile = data[i + 4];
            p->k->info.level   = data[i + 6];
            p->done = 1;
            return;
        }
}

/* Dolby Vision's level by the base layer's picture rate (width x height x
 * frames per second), the levels of the profile 7 / 8 definition. */
static int dovi_level(const DiscStreamInfo *ds)
{
    static const int64_t max_rate[] = { 22118400, 27648000, 49766400, 62208000, 124416000, 199065600,
                                        248832000, 398131200, 497664000 };
    int64_t fps = ds->frame_rate.den ? (ds->frame_rate.num + ds->frame_rate.den / 2) / ds->frame_rate.den : 0;
    int64_t rate = (int64_t)ds->width * ds->height * fps;

    for (int i = 0; i < (int)DR_ARRAY_ELEMS(max_rate); i++)
        if (rate <= max_rate[i])
            return i + 1;
    return 0;
}

/* Whether a track's stream must have data to be described (the codec
 * table; a codec without an entry: video and audio do, subtitles not). The
 * title's video is required apart (its title cannot be used without). */
static int needs_data(const Probe *p)
{
    return p->tc ? !p->tc->info_without_data : p->k->kind != DR_KIND_SUBTITLE;
}

/* ---- the tracks ---- */

/* Track i left out: the tracks after it move up, links to them follow
 * (a link to it is cut). */
static void track_remove(DiscTrack *tracks, int *n, int i)
{
    memmove(tracks + i, tracks + i + 1, (*n - i - 1) * sizeof(*tracks));
    (*n)--;
    for (int j = 0; j < *n; j++) {
        DiscStreamInfo *k = &tracks[j].info;
        k->from_track  = k->from_track  == i ? -1 : k->from_track  > i ? k->from_track  - 1 : k->from_track;
        k->inner_track = k->inner_track == i ? -1 : k->inner_track > i ? k->inner_track - 1 : k->inner_track;
    }
}

static int same_data(const DiscTrack *a, const DiscTrack *b, int ts)
{
    return ts ? same_feed(&a->feed, &b->feed) : a->id == b->id;
}

/* The duplicates: an audio or subtitle track like one before it (each
 * judged as described, before any is taken out; dup: room for n). */
static void duplicates(DrDisc *d, const char *fmt, DiscTrack *tracks, int *n, int ts, int *dup)
{
    int nb = 0;

    for (int i = 1; i < *n; i++) {
        const DiscTrack *k = &tracks[i];
        if ((k->kind != DR_KIND_AUDIO && k->kind != DR_KIND_SUBTITLE) || (k->flags & DISC_TRACK_SIDE))
            continue;
        for (int j = 1; j < i; j++) {
            const DiscTrack *o = &tracks[j];
            if (o->kind == k->kind && !(o->flags & DISC_TRACK_SIDE) && o->info.codec == k->info.codec &&
                same_data(o, k, ts) && o->info.forced == k->info.forced &&
                (o->info.from_track >= 0) == (k->info.from_track >= 0)) {
                dr_vlog(d->log, DR_VLOG_WARNING, "%s: %s stream #%d is identical to stream #%d and was skipped\n", fmt,
                       k->kind == DR_KIND_AUDIO ? "Audio" : "Subtitle", k->number, o->number);
                dup[nb++] = i;
                break;
            }
        }
    }
    while (nb--)
        track_remove(tracks, n, dup[nb]);
}

/* ---- reading ---- */

typedef struct Reader {
    const DiscPassSource *src;
    uint32_t              block;        /* the next block */
    int                   bs;
    int                   err;          /* a block could not be read */
} Reader;

static int pass_read(void *opaque, uint8_t *buf, int size)
{
    Reader *r = opaque;
    int ret;

    if (size < r->bs)
        return DR_ERR(EINVAL);
    for (;;) {
        ret = r->src->read(r->src->opaque, r->block, buf);
        if (ret == DR_ERR_EOF)
            return DR_ERR_EOF;
        if (ret < 0) {
            r->err = ret;
            return ret;
        }
        r->block++;
        if (ret)
            return r->bs;
    }
}

static int probe_wants(const Probe *p, int ts, int id)
{
    uint16_t key = (uint32_t)id >> 16;

    if (!ts)
        return p->ps_id == id;
    if (p->feed.pid != (id & 0xffff))
        return 0;
    if (!p->feed.nb_keys || !p->feed.key[0])
        return 1;
    for (int j = 0; j < p->feed.nb_keys; j++)
        if (p->feed.key[j] == key)
            return 1;
    return 0;
}

int ff_disc_stream_pass(DrDisc *d, const char *title, const DiscPassSource *src, DiscTrack *tracks,
                        int *nb_tracks, int details)
{
    const char *fmt = d->fmt->log_name;
    Probe *p;
    int np = 0, ret = 0, open = 0, unseen = 0, *pids = NULL, nb_pids = 0, *drop = NULL, nb_drop = 0;
    Reader r = { src, 0, src->ts ? TS_UNIT : DISC_BLOCK, 0 };
    DrClipDemux *dm = NULL;

    if (!(drop = dr_calloc(*nb_tracks + 1, sizeof(*drop)))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    duplicates(d, fmt, tracks, nb_tracks, src->ts, drop);
    if (!(p = dr_calloc(2 * *nb_tracks + 1, sizeof(*p))) ||
        (src->ts && !(pids = dr_calloc(2 * *nb_tracks + 1, sizeof(*pids)))) ||
        (src->key && !d->pass_cache && !(d->pass_cache = dr_mallocz(sizeof(*d->pass_cache))))) {
        ret = DR_ERR(ENOMEM);
        goto end;
    }
    /* the probes: every track with data of its own (not a caption track in
     * the video, not a subtitle track from a clip of its own), a Dolby
     * Vision layer */
    for (int i = 0; i < *nb_tracks; i++) {
        DiscTrack *k = &tracks[i];
        if (k->kind == DR_KIND_CAPTION || (k->flags & DISC_TRACK_SIDE))
            continue;
        p[np++] = (Probe){ .k = k, .kind = (k->flags & DISC_TRACK_SECOND_VIEW) ? P_MVC : P_TRACK, .ps_id = k->id,
                           .feed = k->feed, .codec = k->info.codec, .tc = ff_disctl_codec(k->info.codec) };
        if (k->flags & DISC_TRACK_LAYER2)
            p[np++] = (Probe){ .k = k, .kind = P_LAYER, .ps_id = -1, .feed = k->layer_feed,
                               .codec = DR_CODEC_HEVC, .tc = ff_disctl_codec(DR_CODEC_HEVC) };
    }
    /* what titles before found already; a codec FFmpeg cannot open stays
     * without details */
    for (int i = 0; i < np; i++) {
        const Cached *c = cache_find(d->pass_cache, src->key, &p[i], details);
        int want = details && (p[i].k->kind == DR_KIND_VIDEO || p[i].k->kind == DR_KIND_AUDIO);
        if (c) {
            cache_take(c, &p[i], details);
            p[i].done = p[i].cached = 1;
            continue;
        }
        /* LPCM with another header than DVD-Video's 3 bytes (HD DVD's 5):
         * FFmpeg's decoder reads DVD-Video's */
        if (want && p[i].codec == DR_CODEC_PCM_DVD && src->lpcm_header != 3) {
            dr_vlog(d->log, DR_VLOG_VERBOSE, "%s: title %s: LPCM with a %d-byte header: its details are not read "
                   "[untested on real discs]\n", fmt, title, src->lpcm_header);
            want = 0;
        }
        p[i].done = !want;
        if (want && p[i].kind != P_MVC) {
            ret = probe_ops ? probe_ops->open(&p[i].dp, p[i].codec, p[i].tc, p[i].kind == P_LAYER, &p[i].k->info)
                            : DR_ERR_UNSUPPORTED;
            if (ret < 0) {
                if (ret == DR_ERR(ENOMEM))
                    goto end;
                dr_vlog(d->log, DR_VLOG_VERBOSE, "%s: title %s: %s: no decoder: its details are not known\n", fmt, title,
                       ff_disctl_codec_id_name(p[i].codec));
                p[i].done = 1;
                ret = 0;
            }
        }
        open   += !p[i].done;
        unseen += p[i].kind == P_TRACK && !p[i].seen;
    }
    if (open || unseen) {
        int details_on = 1;

        if (src->ts) {
            for (int i = 0; i < np; i++) {
                int have = 0;
                for (int j = 0; j < nb_pids && !have; j++)
                    have = pids[j] == p[i].feed.pid;
                if (!have)
                    pids[nb_pids++] = p[i].feed.pid;
            }
            ret = (dr_clip_ts_open(&dm, (void *)d->log, pids, nb_pids, 0, 0, pass_read, &r));
        } else {
            ret = (dr_clip_ps_open(&dm, (void *)d->log, src->lpcm_header, pass_read, &r));
        }
        if (ret < 0)
            goto end;
        for (;;) {
            int more = 0;
            DrClipPacket pkt;

            /* read on: details still open (within the limit); a track
             * that needs data to be described (or, when asked, any
             * track) without a packet yet */
            for (int i = 0; i < np && !more; i++)
                more = p[i].kind == P_TRACK && !p[i].seen && !p[i].cached && (needs_data(&p[i]) || src->read_all);
            if (details_on && (int64_t)r.block * r.bs >= DETAILS_LIMIT) {
                for (int i = 0; i < np; i++) {
                    if (p[i].dp && (ret = probe_ops->flush(p[i].dp)) < 0)
                        goto end;
                    if (p[i].dp)
                        p[i].done = probe_ops->done(p[i].dp);
                }
                details_on = 0;
            }
            if (!more && !(details_on && open))
                break;
            if ((ret = (dr_clip_read(dm, &pkt))) < 0)
                break;
            for (int i = 0; i < np; i++) {
                if (!probe_wants(&p[i], src->ts, pkt.id))
                    continue;
                p[i].seen = 1;
                if (!details_on || p[i].done)
                    continue;
                if (p[i].kind == P_MVC) {
                    mvc_bytes(&p[i], pkt.data, pkt.size);
                } else if (p[i].dp) {
                    if ((ret = probe_ops->feed(p[i].dp, pkt.data, pkt.size)) < 0)
                        goto end;
                    p[i].done = probe_ops->done(p[i].dp);
                }
                open -= p[i].done;
            }
        }
        if (r.err) {
            dr_vlog(d->log, DR_VLOG_ERROR, "%s: title %s: block %"PRIu32" of segment %d cannot be read: its streams' data "
                   "cannot be found\n", fmt, title, r.block, src->index + 1);
            ret = r.err;
            goto end;
        }
        if (ret != DR_ERR_EOF && ret < 0)
            goto end;
        ret = 0;
        /* the end of a short segment: its last frames */
        for (int i = 0; i < np && details_on; i++) {
            if (p[i].dp && (ret = probe_ops->flush(p[i].dp)) < 0)
                goto end;
            if (p[i].dp)
                p[i].done = probe_ops->done(p[i].dp);
        }
    }
    for (int i = 0; i < np && details; i++)
        if (!p[i].done)
            dr_vlog(d->log, DR_VLOG_VERBOSE, "%s: title %s: %s%s: no frame decoded in the first %d MiB: its details are not "
                   "known\n", fmt, title, ff_disctl_codec_id_name(p[i].codec),
                   p[i].kind == P_LAYER ? " (Dolby Vision layer)" : "", (int)(DETAILS_LIMIT >> 20));
    /* the 3-D second view shows the base view's picture size and rate */
    for (int i = 0; i < *nb_tracks && details; i++) {
        DiscStreamInfo *k = &tracks[i].info;
        if ((tracks[i].flags & DISC_TRACK_SECOND_VIEW) && k->from_track >= 0) {
            const DiscStreamInfo *b = &tracks[k->from_track].info;
            k->width      = b->width;
            k->height     = b->height;
            k->aspect     = b->aspect;
            k->frame_rate = b->frame_rate;
        }
    }
    /* what was found, for titles that share the segment (taken whole) */
    for (int i = 0; i < np && src->key; i++)
        if (!p[i].cached && (!details || p[i].done) &&
            (ret = cache_add(d->pass_cache, src->key, &p[i], details)) < 0)
            goto end;
    /* the Dolby Vision type in full: profile 7 (dual layer, HEVC base
     * layer), level by the picture rate */
    for (int i = 0; i < np; i++)
        if (p[i].kind == P_LAYER && p[i].k->info.dolby_vision[0] == 'B') {
            char type[16];
            dr_strlcpy(type, p[i].k->info.dolby_vision, sizeof(type));
            snprintf(p[i].k->info.dolby_vision, sizeof(p[i].k->info.dolby_vision), "dvhe.07.%02d %s",
                     dovi_level(&p[i].k->info), type);
        }
    /* a video that says full colour range: disc video is limited range
     * (16-235); the bytes are kept as they are */
    for (int i = 0; i < np && details; i++)
        if (p[i].kind == P_TRACK && p[i].k->info.type == DISC_MEDIA_VIDEO && p[i].k->info.full_range)
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: title %s: video stream %d says full colour range (disc video is "
                   "limited range): kept as on the disc\n", fmt, title, p[i].k->stream);
    /* the data */
    if (np && p[0].k == &tracks[0] && !p[0].seen) {
        dr_vlog(d->log, DR_VLOG_ERROR, "%s: title %s: its segment %d has no video packet: the title cannot be used\n", fmt,
               title, src->index + 1);
        ret = DR_ERR_INVALID;
        goto end;
    }
    for (int i = 0; i < np; i++) {
        const DiscTrack *k = p[i].k;
        if (p[i].kind != P_TRACK || p[i].seen)
            continue;
        if (needs_data(&p[i])) {
            dr_vlog(d->log, DR_VLOG_WARNING, "%s: title %s: %s stream %d has no packet in segment %d: left out\n", fmt,
                   title, disc_media_name(k->info.type), k->stream, src->index + 1);
            drop[nb_drop++] = k - tracks;
        } else if (src->read_all) {
            /* said for every title (the rip's track list says it again for the titles it rips) */
            dr_vlog(d->log, DR_VLOG_INFO, "%s: title %s: %s stream %d has no packet in segment %d: its track is "
                   "empty\n", fmt, title, k->info.codec == DR_CODEC_DVD_SUBTITLE ? "sub-picture" : "subtitle",
                   k->stream, src->index + 1);
        }
    }
    /* a caption track in the video: the video's rate */
    for (int i = 1; i < *nb_tracks && details; i++)
        if (tracks[i].kind == DR_KIND_CAPTION)
            tracks[i].info.bit_rate = tracks[0].info.bit_rate;
    /* the probes point into the tracks: the tracks left out last, the
     * highest first */
    for (int i = nb_drop - 1; i >= 0; i--)
        track_remove(tracks, nb_tracks, drop[i]);
end:
    for (int i = 0; p && i < np; i++)
        if (p[i].dp)
            probe_ops->close(&p[i].dp);
    dr_clip_close(&dm);
    dr_free(pids);
    dr_free(drop);
    dr_free(p);
    return ret;
}
