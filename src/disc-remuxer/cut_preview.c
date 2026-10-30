/*
 * disc-remuxer: the cut editor's pictures (cut_preview.h).
 */

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "analyze/preview.h"
#include "base/err.h"
#include "base/log.h"
#include "base/msg.h"
#include "disc/disc.h"
#include "timeline/disctl.h"
#include "cut_preview.h"
#include "writer.h"

/* A decoded picture kept. */
typedef struct Kept {
    int            number, width, height;
    DrPreviewImage img;
    uint64_t       used;            /* when it was last asked for */
} Kept;

struct CutPreview {
    DrDisc         *disc;           /* opened for this preview alone */
    int             title;
    const CutIndex *ix;
    int            *disp;           /* display number -> the index's picture (decoding order) */
    int            *num;            /* the index's picture -> display number */
    uint8_t        *keep;           /* the title's tracks: the video only */
    DrPreview      *dec;
    /* the reading */
    int             open;
    int             from;           /* the index picture (a key frame) it started at */
    int             next;           /* the index picture the next unit read is */
    int64_t         offset;         /* index time - the time read */
    int             have_offset;
    int             decoded;        /* pictures decoded since the call began */
    /* the pictures kept */
    Kept           *kept;
    int             nb_kept, cap_kept;
    int64_t         bytes, max_bytes;
    uint64_t        clock;
};

static const CutIndex *sort_ix;

static int by_time(const void *a, const void *b)
{
    int64_t x = sort_ix->pics[*(const int *)a].time, y = sort_ix->pics[*(const int *)b].time;
    return (x > y) - (x < y);
}

int cut_preview_open(CutPreview **pp, const char *source, int title, const CutIndex *ix, int64_t cache_bytes)
{
    CutPreview *p;
    const DiscInfo *info;
    int ret;

    *pp = NULL;
    if (!ix || ix->nb_pics <= 0)
        return DR_ERR(EINVAL);
    if (!(p = calloc(1, sizeof(*p))) || !(p->disp = malloc(ix->nb_pics * sizeof(*p->disp))) ||
        !(p->num = malloc(ix->nb_pics * sizeof(*p->num)))) {
        cut_preview_close(&p);
        return DR_ERR(ENOMEM);
    }
    p->title     = title;
    p->ix        = ix;
    p->max_bytes = cache_bytes;
    for (int i = 0; i < ix->nb_pics; i++)
        p->disp[i] = i;
    sort_ix = ix;
    qsort(p->disp, ix->nb_pics, sizeof(*p->disp), by_time);
    for (int n = 0; n < ix->nb_pics; n++)
        p->num[p->disp[n]] = n;
    /* the disc again, its lines kept to the detail level */
    if ((ret = open_disc_log(source, "preview", &p->disc)) < 0 || !(info = dr_disc_info(p->disc)) ||
        title < 0 || title >= info->nb_titles) {
        log_msg(MSG_TITLE_OPEN_FAILED, LOG_ERROR, "Cut editor: %s cannot be opened for its pictures: %s", source,
                dr_err_text(ret < 0 ? ret : DR_ERR(EINVAL)));
        cut_preview_close(&p);
        return ret < 0 ? ret : DR_ERR(EINVAL);
    }
    if (!(p->keep = calloc(info->titles[title].nb_streams ? info->titles[title].nb_streams : 1, 1))) {
        cut_preview_close(&p);
        return DR_ERR(ENOMEM);
    }
    p->keep[0] = 1;
    *pp = p;
    return 0;
}

int cut_preview_count(const CutPreview *p)
{
    return p->ix->nb_pics;
}

int64_t cut_preview_time(const CutPreview *p, int n)
{
    return n >= 0 && n < p->ix->nb_pics ? p->ix->pics[p->disp[n]].time : DR_NOPTS_VALUE;
}

int cut_preview_at(const CutPreview *p, int64_t t)
{
    int lo = 0, hi = p->ix->nb_pics - 1;

    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (p->ix->pics[p->disp[mid]].time <= t)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static Kept *kept_find(CutPreview *p, int n, int w, int h)
{
    for (int i = 0; i < p->nb_kept; i++)
        if (p->kept[i].number == n && p->kept[i].width == w && p->kept[i].height == h)
            return &p->kept[i];
    return NULL;
}

/* A decoded picture kept; the ones asked for longest ago go while the
 * pictures take more than the limit (the one just kept stays). */
static int kept_add(CutPreview *p, int n, int w, int h, DrPreviewImage *img)
{
    int64_t size = (int64_t)img->stride * img->height;

    if (kept_find(p, n, w, h)) {
        dr_preview_image_free(img);
        return 0;
    }
    while (p->nb_kept && p->bytes + size > p->max_bytes) {
        int old = 0;
        for (int i = 1; i < p->nb_kept; i++)
            if (p->kept[i].used < p->kept[old].used)
                old = i;
        p->bytes -= (int64_t)p->kept[old].img.stride * p->kept[old].img.height;
        dr_preview_image_free(&p->kept[old].img);
        p->kept[old] = p->kept[--p->nb_kept];
    }
    if (p->nb_kept == p->cap_kept) {
        int cap = p->cap_kept ? 2 * p->cap_kept : 64;
        Kept *k = realloc(p->kept, cap * sizeof(*k));
        if (!k) {
            dr_preview_image_free(img);
            return DR_ERR(ENOMEM);
        }
        p->kept     = k;
        p->cap_kept = cap;
    }
    p->kept[p->nb_kept++] = (Kept){ n, w, h, *img, ++p->clock };
    p->bytes += size;
    return 0;
}

/* The pictures the decoder gives out, kept (but those it cannot decode
 * right: the leading pictures of an open key frame the reading started
 * at). */
static int drain(CutPreview *p, int w, int h)
{
    const CutPicture *s = &p->ix->pics[p->from];
    DrPreviewImage img;
    int ret;

    while ((ret = dr_preview_receive(p->dec, w, h, &img)) > 0) {
        int n = cut_preview_at(p, img.time);
        p->decoded++;
        if (cut_preview_time(p, n) != img.time ||
            (img.time < s->time && s->start != DR_DISC_START_CLEAN)) {
            dr_preview_image_free(&img);
            continue;
        }
        if ((ret = kept_add(p, n, w, h, &img)) < 0)
            return ret;
    }
    return ret;
}

/* The reading from the place of key frame `from` (an index picture). */
static int restart(CutPreview *p, int from)
{
    const CutPicture *k = &p->ix->pics[from];
    int ret;

    dr_disc_title_close(p->disc);
    p->open = 0;
    if (k->block < 0) {
        log_msg(MSG_CUTS, LOG_ERROR, "Cut editor: the key frame at %"PRId64" ticks has no place on the disc", k->time);
        return DR_ERR(EINVAL);
    }
    if ((ret = dr_disc_title_open_at(p->disc, p->title, k->seg, k->block, p->keep)) < 0) {
        log_msg(MSG_CUTS, LOG_ERROR, "Cut editor: title #%d cannot be read from segment %d, block %"PRId64": %s",
                p->title, k->seg + 1, k->block, dr_err_text(ret));
        return ret;
    }
    if (!p->dec && (ret = dr_preview_open(&p->dec, dr_disc_track(p->disc, 0))) < 0) {
        log_msg(MSG_CUTS, LOG_ERROR, "Cut editor: no decoder for the title's video: %s", dr_err_text(ret));
        return ret;
    }
    dr_preview_reset(p->dec);
    p->open        = 1;
    p->from        = from;
    p->next        = from;
    p->have_offset = 0;
    return 0;
}

int cut_preview_picture(CutPreview *p, int n, int w, int h, CutFrame *out)
{
    const CutIndex *ix = p->ix;
    Kept *k;
    int d, s, ret;

    memset(out, 0, sizeof(*out));
    if (n < 0 || n >= ix->nb_pics)
        return DR_ERR(EINVAL);
    p->decoded = 0;
    if (!(k = kept_find(p, n, w, h))) {
        /* the key frame the decoding starts at: its group's, or the one
         * before when the picture is a leading picture of a key frame that
         * does not start cleanly */
        d = p->disp[n];
        for (s = d; s >= 0 && !ix->pics[s].key; s--)
            ;
        if (s < 0)
            s = 0;
        if (ix->pics[d].time < ix->pics[s].time && ix->pics[s].start != DR_DISC_START_CLEAN && s > 0) {
            int b = s - 1;
            while (b > 0 && !ix->pics[b].key)
                b--;
            s = b;
        }
        /* the reading goes on when it started at or before that key frame
         * and has not passed the picture by more than the decoder holds */
        if (!p->open || p->from > s || p->next > d + 16 || p->next < s) {
            if ((ret = restart(p, s)) < 0)
                return ret;
        }
        while (!(k = kept_find(p, n, w, h))) {
            DrDiscFrame fr;

            ret = dr_disc_read(p->disc, &fr);
            if (ret == DR_ERR_EOF) {
                dr_preview_send(p->dec, NULL, 0, 0);
                if ((ret = drain(p, w, h)) < 0)
                    return ret;
                p->open = 0;
                if ((k = kept_find(p, n, w, h)))
                    break;
                log_msg(MSG_CUTS, LOG_ERROR, "Cut editor: picture %d was not decoded", n);
                return DR_ERR_INVALID;
            }
            if (ret < 0)
                return ret;
            if (fr.track) {
                dr_disc_frame_unref(&fr);
                continue;
            }
            /* the first picture read is the key frame; every one after it
             * must be the index's next one */
            if (!p->have_offset) {
                if (!fr.key) {
                    dr_disc_frame_unref(&fr);
                    log_msg(MSG_CUTS, LOG_ERROR, "Cut editor: the reading from segment %d, block %"PRId64" does not "
                            "start with a key frame", ix->pics[p->from].seg + 1, ix->pics[p->from].block);
                    p->open = 0;
                    return DR_ERR_INVALID;
                }
                p->offset      = ix->pics[p->from].time - fr.time;
                p->have_offset = 1;
            }
            if (p->next >= ix->nb_pics || fr.time + p->offset != ix->pics[p->next].time ||
                !!fr.key != !!ix->pics[p->next].key) {
                log_msg(MSG_CUTS, LOG_ERROR, "Cut editor: the picture read from segment %d, block %"PRId64" is not the "
                        "index's picture %d (at %"PRId64", read %"PRId64" ticks): the place does not hold it",
                        ix->pics[p->from].seg + 1, ix->pics[p->from].block, p->next,
                        p->next < ix->nb_pics ? ix->pics[p->next].time : -1, fr.time + p->offset);
                dr_disc_frame_unref(&fr);
                p->open = 0;
                return DR_ERR_INVALID;
            }
            ret = dr_preview_send(p->dec, fr.data, fr.size, ix->pics[p->next].time);
            dr_disc_frame_unref(&fr);
            p->next++;
            if (ret < 0 || (ret = drain(p, w, h)) < 0)
                return ret;
        }
    }
    k->used = ++p->clock;
    *out = (CutFrame){ n, ix->pics[p->disp[n]].time, ix->pics[p->disp[n]].key, ix->pics[p->disp[n]].start,
                       k->img.width, k->img.height, k->img.stride, k->img.bgra, "", p->decoded,
                       p->decoded ? p->num[p->from] : -1 };
    memcpy(out->md5, k->img.md5, sizeof(out->md5));
    return 0;
}

void cut_preview_close(CutPreview **pp)
{
    CutPreview *p = *pp;

    if (!p)
        return;
    for (int i = 0; i < p->nb_kept; i++)
        dr_preview_image_free(&p->kept[i].img);
    free(p->kept);
    dr_preview_close(&p->dec);
    if (p->disc) {
        dr_disc_title_close(p->disc);
        dr_disc_close(&p->disc);
    }
    free(p->keep);
    free(p->disp);
    free(p->num);
    free(p);
    *pp = NULL;
}
