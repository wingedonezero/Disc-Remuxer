/*
 * Disc demuxers: language codes.
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

#ifndef AVFORMAT_DISCLANG_H
#define AVFORMAT_DISCLANG_H

/**
 * The ISO 639-2 bibliographic code of a language code written on a disc:
 * a 2-letter ISO 639-1 code, a 3-letter ISO 639-2 code (bibliographic or
 * terminological) or one of "jp" (Japanese), "iw" (Hebrew) and "ptb"
 * (Brazilian Portuguese, its own code). Matched as written, then in lower
 * case; only 2 or 3 characters, ASCII only. The withdrawn ISO 639-1 codes
 * "in", "ji", "jw", "mo" and "sh" are not known.
 * @return the 3-letter code, NULL when the code is not known
 */
const char *ff_disc_lang_code(const char *code);

#endif /* AVFORMAT_DISCLANG_H */
