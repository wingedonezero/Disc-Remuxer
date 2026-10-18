/*
 * The title timeline, stage 3: a title's segments joined on one timeline.
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

#define SEED_UNITS   1000          /* audio frames searched for the first one with a time */

typedef struct Track {
    int      kind;
    DRFrame *wait;          /* frames of the current segment not placed yet */
    int      nb_wait, cap;
    int      seeded;        /* e is known */
    int64_t  e;             /* expected next time in the current segment */
    int64_t  last_gap;      /* the early-frame value last reported (audio) */
} Track;

struct DRJoin {
    void        *log;
    DRJoinConfig cfg;
    Track       *t;

    int          seg;       /* current segment, -1 before the first */
    int          placed;    /* the current segment has its start and offset */
    int64_t      start, offset;
    int64_t      prev_e0, prev_start, prev_offset;
    int64_t      batch_min; /* earliest video time of the segment's first batch so far */
    int64_t     *chap;      /* the chapters' start times (placed when the title is finished) */
    int          nb_chap;
    int64_t     *vtimes, *ktimes;   /* the video frames' times, the key frames' (for the chapters) */
    int          nb_v, cap_v, nb_k, cap_k;
    int64_t     *starts;    /* each placed segment's start (its first video time), DR_NOPTS_VALUE: not placed */
    int          nb_starts;
    DRJoinStats  st;
};

static int wait_add(Track *t, DRFrame *f)
{
    if (t->nb_wait == t->cap) {
        int cap = t->cap ? 2 * t->cap : 64;
        DRFrame *w = dr_realloc_array(t->wait, cap, sizeof(*w));
        if (!w)
            return DR_ERR(ENOMEM);
        t->wait = w;
        t->cap  = cap;
    }
    t->wait[t->nb_wait++] = *f;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int add_time(int64_t **a, int *n, int *cap, int64_t t)
{
    if (*n == *cap) {
        int c = *cap ? 2 * *cap : 1024;
        int64_t *b = dr_realloc_array(*a, c, sizeof(*b));
        if (!b)
            return DR_ERR(ENOMEM);
        *a   = b;
        *cap = c;
    }
    (*a)[(*n)++] = t;
    return 0;
}

static int cmp_time(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* HH:MM:SS.mmm.uuu.nnn of a time on the title timeline (to the nearest ns), into buf */
static const char *ns_text(char *buf, size_t size, int64_t t)
{
    int64_t ns = t < 0 ? -((-t * 25 + 13) / 27) : (t * 25 + 13) / 27, a = ns < 0 ? -ns : ns;
    snprintf(buf, size, "%s%02"PRId64":%02"PRId64":%02"PRId64".%03"PRId64".%03"PRId64".%03"PRId64, ns < 0 ? "-" : "",
             a / INT64_C(3600000000000), a / INT64_C(60000000000) % 60, a / 1000000000 % 60, a / 1000000 % 1000,
             a / 1000 % 1000, a % 1000);
    return buf;
}

/* the video frames from a up to (not with) b */
static int frames_between(const DRJoin *j, int64_t a, int64_t b)
{
    int n = 0;
    for (int i = 0; i < j->nb_v; i++)
        n += j->vtimes[i] >= a && j->vtimes[i] < b;
    return n;
}

/* Each mark's chapter, by the rule's stages (DRChapterRule): the mark on a
 * key frame; the nearest key frame before it within before_ms; the nearest
 * after it within after_ms; the mark. Said in the standard log as a block:
 * the rule, a line per chapter (kept on a key frame, snapped from -> to,
 * kept between key frames and why, or no chapter and why), the counts. */
static int place_chapters(DRJoin *j)
{
    DRChapterRule r = j->cfg.chapter_rule;
    int64_t before, after, prev = INT64_MIN, last;
    char a[40], b[40], rule[112];
    int on_keys = 0, moved = 0, between = 0, none = 0;  /* the marks, by what the rule did */

    if (!r.set) {
        r.before_ms = 400;
        r.after_ms  = 0;
    }
    before = (int64_t)r.before_ms * (DR_TICKS_PER_SECOND / 1000);
    after  = (int64_t)r.after_ms * (DR_TICKS_PER_SECOND / 1000);
    if (!j->cfg.nb_marks || !j->nb_v)
        return 0;
    qsort(j->vtimes, j->nb_v, sizeof(*j->vtimes), cmp_time);
    qsort(j->ktimes, j->nb_k, sizeof(*j->ktimes), cmp_time);
    last = j->vtimes[j->nb_v - 1];
    if (!(j->chap = dr_calloc(j->cfg.nb_marks, sizeof(*j->chap))))
        return DR_ERR(ENOMEM);
    if (r.exact)
        snprintf(rule, sizeof(rule), "snap off: at the disc's marks");
    else if (after)
        snprintf(rule, sizeof(rule), "snap on: to the nearest key frame up to %d ms back, else up to %d ms on",
                 r.before_ms, r.after_ms);
    else
        snprintf(rule, sizeof(rule), "snap on: to the nearest key frame up to %d ms back", r.before_ms);
    dr_vlog(j->log, DR_VLOG_INFO, "Chapters: %d key frame(s) in the title; %s\n", j->nb_k, rule);
    for (int i = 0; i < j->cfg.nb_marks; i++) {
        int64_t m = j->cfg.marks[i], mf = INT64_MIN, kb = INT64_MIN, ka = INT64_MAX, c;
        int stage, on_key = 0, clash = 0, at_key;
        char next[64] = "no key frame follows";
        if (m == INT64_MAX)                 /* a position never reached */
            continue;
        /* the mark's own frame: the video frame within 1 ms of it (a Blu-ray mark's
         * 45 kHz time cannot hold a 1001-based frame time exactly) */
        for (int k = 0; k < j->nb_v; k++)
            if (DR_ABS(j->vtimes[k] - m) <= DR_TICKS_PER_SECOND / 1000 &&
                (mf == INT64_MIN || DR_ABS(j->vtimes[k] - m) < DR_ABS(mf - m)))
                mf = j->vtimes[k];
        for (int k = 0; k < j->nb_k; k++) {
            int64_t t = j->ktimes[k];
            if (mf != INT64_MIN && t == mf)
                on_key = 1;
            else if (t < m)
                kb = t;
            else if (t > m && ka == INT64_MAX)
                ka = t;
        }
        if (r.exact)
            stage = 0, c = m;
        else if (on_key)
            stage = 1, c = mf;
        else if (before && kb != INT64_MIN && m - kb <= before)
            stage = 2, c = kb;
        else if (after && ka != INT64_MAX && ka - m <= after)
            stage = 3, c = ka;
        else
            stage = 4, c = mf != INT64_MIN ? mf : m;
        /* a mark past the video's last frame starts no chapter */
        if ((stage == 0 || stage == 4) && m > last) {
            dr_vlog(j->log, DR_VLOG_INFO, "  - No chapter for mark %d (%s) - after the video's last frame\n", i + 1,
                    ns_text(a, sizeof(a), m));
            none++;
            continue;
        }
        /* chapters in order: a chapter not after the one before it keeps its mark (or goes) */
        if (c <= prev) {
            if (m <= prev || (stage == 4 && mf != INT64_MIN && mf <= prev)) {
                dr_vlog(j->log, DR_VLOG_WARNING, "  - No chapter for mark %d (%s) - not after the chapter before it "
                        "once that one is placed\n", i + 1, ns_text(a, sizeof(a), m));
                none++;
                continue;
            }
            stage = 4;
            clash = 1;
            c = mf != INT64_MIN && mf > prev ? mf : m;
        }
        if (ka != INT64_MAX)
            snprintf(next, sizeof(next), "the next one %d frame(s) later", frames_between(j, m, ka));
        /* on the key frame at its mark (snap off: the mark itself, within 1 ms of it) */
        at_key = on_key && (c == mf || stage == 0);
        if (stage == 2 || stage == 3)
            dr_vlog(j->log, DR_VLOG_INFO, "  - Snapped 'Chapter %02d' (%s) -> %s (moved by %c%.3f ms, %d frame(s))\n",
                    j->nb_chap + 1, ns_text(a, sizeof(a), m), ns_text(b, sizeof(b), c), stage == 2 ? '-' : '+',
                    (double)DR_ABS(m - c) * 1000 / DR_TICKS_PER_SECOND,
                    stage == 2 ? frames_between(j, c, m) : frames_between(j, m, c));
        else if (at_key)
            dr_vlog(j->log, DR_VLOG_INFO, "  - Kept 'Chapter %02d' (%s) - on a key frame\n", j->nb_chap + 1,
                    ns_text(a, sizeof(a), c));
        else if (stage == 0)
            dr_vlog(j->log, DR_VLOG_INFO, "  - Kept 'Chapter %02d' (%s) - between key frames (%s)\n", j->nb_chap + 1,
                    ns_text(a, sizeof(a), c), next);
        else if (clash)
            dr_vlog(j->log, DR_VLOG_INFO, "  - Kept 'Chapter %02d' (%s) - between key frames; the key frame before it "
                    "holds the chapter before it (%s)\n", j->nb_chap + 1, ns_text(a, sizeof(a), c), next);
        else
            dr_vlog(j->log, DR_VLOG_INFO, "  - Kept 'Chapter %02d' (%s) - between key frames, none within %d ms "
                    "before%s (%s)\n", j->nb_chap + 1, ns_text(a, sizeof(a), c), r.before_ms,
                    after ? " or the time after it" : "", next);
        j->chap[j->nb_chap++] = c;
        prev = c;
        on_keys += at_key;
        moved   += stage == 2 || stage == 3;
        between += !at_key && stage != 2 && stage != 3;
    }
    j->st.chapters = j->nb_chap;
    {
        char no[48] = "";
        if (none)
            snprintf(no, sizeof(no), ", %d mark(s) without a chapter", none);
        if (r.exact)
            dr_vlog(j->log, DR_VLOG_INFO, "Chapters: %d at the disc's marks: %d on a key frame, %d between key "
                    "frames%s\n", j->nb_chap, on_keys, between, no);
        else
            dr_vlog(j->log, DR_VLOG_INFO, "Chapters: snap complete: %d moved, %d on a key frame, %d kept between key "
                    "frames%s\n", moved, on_keys, between, no);
    }
    return 0;
}

static int give(DRJoin *j, int track, DRFrame *f, int64_t ts, int64_t src)
{
    f->time = ts + j->offset - j->start;
    f->src  = src + j->offset - j->start;
    f->seg  = j->seg;
    /* the first frame after a chapter's position: the next open mark is its time */
    if (!track && (f->flags & DR_F_POSITION) && j->cfg.open_marks)
        for (int i = 0; i < j->cfg.nb_marks; i++)
            if (j->cfg.open_marks[i] == INT64_MAX) {
                j->cfg.open_marks[i] = f->time;
                break;
            }
    /* the video's frames and key frames, for the chapters (an empty marker is no frame) */
    if (!track && j->cfg.nb_marks && (f->dur || f->size)) {
        if ((add_time(&j->vtimes, &j->nb_v, &j->cap_v, f->time) < 0) ||
            ((f->flags & DR_F_KEY) && add_time(&j->ktimes, &j->nb_k, &j->cap_k, f->time) < 0)) {
            ff_disctl_frame_unref(f);
            return DR_ERR(ENOMEM);
        }
    }
    j->st.frames++;
    return j->cfg.out(j->cfg.out_opaque, track, f);
}

static void event(DRJoin *j, int kind, int track, int64_t pos, int64_t dur, int64_t skew)
{
    if (j->cfg.event)
        j->cfg.event(j->cfg.event_opaque, &(DREvent){ kind, track, pos, dur, skew, 0 });
}

/* One frame of a placed segment, with its track's expected time known. */
static int place_frame(DRJoin *j, int track, DRFrame *f)
{
    Track *t = &j->t[track];
    int64_t ts = f->time, src = ts;

    if (!f->dur && !f->size)                       /* an empty marker keeps its time */
        return give(j, track, f, ts, ts);
    if (t->kind == DR_KIND_VIDEO) {
        if (ts < 0) {
            dr_vlog(j->log, DR_VLOG_ERROR, "Title timeline: joiner: a video frame without a time\n");
            return DR_ERR_BUG;
        }
        if (!t->seeded || t->e < ts + f->dur)
            t->e = ts + f->dur;                    /* running maximum of the frame ends */
        t->seeded = 1;
        return give(j, track, f, ts, ts);
    }
    if (ts == DR_NOPTS_VALUE) {
        ts = t->e;                                 /* no time of its own: where the track is */
        src = ts;
    } else {
        int64_t gap = t->e - ts;
        if (gap > 0 && t->kind == DR_KIND_LAYER) {
            /* decoding order: an earlier time than the picture before it */
        } else if (gap > 0 && gap > f->dur && t->kind == DR_KIND_SUBTITLE) {
            /* a sub-picture replaces the one shown before it: it keeps its
             * time (the reference moves it to the end of the one before) */
            dr_vlog(j->log, DR_VLOG_DEBUG, "Title timeline: joiner: subtitle track %d: a unit at %"PRId64" ticks starts "
                   "%"PRId64" ticks before the one before it ends: kept at its time\n", track,
                   j->offset + ts - j->start, gap);
            event(j, DR_EV_SUB_OVERLAP, track, j->offset + ts - j->start, gap, 0);
        } else if (gap > 0 && gap > f->dur + DR_TICKS_PER_PTS) {
            /* earlier than expected by more than its own duration (by more
             * than one 90 kHz tick: the disc's stamps are rounded to it) */
            if (t->kind == DR_KIND_AUDIO && gap != t->last_gap) {
                event(j, DR_EV_RETIME, track, j->offset + t->e - j->start, f->dur, -gap);
                t->last_gap = gap;
            }
            ts = t->e;
            j->st.retimed++;
        }
    }
    t->e = ts + f->dur;
    return give(j, track, f, ts, src);
}

/* The expected time of an audio / subtitle track at the start of its frames
 * in this segment: audio: the first frame with a time, minus the durations
 * of the frames before it; subtitles: the first frame's time. 0 = not yet. */
static int seed(DRJoin *j, int track, int final)
{
    Track *t = &j->t[track];

    if (t->kind == DR_KIND_SUBTITLE || t->kind == DR_KIND_LAYER) {
        if (!t->nb_wait)
            return 0;
        if (t->wait[0].time == DR_NOPTS_VALUE || t->wait[0].time < 0) {
            dr_vlog(j->log, DR_VLOG_ERROR, "Title timeline: joiner: %s track %d: its first frame in segment %d "
                   "has no time\n", t->kind == DR_KIND_LAYER ? "layer" : "subtitle", track, j->seg);
            return DR_ERR_INVALID;
        }
        t->e = t->wait[0].time;
        return 1;
    }
    for (int i = 0; i < t->nb_wait && i < SEED_UNITS; i++) {
        if (t->wait[i].dur <= 0 && t->wait[i].size) {
            dr_vlog(j->log, DR_VLOG_ERROR, "Title timeline: joiner: audio track %d: a frame without a duration\n", track);
            return DR_ERR_INVALID;
        }
        if (t->wait[i].time != DR_NOPTS_VALUE && t->wait[i].time >= 0) {
            int64_t e = t->wait[i].time;
            for (int k = i - 1; k >= 0; k--)
                e -= t->wait[k].dur;
            t->e = e;
            return 1;
        }
    }
    if (final || t->nb_wait >= SEED_UNITS) {
        dr_vlog(j->log, DR_VLOG_ERROR, "Title timeline: joiner: audio track %d: none of its first %d frames in segment %d "
               "has a time\n", track, DR_MIN(t->nb_wait, SEED_UNITS), j->seg);
        return DR_ERR_INVALID;
    }
    return 0;
}

/* Places the waiting frames of a track once its expected time is known. */
static int drain(DRJoin *j, int track, int final)
{
    Track *t = &j->t[track];
    int ret;

    if (!j->placed || !t->nb_wait)
        return 0;
    if (!t->seeded && t->kind != DR_KIND_VIDEO) {
        if ((ret = seed(j, track, final)) <= 0)
            return ret;
        t->seeded = 1;
    }
    for (int i = 0; i < t->nb_wait; i++)
        if ((ret = place_frame(j, track, &t->wait[i])) < 0) {
            for (i++; i < t->nb_wait; i++)
                ff_disctl_frame_unref(&t->wait[i]);
            t->nb_wait = 0;
            return ret;
        }
    t->nb_wait = 0;
    return 0;
}

/* The segment's start (its first video time) and place on the timeline. */
static int place_segment(DRJoin *j)
{
    if (j->batch_min == DR_NOPTS_VALUE) {
        dr_vlog(j->log, DR_VLOG_ERROR, "Title timeline: joiner: segment %d has no video frame with a time\n", j->seg);
        return DR_ERR_INVALID;
    }
    j->start  = j->batch_min;
    j->offset = j->seg ? j->prev_e0 - j->prev_start + j->prev_offset : 0;
    if (j->seg >= j->nb_starts) {
        int64_t *p = dr_realloc_array(j->starts, j->seg + 1, sizeof(*p));
        if (!p)
            return DR_ERR(ENOMEM);
        for (int k = j->nb_starts; k <= j->seg; k++)
            p[k] = DR_NOPTS_VALUE;
        j->starts    = p;
        j->nb_starts = j->seg + 1;
    }
    j->starts[j->seg] = j->start;
    j->placed = 1;
    j->st.placed = 1;
    j->st.start  = j->start;
    j->st.offset = j->offset;
    dr_vlog(j->log, DR_VLOG_DEBUG, "Title timeline: joiner: segment %d starts at %"PRId64" ticks, placed at %"PRId64"\n",
           j->seg, j->start, j->offset);
    for (int k = 0; k < j->cfg.nb_tracks; k++) {
        int ret = drain(j, k, 0);
        if (ret < 0)
            return ret;
    }
    return 0;
}

static int end_segment(DRJoin *j)
{
    int ret;

    if (j->seg < 0)
        return 0;
    if (!j->placed && (ret = place_segment(j)) < 0)
        return ret;
    for (int k = 0; k < j->cfg.nb_tracks; k++)
        if ((ret = drain(j, k, 1)) < 0)
            return ret;
    j->prev_e0     = j->t[0].e;
    j->prev_start  = j->start;
    j->prev_offset = j->offset;
    return 0;
}

int ff_disctl_join_segment(DRJoin *j)
{
    int ret = end_segment(j);

    if (ret < 0)
        return ret;
    j->seg++;
    j->placed    = 0;
    j->st.placed = 0;
    j->batch_min = DR_NOPTS_VALUE;
    for (int k = 0; k < j->cfg.nb_tracks; k++)
        j->t[k].seeded = 0;
    j->st.segments = j->seg + 1;
    return 0;
}

int ff_disctl_join_push(DRJoin *j, int track, DRFrame *f)
{
    Track *t;
    int ret;

    if (track < 0 || track >= j->cfg.nb_tracks || j->seg < 0) {
        ff_disctl_frame_unref(f);
        return DR_ERR(EINVAL);
    }
    t = &j->t[track];
    if (j->placed && (t->seeded || t->kind == DR_KIND_VIDEO) && !t->nb_wait)
        return place_frame(j, track, f);

    if (!track && !j->placed && f->time != DR_NOPTS_VALUE && f->time >= 0 && (f->dur || f->size))
        j->batch_min = j->batch_min == DR_NOPTS_VALUE ? f->time : DR_MIN(j->batch_min, f->time);
    {
        int batch_end = !track && (f->flags & DR_F_BATCH);
        if ((ret = wait_add(t, f)) < 0)
            return ret;
        if (batch_end && !j->placed)
            return place_segment(j);
    }
    return drain(j, track, 0);
}

int ff_disctl_join_finish(DRJoin *j)
{
    int ret = end_segment(j);
    return ret < 0 ? ret : place_chapters(j);
}

int ff_disctl_join_open(DRJoin **jp, void *logctx, const DRJoinConfig *cfg)
{
    DRJoin *j;

    *jp = NULL;
    if (cfg->nb_tracks < 1 || cfg->kinds[0] != DR_KIND_VIDEO)
        return DR_ERR(EINVAL);
    if (!(j = dr_mallocz(sizeof(*j))) || !(j->t = dr_calloc(cfg->nb_tracks, sizeof(*j->t)))) {
        dr_free(j);
        return DR_ERR(ENOMEM);
    }
    j->log = logctx;
    j->cfg = *cfg;
    j->seg = -1;
    for (int k = 0; k < cfg->nb_tracks; k++) {
        j->t[k].kind     = cfg->kinds[k];
        j->t[k].last_gap = INT64_MIN;
    }
    *jp = j;
    return 0;
}

void ff_disctl_join_stats(const DRJoin *j, DRJoinStats *st)
{
    *st = j->st;
}

int ff_disctl_join_segment_start(const DRJoin *j, int seg, int64_t *start)
{
    if (seg < 0 || seg >= j->nb_starts || j->starts[seg] == DR_NOPTS_VALUE)
        return DR_ERR(EAGAIN);
    *start = j->starts[seg];
    return 0;
}

int ff_disctl_join_chapters(const DRJoin *j, const int64_t **starts)
{
    *starts = j->chap;
    return j->nb_chap;
}

void ff_disctl_join_close(DRJoin **jp)
{
    DRJoin *j = *jp;

    if (!j)
        return;
    for (int k = 0; k < j->cfg.nb_tracks; k++) {
        for (int i = 0; i < j->t[k].nb_wait; i++)
            ff_disctl_frame_unref(&j->t[k].wait[i]);
        dr_freep(&j->t[k].wait);
    }
    dr_freep(&j->t);
    dr_freep(&j->chap);
    dr_freep(&j->vtimes);
    dr_freep(&j->ktimes);
    dr_freep(&j->starts);
    dr_freep(jp);
}
