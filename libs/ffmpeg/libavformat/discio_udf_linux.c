/*
 * Disc I/O: UDF reader based on Linux (placeholder until the port lands)
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/error.h"
#include "libavutil/log.h"

#include "discio.h"

int ff_discio_udf_linux_mount(DiscIOSource *src, DiscIOFS **out)
{
    *out = NULL;
    av_log(src->logctx, AV_LOG_ERROR, "The UDF reader based on Linux is not built yet\n");
    return AVERROR(ENOSYS);
}
