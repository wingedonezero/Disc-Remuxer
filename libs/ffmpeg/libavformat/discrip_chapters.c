/*
 * The shared rip core: a title's chapters. The disc's chapter records
 * become the mark times the joiner puts on video key frames, and the list
 * of chapters those frames start.
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

#include "libavutil/common.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"

#include "discrip.h"

#define SNAP 107999999LL         /* a first mark this close after the title start (just under 0.1 s) is at 0 */

void ff_discrip_chapter_plan_free(DRChapterPlan *p)
{
    av_freep(&p->marks);
    av_freep(&p->atoms);
    p->nb_marks = p->nb_atoms = 0;
}

int ff_discrip_chapter_plan(void *log, const int64_t *r, int n, int64_t skip, int chapter00, DRChapterPlan *p)
{
    int first = -1, at_start = 0;
    int64_t prev = 0;

    memset(p, 0, sizeof(*p));
    if (!n)
        return 0;
    /* a broken tail: a last record at 0 after one that is not, or one
     * earlier than the record before it, is cut off */
    while (n > 1 && ((!r[n - 1] && r[n - 2]) || (uint64_t)r[n - 2] > (uint64_t)r[n - 1])) {
        av_log(log, AV_LOG_WARNING, "Rip core: chapters: record %d (at %"PRId64") does not follow record %d "
               "(at %"PRId64"): cut off with the records after it\n", n - 1, r[n - 1], n - 2, r[n - 2]);
        n--;
    }
    /* times must not go back: the title is refused */
    for (int i = 0; i < n; i++) {
        if ((uint64_t)r[i] < (uint64_t)prev) {
            av_log(log, AV_LOG_ERROR, "Rip core: chapters: record %d (at %"PRId64") is earlier than the one "
                   "before it (at %"PRId64"): the title's chapter list is broken\n", i, r[i], prev);
            return AVERROR_INVALIDDATA;
        }
        prev = r[i];
    }
    if (!(p->marks = av_malloc_array(n + 1, sizeof(*p->marks))) ||
        !(p->atoms = av_malloc_array(n + 1, sizeof(*p->atoms)))) {
        ff_discrip_chapter_plan_free(p);
        return AVERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++) {
        int64_t d;

        if ((uint64_t)r[i] < (uint64_t)skip) {
            av_log(log, AV_LOG_DEBUG, "Rip core: chapters: record %d is in the leading segments left out: "
                   "dropped\n", i);
            continue;
        }
        d = r[i] - skip;
        if (!p->nb_atoms && d <= SNAP) {
            d        = 0;
            at_start = 1;
        }
        if (p->nb_marks && d <= p->marks[p->nb_marks - 1]) {
            /* the same time as the record before it: one chapter (the
             * reference keeps the record, so later names shift by one) */
            av_log(log, AV_LOG_WARNING, "Rip core: chapters: record %d is at the same time as the one before it: "
                   "dropped\n", i);
            continue;
        }
        p->marks[p->nb_marks++] = d;
        if (first < 0)
            first = i;
        p->atoms[p->nb_atoms++] = i;
    }
    if (!p->nb_atoms || at_start)
        return 0;
    /* the first chapter does not start the title: one is added at 0 */
    if (first) {
        memmove(p->atoms + 1, p->atoms, p->nb_atoms * sizeof(*p->atoms));
        p->atoms[0] = 0;
    } else if (chapter00) {
        memmove(p->atoms + 1, p->atoms, p->nb_atoms * sizeof(*p->atoms));
        p->atoms[0] = DR_CHAPTER_00;
    } else
        return 0;
    memmove(p->marks + 1, p->marks, p->nb_marks * sizeof(*p->marks));
    p->marks[0] = 0;
    p->nb_marks++;
    p->nb_atoms++;
    return 0;
}

int ff_discrip_chapter_list(const DRChapterPlan *p, const int64_t *starts, int nb_starts, int64_t duration,
                            DRChapter **out, int *nb_out)
{
    DRChapter *c;
    int n = FFMIN(nb_starts, p->nb_atoms);

    *out    = NULL;
    *nb_out = 0;
    if (n < 2)
        return 0;
    if (!(c = av_malloc_array(n, sizeof(*c))))
        return AVERROR(ENOMEM);
    for (int k = 0; k < n; k++) {
        c[k].start  = starts[k];
        c[k].end    = k + 1 < n ? starts[k + 1] : duration;
        c[k].record = p->atoms[k];
    }
    *out    = c;
    *nb_out = n;
    return 0;
}
