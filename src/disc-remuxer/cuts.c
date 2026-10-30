/*
 * disc-remuxer: cuts (cuts.h). The places a part can start come from the
 * title's pictures in decoding order and the codecs' own marks of how
 * decoding starts at a key frame (ISO/IEC 13818-2 closed_gop / broken_link,
 * H.264 IDR and recovery points, H.265 IRAP pictures and their RADL / RASL
 * leading pictures, SMPTE 421M entry points).
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/err.h"
#include "base/log.h"
#include "base/macros.h"
#include "base/msg.h"
#include "base/settings.h"
#include "timeline/disctl.h"
#include "cuts.h"
#include "engine.h"

#define MS (DR_TICKS_PER_SECOND / 1000)

static int by_zero(const void *a, const void *b)
{
    const CutPoint *x = a, *y = b;
    return (x->zero > y->zero) - (x->zero < y->zero);
}

int cut_points(const CutPicture *pics, int nb, int codec, CutPoint **points, int *nb_points)
{
    CutPoint *p;
    int n = 0;

    *points    = NULL;
    *nb_points = 0;
    for (int i = 1; i < nb; i++)
        n += pics[i].key;
    if (!n)
        return 0;
    if (!(p = calloc(n, sizeof(*p))))
        return DR_ERR(ENOMEM);
    n = 0;
    for (int i = 1; i < nb; i++) {
        CutPoint *c;
        if (!pics[i].key)
            continue;
        c = &p[n++];
        c->zero = c->key_time = pics[i].time;
        c->start = pics[i].start;
        /* its leading pictures: after it in decoding order up to the next
         * key frame, shown before it */
        for (int j = i + 1; j < nb && !pics[j].key; j++) {
            if (pics[j].time >= pics[i].time)
                continue;
            if (!ff_disctl_leading_kept(pics[i].start, codec, pics[j].rasl, pics[j].ref)) {
                c->left_out++;
                continue;
            }
            if (ff_disctl_leading_decodes(pics[i].start, codec, pics[j].rasl))
                c->kept++;
            else
                c->undecodable++;
            c->zero = DR_MIN(c->zero, pics[j].time);
        }
    }
    qsort(p, n, sizeof(*p), by_zero);
    *points    = p;
    *nb_points = n;
    return 0;
}

/* One time: h:mm:ss[.f] or m:ss[.f] (minutes may pass 59 without hours);
 * 0, or -1. */
static int parse_time(const char *s, int64_t *ticks)
{
    int64_t f[3], frac = 0, scale = 1;
    int nf = 0, digits = 0;

    while (nf < 3) {
        if (!isdigit((unsigned char)*s))
            return -1;
        f[nf] = 0;
        while (isdigit((unsigned char)*s)) {
            if (f[nf] > 1000000)
                return -1;
            f[nf] = f[nf] * 10 + (*s++ - '0');
        }
        nf++;
        if (*s != ':')
            break;
        s++;
    }
    if (nf < 2)
        return -1;
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s) && digits < 9) {
            frac   = frac * 10 + (*s++ - '0');
            scale *= 10;
            digits++;
        }
        if (!digits)
            return -1;
    }
    if (*s)
        return -1;
    /* seconds below 60; minutes below 60 when hours are given */
    if (f[nf - 1] > 59 || (nf == 3 && f[1] > 59))
        return -1;
    *ticks = ((nf == 3 ? f[0] * 3600 + f[1] * 60 : f[0] * 60) + f[nf - 1]) * DR_TICKS_PER_SECOND +
             frac * DR_TICKS_PER_SECOND / scale;
    return 0;
}

int cut_parse(const char *spec, CutRequest **reqs, int *nb, const char **bad)
{
    CutRequest *r;
    int n = 1, k = 0;
    const char *s = spec;

    *reqs = NULL;
    *nb   = 0;
    *bad  = spec;
    for (const char *c = spec; *c; c++)
        n += *c == ',';
    if (!(r = calloc(n, sizeof(*r))))
        return -1;
    while (*s) {
        const char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char item[40];

        while (len && isspace((unsigned char)*s))
            s++, len--;
        while (len && isspace((unsigned char)s[len - 1]))
            len--;
        *bad = s;
        if (!len || len >= sizeof(item))
            goto fail;
        memcpy(item, s, len);
        item[len] = 0;
        memcpy(r[k].text, item, len + 1);
        if (item[0] == 'c' || item[0] == 'C') {
            char *end;
            long ch = strtol(item + 1, &end, 10);
            if (!isdigit((unsigned char)item[1]) || *end || ch < 1 || ch > 9999)
                goto fail;
            r[k].chapter = (int)ch;
        } else if (parse_time(item, &r[k].time) < 0)
            goto fail;
        k++;
        if (!e)
            break;
        s = e + 1;
        if (!*s) {
            *bad = s;
            goto fail;
        }
    }
    if (!k)
        goto fail;
    *reqs = r;
    *nb   = k;
    *bad  = NULL;
    return 0;
fail:
    free(r);
    return -1;
}

int cut_resolve(const CutPoint *points, int nb_points, const int64_t *chapters, int nb_chapters, int64_t end,
                const CutRequest *reqs, int nb_reqs, int rule, CutResult *res)
{
    int made = 0;

    for (int i = 0; i < nb_reqs; i++) {
        CutResult *r = &res[i];
        int64_t t;

        memset(r, 0, sizeof(*r));
        r->point   = -1;
        r->same_as = -1;
        if (reqs[i].chapter) {
            if (reqs[i].chapter > nb_chapters) {
                r->why = "the title has no such chapter";
                continue;
            }
            t = chapters[reqs[i].chapter - 1];
        } else
            t = reqs[i].time;
        r->target = t;
        if (t <= 0) {
            r->why = "at the title's start: no part before it";
            continue;
        }
        if (t >= end) {
            r->why = "not before the end of the title's video";
            continue;
        }
        /* on a point: its first picture or its key frame within 1 ms */
        for (int p = 0; p < nb_points && r->point < 0; p++)
            if (DR_ABS(points[p].zero - t) <= MS || DR_ABS(points[p].key_time - t) <= MS) {
                r->point    = p;
                r->on_point = 1;
            }
        if (r->point < 0 && rule == CUT_NEXT) {
            for (int p = 0; p < nb_points && r->point < 0; p++)
                if (points[p].zero > t)
                    r->point = p;
            if (r->point < 0)
                r->why = "no key frame after it";
        } else if (r->point < 0) {
            for (int p = 0; p < nb_points; p++)
                if (r->point < 0 || DR_ABS(points[p].zero - t) < DR_ABS(points[r->point].zero - t))
                    r->point = p;
            if (r->point < 0)
                r->why = "the title's video has no key frame to cut at";
        }
        if (r->point < 0)
            continue;
        for (int k = 0; k < i; k++)
            if (res[k].point == r->point) {
                r->same_as = k;
                r->point   = -1;
                r->why     = "the same key frame as an earlier cut";
                break;
            }
        made += r->point >= 0;
    }
    return made;
}

static int add_picture(CutIndex *ix, int *cap, const DrDiscFrame *fr)
{
    if (ix->nb_pics == *cap) {
        int n = *cap ? *cap * 2 : 4096;
        CutPicture *p = realloc(ix->pics, n * sizeof(*p));
        if (!p)
            return DR_ERR(ENOMEM);
        ix->pics = p;
        *cap     = n;
    }
    ix->pics[ix->nb_pics++] = (CutPicture){ fr->time, fr->duration, fr->key, (uint8_t)fr->start, fr->rasl, fr->ref };
    ix->end = DR_MAX(ix->end, fr->time + fr->duration);
    return 0;
}

int cut_index_read(DrDisc *d, int title, int64_t stop_after, CutIndex *ix)
{
    const DiscInfo *info = dr_disc_info(d);
    uint8_t *keep;
    int cap = 0, keys_after = 0, ret;

    memset(ix, 0, sizeof(*ix));
    if (!info || title < 0 || title >= info->nb_titles)
        return DR_ERR(EINVAL);
    /* the video only (track 0 is always read) */
    if (!(keep = calloc(info->titles[title].nb_streams ? info->titles[title].nb_streams : 1, 1)))
        return DR_ERR(ENOMEM);
    keep[0] = 1;
    /* the chapters as a rip places them */
    dr_disc_set_chapter_rule(d, !setting_bool("chapters.snap"), (int)setting_int("chapters.before_ms"),
                             (int)setting_int("chapters.after_ms"));
    ret = dr_disc_title_open(d, title, -1, 0, keep);
    free(keep);
    if (ret < 0) {
        log_msg(MSG_TITLE_OPEN_FAILED, LOG_ERROR, "Title #%d cannot be opened: %s", title, dr_err_text(ret));
        return ret;
    }
    ix->codec = dr_disc_nb_tracks(d) <= 0 ? DR_CUT_OTHER : dr_disc_track(d, 0)->codec == DR_CODEC_HEVC ? DR_CUT_HEVC :
                dr_disc_track(d, 0)->codec == DR_CODEC_H264 ? DR_CUT_H264 : DR_CUT_OTHER;
    for (;;) {
        DrDiscFrame fr;

        if ((ret = dr_disc_read(d, &fr)) < 0)
            break;
        if (engine_cancelled()) {
            dr_disc_frame_unref(&fr);
            ret = DR_ERR_CANCELLED;
            break;
        }
        log_progress(title, dr_disc_progress(d) / 10000.0);
        if (fr.track || fr.time == DR_NOPTS_VALUE) {
            dr_disc_frame_unref(&fr);
            continue;
        }
        ret = add_picture(ix, &cap, &fr);
        dr_disc_frame_unref(&fr);
        if (ret < 0)
            break;
        /* enough for the requests: the first key frame after them and its
         * leading pictures (read before the next key frame) */
        if (stop_after >= 0 && ix->pics[ix->nb_pics - 1].key && ix->pics[ix->nb_pics - 1].time > stop_after &&
            ++keys_after == 2)
            break;
    }
    if (ret == DR_ERR_EOF) {
        int n = dr_disc_nb_chapters(d);
        ix->complete = 1;
        ret = 0;
        if (n > 0 && !(ix->chapters = calloc(n, sizeof(*ix->chapters))))
            ret = DR_ERR(ENOMEM);
        for (int i = 0; ret >= 0 && i < n; i++)
            ix->chapters[ix->nb_chapters++] = dr_disc_chapter(d, i)->start;
    }
    dr_disc_title_close(d);
    if (ret < 0 && ret != DR_ERR_CANCELLED)
        log_msg(MSG_TITLE_FAILED, LOG_ERROR, "Title #%d: its video cannot be read for the cuts: %s", title,
                dr_err_text(ret));
    if (ret < 0)
        cut_index_free(ix);
    return ret;
}

void cut_index_free(CutIndex *ix)
{
    free(ix->pics);
    free(ix->chapters);
    memset(ix, 0, sizeof(*ix));
}

const char *cut_time_text(char *buf, int size, int64_t t)
{
    int64_t ns = t < 0 ? -((-t * 25 + 13) / 27) : (t * 25 + 13) / 27, a = ns < 0 ? -ns : ns;
    snprintf(buf, size, "%s%02"PRId64":%02"PRId64":%02"PRId64".%03"PRId64".%03"PRId64".%03"PRId64, ns < 0 ? "-" : "",
             a / INT64_C(3600000000000), a / INT64_C(60000000000) % 60, a / 1000000000 % 60, a / 1000000 % 1000,
             a / 1000 % 1000, a % 1000);
    return buf;
}
