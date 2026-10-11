/*
 * DVD-Video: the stream pass of a title, done by libdvdread's stream probe
 * (DVDProbeTitleStreams_rec(), vob_read.h) on the title's largest segment
 * (the first of the largest), for what its title set's attributes cannot
 * say: closed captions in the video (the title then gets a caption track,
 * English, after the sub-pictures) and the audio streams without data. Each
 * audio stream goes in with its stream number, its attributes' coding mode
 * and whether the program chain's audio_control lists it, looked up by the
 * stream's place in the title's audio list (audio_control's streams come
 * first, in its order), not by its stream number (where the two differ a
 * debug line says so). Left out: a stream of an unknown coding mode, one
 * without data in the first 48 MiB that audio_control does not list, one
 * that it lists without data in any zone the probe reads. A sector that
 * cannot be read (or a scrambled one without a key) fails the pass: the
 * title cannot be used.
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

#include <inttypes.h>

#include <dvdread/vob_read.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

int ff_dvd_title_streams(DVDDisc *d, DVDTitle *t)
{
    dvd_probe_audio_t audio[8] = { 0 };
    const DVDSegment *g;
    dvd_extent_t *ext;
    dvd_vob_reader_t *vr;
    uint32_t failed = 0;
    int64_t best = -1;
    int seg = 0, captions = 0, ret;

    t->audio_dropped = 0;
    t->captions = 0;
    if (!t->nb_segments)
        return 0;
    for (int k = 0; k < t->nb_segments; k++)
        if (t->segments[k].size > best) {
            best = t->segments[k].size;
            seg = k;
        }
    g = &t->segments[seg];
    for (int i = 0; i < t->nb_audio && i < FF_ARRAY_ELEMS(audio); i++) {
        audio[i].stream = t->audio[i].stream;
        audio[i].coding = t->vts->header[DVD_VTS_AUDIO_ATTR + 8 * t->audio[i].attr] >> 5;
        audio[i].listed = t->audio_listed >> i & 1;
    }
    if (!(ext = av_malloc_array(FFMAX(g->nb_extents, 1), sizeof(*ext))))
        return AVERROR(ENOMEM);
    for (int i = 0; i < g->nb_extents; i++)
        ext[i] = (dvd_extent_t){ g->extents[i].logical, g->extents[i].sector, g->extents[i].count };
    if (!(vr = ff_dvd_own_reading(d, t->vts)))
        ret = -2;
    else
        ret = DVDProbeTitleStreams_rec(vr, ext, g->nb_extents, audio, FFMIN(t->nb_audio, FF_ARRAY_ELEMS(audio)),
                                       &captions, &failed);
    av_free(ext);
    if (ret == -1)
        return AVERROR(ENOMEM);
    if (ret < 0) {
        av_log(d->log, AV_LOG_ERROR, "DVD: title %s: sector %"PRIu32" of segment %d cannot be read: the title's "
               "streams cannot be found\n", t->name, failed, seg + 1);
        return ret == -3 ? AVERROR_INVALIDDATA : vr ? AVERROR(EIO) : AVERROR(EINVAL);
    }
    if ((t->captions = captions))
        av_log(d->log, AV_LOG_VERBOSE, "DVD: title %s: closed captions in the video: a caption track\n", t->name);

    for (int i = 0; i < t->nb_audio && i < FF_ARRAY_ELEMS(audio); i++) {
        int s = t->audio[i].stream, listed = audio[i].listed;

        if (audio[i].result == DVD_PROBE_UNKNOWN) {
            av_log(d->log, AV_LOG_WARNING, "DVD: title %s: audio stream %d has coding mode %d, which is not known: "
                   "left out\n", t->name, s, audio[i].coding);
            t->audio_dropped |= 1 << i;
            continue;
        }
        if (audio[i].result == DVD_PROBE_FOUND)
            continue;
        if (listed != (t->audio_listed >> s & 1))
            av_log(d->log, AV_LOG_DEBUG, "DVD: title %s: audio stream %d is audio track %d: whether audio_control "
                   "lists it is taken from track %d's place (%s), not from the stream number (%s)\n", t->name, s,
                   i, i, listed ? "listed" : "not listed", listed ? "not listed" : "listed");
        if (audio[i].result == DVD_PROBE_NOT_LISTED) {
            av_log(d->log, AV_LOG_INFO, "DVD: title %s: audio stream %d has no data in the first 48 MiB and the "
                   "program chain does not list it: left out\n", t->name, s);
            t->audio_dropped |= 1 << i;
        } else if (audio[i].result == DVD_PROBE_EMPTY) {
            av_log(d->log, AV_LOG_WARNING, "DVD: title %s: audio stream %d (track %d) looks empty (no data in the "
                   "first 200 MiB, the 100 MiB from the middle and the last 100 MiB of segment %d): left out\n",
                   t->name, s, i, seg + 1);
            t->audio_dropped |= 1 << i;
        }
    }
    return 0;
}
