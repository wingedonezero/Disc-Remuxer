/*
 * DVD-Video: the navigation scan, run by libdvdnav (dvdnav/scan.h:
 * dvdnav_scan_run(): the disc played the way a viewer would play it, per
 * region the disc allows, from its start and from every title, every
 * reachable menu button pressed, the cells each path plays recorded per
 * title and program chain, named and ordered). Here the navigator is opened
 * on the disc layer (dvd_nav.c), every VOB sector it reads is handed to the
 * scan (its lines go into the log as the navigators' do); its results go to
 * the title list as they are. A failed scan leaves no results: the titles keep the order of the
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

/* A VOB sector the navigator read, to the scan. */
static int scan_sector(void *opaque, int set, int menu, uint32_t sector, const uint8_t *data)
{
    return dvdnav_scan_sector(opaque, set, menu, sector, data) < 0 ? AVERROR(EINVAL) : 0;
}

int ff_dvd_scan(DVDDisc *d, void (*trace)(void *opaque, const char *line), void *opaque, dvdnav_scan_results_t *out)
{
    dvdnav_logger_cb logcb;
    dvdnav_scan_t *s;
    dvdnav_t *nav;
    int ret;

    memset(out, 0, sizeof(*out));
    if (ff_dvd_nav_logger(d, &logcb) < 0 || !(s = dvdnav_scan_new(d->rec, d, &logcb, trace, opaque)))
        return AVERROR(ENOMEM);
    if ((ret = ff_dvd_nav_open(d, &(DVDNavHooks){ s, scan_sector }, &nav)) >= 0) {
        ret = dvdnav_scan_run(s, nav, out);
        ret = ret == -2 ? AVERROR(ENOMEM) : ret < 0 ? AVERROR_EXTERNAL : 0;
    }
    if (ret < 0)
        av_log(d->log, AV_LOG_WARNING, "DVD: the navigation scan failed: the titles keep the order of the title "
               "table\n");
    dvdnav_scan_close(s);
    return ret;
}
