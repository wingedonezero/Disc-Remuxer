/*
 * DVD-Video: the navigation scan, run by libdvdnav (dvdnav/scan.h:
 * dvdnav_scan_run(): the disc played the way a viewer would play it, per
 * region the disc allows, from its start and from every title, every
 * reachable menu button pressed, the cells each path plays recorded per
 * title and program chain, named and ordered). Here the navigator is opened
 * on the disc layer (dvd_nav.c), every VOB sector it reads is handed to the
 * scan, the scan's lines are logged and its results copied for the title
 * list. A failed scan leaves no results: the titles keep the order of the
 * title table.
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <dvdnav/dvdnav.h>
#include <dvdnav/scan.h>

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/macros.h"
#include "libavutil/mem.h"

#include "dvd_internal.h"

/* The scan's lines: libdvdnav's levels INFO, ERROR, WARN, DEBUG. */
static void scan_log(void *priv, dvdnav_logger_level_t level, const char *fmt, va_list va)
{
    static const int levels[] = { AV_LOG_VERBOSE, AV_LOG_ERROR, AV_LOG_WARNING, AV_LOG_DEBUG };
    DVDDisc *d = priv;
    char line[1024];
    size_t n;

    vsnprintf(line, sizeof(line), fmt, va);
    n = strlen(line);
    av_log(d->log, levels[FFMIN((unsigned)level, 3)], "DVD: %s%s", line, n && line[n - 1] == '\n' ? "" : "\n");
}

/* A VOB sector the navigator read, to the scan. */
static int scan_sector(void *opaque, int set, int menu, uint32_t sector, const uint8_t *data)
{
    return dvdnav_scan_sector(opaque, set, menu, sector, data) < 0 ? AVERROR(EINVAL) : 0;
}

int ff_dvd_scan(DVDDisc *d, void (*trace)(void *opaque, const char *line), void *opaque, DVDScan *out)
{
    static const dvdnav_logger_cb logcb = { scan_log, NULL };
    dvdnav_scan_results_t r = { 0 };
    dvdnav_scan_t *s;
    dvdnav_t *nav;
    int ret;

    memset(out, 0, sizeof(*out));
    if (!(s = dvdnav_scan_new(d->rec, d, &logcb, trace, opaque)))
        return AVERROR(ENOMEM);
    if ((ret = ff_dvd_nav_open(d, &(DVDNavHooks){ s, scan_sector }, &nav)) >= 0) {
        ret = dvdnav_scan_run(s, nav, &r);
        ret = ret == -2 ? AVERROR(ENOMEM) : ret < 0 ? AVERROR_EXTERNAL : 0;
        for (int i = 0; i < d->nb_ifos && i < FF_ARRAY_ELEMS(r.entered); i++)
            if (d->ifos[i] && r.entered[i])
                d->ifos[i]->entered_by_navigation = 1;
    }
    if (ret >= 0 && !(out->results = av_calloc(FFMAX(r.nb_results, 1), sizeof(*out->results))))
        ret = AVERROR(ENOMEM);
    for (int i = 0; ret >= 0 && i < r.nb_results; i++) {
        const dvdnav_scan_result_t *a = &r.results[i];
        DVDScanResult *o = &out->results[i];
        memcpy(o->name, a->name, sizeof(o->name));
        o->title    = a->title;
        o->pgc      = a->pgc;
        o->nb_cells = a->nb_cells;
        if (!(o->cells = av_memdup(a->cells, FFMAX(a->nb_cells, 1)))) {
            ret = AVERROR(ENOMEM);
            break;
        }
        out->nb_results++;
    }
    if (ret < 0) {
        ff_dvd_scan_free(out);
        av_log(d->log, AV_LOG_WARNING, "DVD: the navigation scan failed: the titles keep the order of the title "
               "table\n");
    }
    dvdnav_scan_free_results(&r);
    dvdnav_scan_close(s);
    return ret;
}

void ff_dvd_scan_free(DVDScan *s)
{
    for (int i = 0; i < s->nb_results; i++)
        av_freep(&s->results[i].cells);
    av_freep(&s->results);
    s->nb_results = 0;
}
