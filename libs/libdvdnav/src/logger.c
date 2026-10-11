/*
* This file is part of libdvdnav, a DVD navigation library.
*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
*/

#include "config.h"
#include <stdio.h>
#include <stdarg.h>

#include "dvdnav/dvdnav.h"
#include "logger.h"

void dvdnav_log( void *priv, const dvdnav_logger_cb *logcb,
                 dvdnav_logger_level_t level, const char *fmt, ... )
{
    va_list list;
    va_start(list, fmt);
    if(logcb && logcb->pf_log)
        logcb->pf_log(priv, level, fmt, list);
    else
    {
        FILE *stream = (level == DVDNAV_LOGGER_LEVEL_ERROR) ? stderr : stdout;
        fprintf(stream, "libdvdnav: ");
        vfprintf(stream, fmt, list);
        fprintf(stream, "\n");
    }
    va_end(list);
}

void dvdnav_report( void *priv, const dvdnav_logger_cb *logcb,
                    dvd_report_level_t level, int64_t sector,
                    const char *fmt, ... )
{
    char text[512];
    va_list list;
    dvd_report_t report = { level, NULL, NULL, NULL, -1, sector, NULL, 0, text };

    va_start(list, fmt);
    vsnprintf(text, sizeof(text), fmt, list);
    va_end(list);
    if(logcb && logcb->pf_report) {
        logcb->pf_report(priv, &report);
        dvdnav_log(priv, logcb, DVDNAV_LOGGER_LEVEL_DEBUG, "%s", text);
    } else {
        dvdnav_log(priv, logcb, level == DVD_REPORT_ERROR ? DVDNAV_LOGGER_LEVEL_ERROR :
                                level == DVD_REPORT_WARN  ? DVDNAV_LOGGER_LEVEL_WARN :
                                                            DVDNAV_LOGGER_LEVEL_INFO, "%s", text);
    }
}
