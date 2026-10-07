/*
 * Blu-ray: the tracks of a title, with the reference program's rules.
 *
 * The title's streams are numbered: the streams of the first clip of the
 * largest segment (its ProgramInfo order), then the title's text subtitles
 * (each from its own clip), then, when every item of that segment has a
 * dependent-view clip (application type 8, first stream coding type 0x20:
 * MVC), that clip's streams after its first. A stream whose PID the title
 * does not keep is not described.
 *
 * Each stream is described as one or two tracks by its coding type
 * (BD-ROM stream_coding_type):
 *   0x02 MPEG-2 video, 0xea VC-1, 0x1b AVC (+ the MVC second view of a 3-D
 *   title), 0x24 HEVC; 0x03 / 0x04 MPEG audio, 0x0f / 0x11 AAC, 0x80 LPCM,
 *   0x81 AC-3, 0x82 DTS; 0x83 TrueHD + its AC-3; 0x84 / 0xa1 E-AC-3 + its
 *   AC-3 (E-AC-3 alone when the probe found no AC-3 substream); 0x85 / 0x86 /
 *   0xa2 DTS-HD + its DTS core (DTS-HD alone without a core substream); 0x90
 *   PGS + its forced-only copy; 0x92 text subtitle + its forced-only copy;
 *   0x06 private data only with the kind the program map gives; anything
 *   else no track.
 * Languages: audio from the coding info's bytes 1..3, PGS 0..2, text
 * subtitles 1..3; else what the probe found.
 *
 * A subtitle of a 3-D title takes the offset sequence the playlist's 3-D
 * data gives its PG stream (if any).
 *
 * Tracks: the first stream with a video track gives the video (and the 3-D
 * second view), then every audio track of every stream, then every subtitle
 * track, then the text subtitles from their own clips; an audio or subtitle
 * track equal to an earlier one (kind, PID and what of the PID it takes) is
 * left out.
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

#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "bluray_internal.h"

/* track kinds (internal codes) */
enum {
    K_MPEG2 = 0x01, K_VC1 = 0x02, K_AVC = 0x03, K_MVC = 0x04, K_HEVC = 0x05,
    K_MPA = 0x40, K_AC3 = 0x41, K_DTS = 0x43, K_LPCM = 0x44, K_AAC = 0x45,
    K_DTSHD = 0x48, K_DTSHD_CORE = 0x49, K_TRUEHD = 0x4b, K_EAC3 = 0x4d, K_EAC3_CORE = 0x4e,
    K_PGS = 0xc1, K_PGS_FORCED = 0xc2, K_TEXT = 0xc3, K_TEXT_FORCED = 0xc4,
};

#define MAX_PARTS 3

typedef struct Part {
    int      code;
    uint16_t pid;
    uint32_t sel1, sel2;
    uint32_t flags;
    char     lang[4];
    int      offset_seq;     /* 3-D subtitle: the offset sequence of the playlist's 3-D data */
} Part;

typedef struct Desc {
    int  n;
    Part k[MAX_PARTS];
} Desc;

static int is_video(int code)    { return code >= 0x01 && code < 0x40; }
static int is_audio(int code)    { return code >= 0x40 && code < 0xc0; }
static int is_subtitle(int code) { return code >= 0xc0 && code < 0xca; }

static enum AVCodecID codec_of(int code)
{
    switch (code) {
    case K_MPEG2:      return AV_CODEC_ID_MPEG2VIDEO;
    case K_VC1:        return AV_CODEC_ID_VC1;
    case K_AVC:
    case K_MVC:        return AV_CODEC_ID_H264;
    case K_HEVC:       return AV_CODEC_ID_HEVC;
    case K_MPA:        return AV_CODEC_ID_MP2;
    case K_AC3:        return AV_CODEC_ID_AC3;
    case K_DTS:
    case K_DTSHD:
    case K_DTSHD_CORE: return AV_CODEC_ID_DTS;
    case K_LPCM:       return AV_CODEC_ID_PCM_BLURAY;
    case K_AAC:        return AV_CODEC_ID_AAC;
    case K_TRUEHD:     return AV_CODEC_ID_TRUEHD;
    case K_EAC3:
    case K_EAC3_CORE:  return AV_CODEC_ID_EAC3;
    case K_PGS:
    case K_PGS_FORCED: return AV_CODEC_ID_HDMV_PGS_SUBTITLE;
    case K_TEXT:
    case K_TEXT_FORCED:return AV_CODEC_ID_HDMV_TEXT_SUBTITLE;
    }
    return AV_CODEC_ID_NONE;
}

/* A split stream: the main part and its core (E-AC-3 / DTS-HD: one part
 * when the probe found no core substream). */
static void split(Desc *d, const BlurayClipStream *e, int main, int core, int alone, int secondary)
{
    if (!(e->probe & BLURAY_PROBE_NO_CORE)) {
        d->n = 2;
        d->k[0] = (Part){ .code = main, .sel1 = 0x71, .sel2 = 0x72, .flags = BLURAY_TRACK_HAS_CORE };
        d->k[1] = (Part){ .code = core, .sel1 = 0x71, .flags = BLURAY_TRACK_CORE | BLURAY_TRACK_DERIVED };
        if (secondary) {
            d->k[0].flags |= BLURAY_TRACK_SECONDARY;
            d->k[1].flags |= BLURAY_TRACK_SECONDARY;
        }
    } else {
        d->n = 1;
        d->k[0] = (Part){ .code = alone, .sel1 = 0x72, .flags = secondary ? BLURAY_TRACK_SECONDARY : 0 };
    }
}

/* One clip stream described (its parts, PIDs and languages). */
static void describe(const BlurayClipStream *e, Desc *d)
{
    memset(d, 0, sizeof(*d));
    switch (e->coding_type) {
    case 0x02: d->n = 1; d->k[0].code = K_MPEG2; break;
    case 0xea: d->n = 1; d->k[0].code = K_VC1;   break;
    case 0x1b:
        d->n = 1; d->k[0].code = K_AVC;
        if (e->probe & BLURAY_PROBE_AVC_ID)
            d->k[0].sel1 = e->probe & 0xf;
        break;
    case 0x24:
        d->n = 1; d->k[0].code = K_HEVC;
        if (e->probe & BLURAY_PROBE_HEVC_EL)
            d->k[0].sel1 = 0x1015;
        break;
    case 0x03: case 0x04: d->n = 1; d->k[0].code = K_MPA; break;
    case 0x0f: case 0x11: d->n = 1; d->k[0].code = K_AAC; d->k[0].sel2 = e->coding_type; break;
    case 0x80: d->n = 1; d->k[0].code = K_LPCM; break;
    case 0x81: d->n = 1; d->k[0].code = K_AC3; d->k[0].sel1 = 0x71; break;
    case 0x82: d->n = 1; d->k[0].code = K_DTS; d->k[0].sel1 = 0x71; break;
    case 0x83:
        d->n = 2;
        d->k[0] = (Part){ .code = K_TRUEHD, .sel1 = 0x72, .flags = BLURAY_TRACK_HAS_CORE };
        d->k[1] = (Part){ .code = K_AC3, .sel1 = 0x76, .flags = BLURAY_TRACK_CORE | BLURAY_TRACK_DERIVED };
        break;
    case 0x84: case 0xa1: split(d, e, K_EAC3_CORE, K_AC3, K_EAC3, e->coding_type == 0xa1); break;
    case 0x85: case 0x86: case 0xa2: split(d, e, K_DTSHD_CORE, K_DTS, K_DTSHD, e->coding_type == 0xa2); break;
    case 0x90:
        d->n = 2;
        d->k[0].code = K_PGS;
        d->k[1] = (Part){ .code = K_PGS_FORCED, .flags = BLURAY_TRACK_FORCED | BLURAY_TRACK_DERIVED };
        break;
    case 0x92:
        d->n = 2;
        d->k[0].code = K_TEXT;
        d->k[1] = (Part){ .code = K_TEXT_FORCED, .flags = BLURAY_TRACK_FORCED | BLURAY_TRACK_DERIVED };
        break;
    case 0x06:
        if (!(e->probe & BLURAY_PROBE_SUBTYPE) || e->subtype == 0xc6 || e->subtype == 0xc7 || e->subtype == 0xc9)
            return;
        d->n = 1; d->k[0].code = e->subtype;
        break;
    default:
        return;
    }
    for (int i = 0; i < d->n; i++) {
        Part *k = &d->k[i];
        const uint8_t *l = NULL;
        k->pid = e->pid;
        if (is_audio(k->code) || e->coding_type == 0x92)
            l = e->info + 1;
        else if (e->coding_type == 0x90)
            l = e->info;
        if (l && l[0])
            memcpy(k->lang, l, 3);
        if (!k->lang[0] && (e->probe & BLURAY_PROBE_LANG))
            memcpy(k->lang, e->probe_lang, 3);
    }
}

/* ---- the title's stream numbering ---- */

/* A dependent-view (MVC) clip: application type 8, first stream 0x20. */
static int mvc_clip(const BlurayClip *c)
{
    return c && c->app_type == 8 && c->program.nb_streams && c->program.streams[0].coding_type == 0x20;
}

static int segment_3d(const BluraySegment *s)
{
    for (int i = 0; i < s->nb_items; i++)
        if (!mvc_clip(s->items[i].dep))
            return 0;
    return s->nb_items > 0;
}

static int title_3d(const BlurayTitle *t)
{
    for (int i = 0; i < t->nb_segs; i++)
        if (!segment_3d(&t->segs[i]))
            return 0;
    return 1;
}

static int keeps_pid(const BlurayTitle *t, uint16_t pid)
{
    int lo = 0, hi = t->nb_pids;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (t->pids[mid] < pid)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < t->nb_pids && t->pids[lo] == pid;
}

static int stream_count(const BlurayTitle *t, const BluraySegment *s)
{
    const BlurayClip *c = s->items[0].clip, *dep = s->items[0].dep;

    return c->program.nb_streams + t->nb_side + (segment_3d(s) ? dep->program.nb_streams - 1 : 0);
}

/* The STC sequence's index over all ATC sequences of clip c, 0xff when it
 * is not one of them. */
static int stc_index(const BlurayClip *c, const BlurayStc *stc)
{
    int n = 0;

    for (int a = 0; a < c->nb_atc; a++)
        for (int k = 0; k < c->atc[a].nb_stc; k++, n++)
            if (&c->atc[a].stc[k] == stc)
                return n;
    return 0xff;
}

/* A subtitle of a 3-D title: the offset sequence the playlist's 3-D data
 * (STN_table_SS of the segment's first PlayItem) gives the PG stream with
 * this PID (0xff: none). */
static void offset_sequence(void *log, const BluraySegment *s, uint16_t pid, Desc *d)
{
    const BlurayPlayItem *it = s->items[0].item;
    int nss;

    if (!it || !it->ss.present || !(nss = it->nb_pg))
        return;
    for (int i = 0; i < nss && i < it->nb_pg; i++) {
        const BlurayStnStream *pg = &it->pg[i];
        int v;
        if (pg->entry_type != 1 || pg->entry_len <= 1) {
            av_log(log, AV_LOG_DEBUG, "Blu-ray: PG stream %d of a 3-D PlayItem has no PID entry\n", i);
            continue;
        }
        if (AV_RB16(pg->entry) != pid)
            continue;
        if ((v = it->ss.pg[i][0]) == 0xff)
            return;
        for (int k = 0; k < d->n; k++)
            if (d->k[k].code >= K_PGS && d->k[k].code <= K_TEXT_FORCED) {
                d->k[k].flags     |= BLURAY_TRACK_3D_PLANE;
                d->k[k].offset_seq = v;
            }
        return;
    }
}

/* Stream idx of the segment described: 1, 0 no such stream (side streams
 * whose entry is not a sub-path stream). side_* set for a text subtitle
 * from its own clip. */
static int stream_desc(void *log, const BlurayTitle *t, const BluraySegment *s, int idx, int three_d,
                       Desc *d, uint32_t *side_clip, int *side_stc)
{
    const BlurayClip *c = s->items[0].clip;
    int nc = c->program.nb_streams;

    memset(d, 0, sizeof(*d));
    *side_clip = 0;
    *side_stc  = -1;
    if (idx >= nc + t->nb_side) {                             /* the dependent view's streams */
        const BlurayClip *dep = s->items[0].dep;
        int k = idx - nc - t->nb_side + 1;
        if (!dep || k >= dep->program.nb_streams)
            return 0;
        if (keeps_pid(t, dep->program.streams[k].pid)) {
            describe(&dep->program.streams[k], d);
            if (three_d)
                offset_sequence(log, s, dep->program.streams[k].pid, d);
        }
    } else if (idx >= nc) {                                   /* a text subtitle from its own clip */
        const BluraySide *r = &t->side[idx - nc];
        uint16_t pid;
        if (r->entry->entry_type != 2 || r->entry->entry_len < 4)
            return 0;
        pid = AV_RB16(r->entry->entry + 2);
        for (int i = 0; i < r->clip->program.nb_streams; i++)
            if (r->clip->program.streams[i].pid == pid) {
                describe(&r->clip->program.streams[i], d);
                if (three_d)
                    offset_sequence(log, s, pid, d);
                break;
            }
        for (int i = 0; i < d->n; i++)
            d->k[i].flags |= BLURAY_TRACK_SIDE;
        *side_clip = r->clip->number;
        *side_stc  = stc_index(r->clip, r->stc);
        if (r->clip->nb_atc > 1)
            av_log(log, AV_LOG_DEBUG, "Blu-ray: clip %05u of a text subtitle has %d ATC sequences\n",
                   r->clip->number, r->clip->nb_atc);
    } else {
        const BlurayClipStream *e = &c->program.streams[idx];
        if (keeps_pid(t, e->pid)) {
            describe(e, d);
            if (three_d)
                offset_sequence(log, s, e->pid, d);
        }
    }
    /* the 3-D second view of the AVC video */
    if (d->n == 1 && three_d && d->k[0].code == K_AVC && s->items[0].dep) {
        d->k[0].flags |= BLURAY_TRACK_3D_BASE;
        d->k[1] = (Part){ .code = K_MVC, .pid = d->k[0].pid, .sel1 = s->items[0].dep->program.streams[0].pid,
                          .flags = BLURAY_TRACK_DERIVED };
        d->n = 2;
    }
    return 1;
}

/* ---- tracks ---- */

static int add_track(BlurayTitle *t, const Part *k, int stream, uint32_t side_clip, int side_stc,
                     int from_track)
{
    BlurayTrack *tk, *v = av_realloc_array(t->tracks, t->nb_tracks + 1, sizeof(*t->tracks));

    if (!v)
        return AVERROR(ENOMEM);
    t->tracks = v;
    tk = &t->tracks[t->nb_tracks];
    memset(tk, 0, sizeof(*tk));
    tk->code   = k->code;
    tk->type   = is_video(k->code) ? AVMEDIA_TYPE_VIDEO : is_audio(k->code) ? AVMEDIA_TYPE_AUDIO :
                 AVMEDIA_TYPE_SUBTITLE;
    tk->codec  = codec_of(k->code);
    tk->pid    = k->pid;
    tk->sel1   = k->sel1;
    tk->sel2   = k->sel2;
    tk->flags  = k->flags;
    tk->stream = stream;
    memcpy(tk->lang, k->lang, sizeof(tk->lang));
    tk->offset_seq  = (k->flags & BLURAY_TRACK_3D_PLANE) ? k->offset_seq : -1;
    tk->side_clip   = side_clip;
    tk->side_stc    = side_stc;
    tk->inner_track = -1;
    tk->from_track  = from_track;
    if (from_track >= 0)
        t->tracks[from_track].inner_track = t->nb_tracks;
    return t->nb_tracks++;
}

/* The parts of the descriptor in [lo, hi) of kind code range, not side
 * (side < 0: any, else side flag must equal it), added unless equal to an
 * earlier track of the same pass. */
static int add_parts(void *log, BlurayTitle *t, const Desc *d, int stream, int lo, int hi, int want_side,
                     uint32_t side_clip, int side_stc, int first_of_pass, const char *what)
{
    int ret, main_track = -1;

    for (int i = 0; i < d->n; i++) {
        const Part *k = &d->k[i];
        int side = !!(k->flags & BLURAY_TRACK_SIDE), dup = -1;
        if (k->code < lo || k->code >= hi || side != want_side)
            continue;
        if (what)
            for (int j = first_of_pass; j < t->nb_tracks && dup < 0; j++) {
                const BlurayTrack *o = &t->tracks[j];
                if (o->code == k->code && o->pid == k->pid && o->sel1 == k->sel1 && o->sel2 == k->sel2)
                    dup = o->stream;
            }
        if (dup >= 0) {
            av_log(log, AV_LOG_WARNING, "Blu-ray: %s stream #%d is identical to stream #%d and was skipped\n",
                   what, stream, dup);
            continue;
        }
        if ((ret = add_track(t, k, stream, side_clip, side_stc,
                             (k->flags & BLURAY_TRACK_DERIVED) ? main_track : -1)) < 0)
            return ret;
        if (!(k->flags & BLURAY_TRACK_DERIVED))
            main_track = ret;
    }
    return 0;
}

int ff_bluray_title_tracks(void *log, BlurayTitle *t)
{
    const BluraySegment *s;
    int64_t best = -1;
    int three_d, n, video = -1, first, ret;
    Desc d;
    uint32_t side_clip;
    int side_stc;

    av_freep(&t->tracks);
    t->nb_tracks = 0;
    if (!t->nb_segs)
        return AVERROR_INVALIDDATA;
    /* the largest segment (the first of equal ones) */
    for (int i = 0; i < t->nb_segs; i++) {
        int64_t size = 0;
        for (int k = 0; k < t->segs[i].nb_items; k++)
            size += t->segs[i].items[k].length;
        if (size > best) {
            best = size;
            t->track_seg = i;
        }
    }
    s       = &t->segs[t->track_seg];
    three_d = title_3d(t);
    n       = stream_count(t, s);

    /* the video: the first stream that describes one */
    for (int i = 0; i < n && video < 0; i++)
        if (stream_desc(log, t, s, i, three_d, &d, &side_clip, &side_stc) && d.n && is_video(d.k[0].code))
            video = i;
    if (video < 0) {
        av_log(log, AV_LOG_WARNING, "Blu-ray: title %s has no video stream\n", t->name);
        return AVERROR_INVALIDDATA;
    }
    stream_desc(log, t, s, video, three_d, &d, &side_clip, &side_stc);
    if ((ret = add_parts(log, t, &d, video, 0x01, 0x40, 0, 0, -1, 0, NULL)) < 0)
        return ret;

    /* audio, then subtitles, an identical one left out */
    first = t->nb_tracks;
    for (int i = 0; i < n; i++)
        if (stream_desc(log, t, s, i, three_d, &d, &side_clip, &side_stc) &&
            (ret = add_parts(log, t, &d, i, 0x40, 0xc0, 0, 0, -1, first, "Audio")) < 0)
            return ret;
    first = t->nb_tracks;
    for (int i = 0; i < n; i++)
        if (stream_desc(log, t, s, i, three_d, &d, &side_clip, &side_stc) &&
            (ret = add_parts(log, t, &d, i, 0xc0, 0xca, 0, 0, -1, first, "Subtitle")) < 0)
            return ret;

    /* text subtitles from their own clips */
    for (int i = 0; i < n; i++) {
        if (!stream_desc(log, t, s, i, three_d, &d, &side_clip, &side_stc))
            continue;
        for (int k = 0; k < d.n; k++)
            if ((d.k[k].flags & BLURAY_TRACK_SIDE) && is_subtitle(d.k[k].code) &&
                d.k[k].code != K_TEXT && d.k[k].code != K_TEXT_FORCED) {
                av_log(log, AV_LOG_DEBUG, "Blu-ray: title %s: stream #%d of a sub-path is no text subtitle: "
                       "left out\n", t->name, i);
                d.k[k].code = 0;
            }
        if ((ret = add_parts(log, t, &d, i, 0xc0, 0xca, 1, side_clip, side_stc, 0, NULL)) < 0)
            return ret;
    }
    av_log(log, AV_LOG_DEBUG, "Blu-ray: title %s: %d tracks from segment %d (%d streams%s)\n", t->name,
           t->nb_tracks, t->track_seg, n, three_d ? ", 3-D" : "");
    return 0;
}
