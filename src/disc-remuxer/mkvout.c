/*
 * disc-remuxer: a title into one Matroska file (output.format = mkv; the
 * writer in src/mkv). The title's tracks the writer takes (H.264 video,
 * LPCM, AC-3 / E-AC-3 and DTS audio and PGS subtitles so far) go in with the
 * title's own times and chapters; the others are left out, each said in the
 * log. The frames are held until every video and audio track in the file has
 * given its first one (LPCM's format is known from then on), then written in
 * the order they come (subtitles, which may start late, are not waited for).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "base/err.h"
#include "base/log.h"
#include "base/macros.h"
#include "base/mem.h"
#include "base/settings.h"
#include "disc/disc.h"
#include "disc/info.h"
#include "mkv/mkv.h"
#include "timeline/disctl.h"
#include "mkvout.h"

#define HOLD_MAX 4096           /* frames held until every track has started */

struct MkvOut {
    char        *path;
    MkvWriter   *w;
    int          nb_tracks;     /* the title's tracks */
    int         *map;           /* title track -> file track, -1 left out */
    int          nb_file;       /* tracks in the file */
    int          nb_wait;       /* of them the ones waited for (video, audio) */
    int         *started;       /* file track: its first frame came */
    int          nb_started;
    DrDiscFrame *hold;
    int          nb_hold;
    MkvChapter  *est;           /* the chapters as the title list has them */
    int          nb_est;
    MkvFileSpec  spec;
    int          opened;
};

static int64_t ns_of(int64_t ticks)
{
    /* nearest ns (DR_TICKS_PER_SECOND = 1.08 x 1e9) */
    return dr_rescale(ticks, 25, 27);
}

/* A frame's time in ns: PGS sets at their presentation time on the disc's
 * 90 kHz clock (as a .sup file carries it, the nearest tick; as ns cut to
 * the whole ns), every other frame to the nearest ns. */
static int64_t frame_ns(const DrDiscTrack *t, int64_t ticks)
{
    if (t->codec == DR_CODEC_HDMV_PGS_SUBTITLE)
        return dr_rescale(ticks, 1, DR_TICKS_PER_SECOND / 90000) * 100000 / 9;
    return ns_of(ticks);
}

static int mkv_codec_of(const DrDiscTrack *t)
{
    if (t->type == DISC_MEDIA_VIDEO && t->codec == DR_CODEC_H264 && !t->second_view)
        return MKV_CODEC_H264;
    if (t->type == DISC_MEDIA_AUDIO && (t->codec == DR_CODEC_AC3 || t->codec == DR_CODEC_EAC3))
        return MKV_CODEC_AC3;
    if (t->type == DISC_MEDIA_AUDIO && t->codec == DR_CODEC_DTS)
        return MKV_CODEC_DTS;
    if (t->type == DISC_MEDIA_SUBTITLE && t->codec == DR_CODEC_HDMV_PGS_SUBTITLE)
        return MKV_CODEC_PGS;
    /* LPCM: the disc's (before its first frame) or the PCM it leaves as */
    if (t->type == DISC_MEDIA_AUDIO && (t->codec == DR_CODEC_PCM_S16LE || t->codec == DR_CODEC_PCM_S24LE ||
                                        t->codec == DR_CODEC_PCM_BLURAY || t->codec == DR_CODEC_PCM_DVD))
        return MKV_CODEC_PCM;
    return 0;
}

int mkvout_open(MkvOut **pm, DrDisc *d, const char *path, int title, int part)
{
    MkvOut *m = dr_calloc(1, sizeof(*m));
    const DiscInfo *info = dr_disc_info(d);

    if (!m || !(m->path = dr_strdup(path)))
        goto nomem;
    m->nb_tracks = dr_disc_nb_tracks(d);
    if (!(m->map = dr_calloc(m->nb_tracks ? m->nb_tracks : 1, sizeof(*m->map))) ||
        !(m->started = dr_calloc(m->nb_tracks ? m->nb_tracks : 1, sizeof(*m->started))) ||
        !(m->hold = dr_calloc(HOLD_MAX, sizeof(*m->hold))))
        goto nomem;
    for (int i = 0; i < m->nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        if (mkv_codec_of(t)) {
            m->map[i] = m->nb_file++;
            m->nb_wait += mkv_codec_of(t) != MKV_CODEC_PGS;
        } else {
            m->map[i] = -1;
            log_text(LOG_WARNING, "Track %d (%s): not written into the MKV file: the MKV writer takes H.264 video, "
                     "LPCM, AC-3, E-AC-3 and DTS audio and PGS subtitles so far", t->number,
                     ff_disctl_codec_id_name(t->codec));
        }
    }
    /* the chapters as the title list has them (a part: those inside it, from
     * its own start): their room near the file's start */
    if (info && title < info->nb_titles) {
        const DiscTitleInfo *ti = &info->titles[title];
        int64_t from = 0, to = INT64_MAX;
        if (part >= 0 && part < ti->nb_parts) {
            from = ti->parts[part].start;
            to   = from + ti->parts[part].duration;
        }
        if (ti->nb_chapters && (m->est = dr_calloc(ti->nb_chapters, sizeof(*m->est)))) {
            for (int i = 0; i < ti->nb_chapters; i++)
                if (ti->chapters[i].start >= from && ti->chapters[i].start < to) {
                    m->est[m->nb_est].start = (ti->chapters[i].start - from) * 1000;
                    m->est[m->nb_est].name  = ti->chapters[i].name;
                    m->nb_est++;
                }
        }
    }
    *pm = m;
    return 0;
nomem:
    mkvout_close(&m, d, 1, NULL);
    return DR_ERR(ENOMEM);
}

/* Every file track has started: the file is opened and the held frames go in. */
static int open_file(MkvOut *m, DrDisc *d)
{
    MkvTrackSpec *specs = dr_calloc(m->nb_file ? m->nb_file : 1, sizeof(*specs));
    int ret;

    if (!specs)
        return DR_ERR(ENOMEM);
    for (int i = 0; i < m->nb_tracks; i++) {
        const DrDiscTrack *t = dr_disc_track(d, i);
        MkvTrackSpec *s;
        if (m->map[i] < 0)
            continue;
        s = &specs[m->map[i]];
        s->codec    = mkv_codec_of(t);
        s->language = t->language;
        s->forced   = t->forced;
        s->width    = t->width;
        s->height   = t->height;
        s->sample_rate = t->sample_rate;
        s->channels    = t->channels;
        if (s->codec == MKV_CODEC_PCM)
            s->bits    = t->codec == DR_CODEC_PCM_S24LE ? 24 : 16;
        if (s->codec == MKV_CODEC_PGS)
            s->zlib    = !strcmp(setting_text("mkv.compression"), "zlib");
        if ((s->codec == MKV_CODEC_AC3 || s->codec == MKV_CODEC_DTS) &&
            (s->remove_dialnorm = setting_bool("mkv.remove_dialog_normalization")))
            log_text(LOG_INFO, "Track %d: its dialogue normalization is set to none in the MKV file "
                     "(mkv.remove_dialog_normalization)", t->number);
    }
    /* DR_MKV_NO_VARIABLE_DATA (tests): fixed UIDs, no date, mkvmerge's
     * "no_variable_data" application names, so a file can be compared byte
     * for byte with the one mkvmerge writes with --engage no_variable_data */
    if (getenv("DR_MKV_NO_VARIABLE_DATA")) {
        m->spec.muxing_app  = "no_variable_data";
        m->spec.writing_app = "no_variable_data";
        m->spec.date        = 0;
        m->spec.fixed_ids   = 1;
    } else {
        m->spec.muxing_app  = "libebml v1.4.7 + libmatroska v1.7.2";
        m->spec.writing_app = "disc-remuxer " DISC_REMUXER_VERSION;
        m->spec.date        = (int64_t)time(NULL);
    }
    m->spec.chapters    = m->est;
    m->spec.nb_chapters = m->nb_est;
    ret = mkv_open(&m->w, m->path, &m->spec, specs, m->nb_file);
    dr_free(specs);
    if (ret < 0)
        return DR_ERR(-ret);
    m->opened = 1;
    for (int k = 0; k < m->nb_hold; k++) {
        DrDiscFrame *fr = &m->hold[k];
        if (ret >= 0) {
            ret = mkv_frame(m->w, m->map[fr->track], fr->data, fr->size,
                            frame_ns(dr_disc_track(d, fr->track), fr->time),
                            fr->duration > 0 ? ns_of(fr->duration) : -1, fr->key);
            if (ret < 0)
                ret = DR_ERR(-ret);
        }
        dr_disc_frame_unref(fr);
    }
    m->nb_hold = 0;
    return ret < 0 ? ret : 0;
}

int mkvout_frame(MkvOut *m, DrDisc *d, DrDiscFrame *fr)
{
    int ft = fr->track < m->nb_tracks ? m->map[fr->track] : -1;
    int ret;

    if (ft < 0 || fr->time == DR_NOPTS_VALUE) {
        dr_disc_frame_unref(fr);
        return 0;
    }
    if (!m->opened) {
        if (!m->started[ft] && dr_disc_track(d, fr->track)->type != DISC_MEDIA_SUBTITLE) {
            m->started[ft] = 1;
            m->nb_started++;
        }
        if (m->nb_hold == HOLD_MAX) {
            dr_disc_frame_unref(fr);
            log_text(LOG_ERROR, "MKV: a track gave no frame within the first %d frames of the others", HOLD_MAX);
            return DR_ERR(EINVAL);
        }
        m->hold[m->nb_hold++] = *fr;    /* the frame's reference is held */
        if (m->nb_started < m->nb_wait)
            return 0;
        return open_file(m, d);
    }
    ret = mkv_frame(m->w, ft, fr->data, fr->size, frame_ns(dr_disc_track(d, fr->track), fr->time),
                    fr->duration > 0 ? ns_of(fr->duration) : -1, fr->key);
    dr_disc_frame_unref(fr);
    return ret < 0 ? DR_ERR(-ret) : 0;
}

int mkvout_close(MkvOut **pm, DrDisc *d, int abort, int64_t *bytes)
{
    MkvOut *m = *pm;
    int ret = 0;

    if (!m)
        return 0;
    *pm = NULL;
    /* a track that never gave a frame: the file is written with what came */
    if (!abort && !m->opened && m->nb_hold && m->nb_file)
        ret = open_file(m, d);
    if (m->w) {
        int nb = dr_disc_nb_chapters(d);
        MkvChapter *ch = nb ? dr_calloc(nb, sizeof(*ch)) : NULL;
        MkvStats st;
        int r;
        for (int i = 0; ch && i < nb; i++) {
            const DrDiscChapter *c = dr_disc_chapter(d, i);
            ch[i].start = ns_of(c->start);
            ch[i].name  = c->name;
        }
        r = mkv_close(&m->w, ch, ch ? nb : 0, abort || ret < 0, &st);
        if (r < 0 && ret >= 0)
            ret = DR_ERR(-r);
        if (ret >= 0 && !abort) {
            log_text(LOG_DETAIL, "MKV: %"PRId64" clusters, %"PRId64" blocks, %"PRId64" cue points, %"PRId64" bytes",
                     st.clusters, st.blocks, st.cues, st.bytes);
            for (int k = 0; k < st.nb_removed && k < (int)(sizeof(st.removed) / sizeof(*st.removed)); k++)
                for (int i = 0; i < m->nb_tracks; i++)
                    if (m->map[i] == st.removed[k]) {
                        const DrDiscTrack *t = dr_disc_track(d, i);
                        log_text(LOG_INFO, "Track %d (%s%s): no frames in the title: taken out of the MKV file's "
                                 "track list", t->number, ff_disctl_codec_id_name(t->codec),
                                 t->forced ? ", forced subtitles" : "");
                    }
            if (st.late)
                log_text(LOG_WARNING, "MKV: %"PRId64" subtitle display set(s) came after later video or audio was "
                         "written: they are in the file out of time order", st.late);
            if (bytes)
                *bytes = st.bytes;
        }
        dr_free(ch);
    }
    for (int k = 0; k < m->nb_hold; k++)
        dr_disc_frame_unref(&m->hold[k]);
    dr_free(m->hold);
    dr_free(m->map);
    dr_free(m->started);
    dr_free(m->est);
    dr_free(m->path);
    dr_free(m);
    return ret;
}

int mkvout_has_track(const MkvOut *m, int track)
{
    return m && track >= 0 && track < m->nb_tracks && m->map[track] >= 0;
}
