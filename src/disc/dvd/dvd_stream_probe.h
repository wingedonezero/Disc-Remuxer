/*
 * DVD-Video: the packets of a title VOB sector and the stream probe of a
 * title (dvd_stream_probe.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef DISC_DVD_DVD_STREAM_PROBE_H
#define DISC_DVD_DVD_STREAM_PROBE_H

#include <stdint.h>

#include "dvd_vob.h"

#include "dvd_cells.h"

/**
 * A packet of a title VOB sector by the program stream rules the stream
 * probe uses: at *off of the sector (0: the pack header first, 14 bytes plus
 * pack_stuffing_length, the stuffing counted only when its first byte is
 * not zero). *off becomes the next packet's offset, DVD_VIDEO_LB_LEN or more
 * when the sector is done: after private stream 2, at fill bytes (0x00 or
 * 0xff to the end), after padding packets (a packet after padding near the
 * sector's end only when it parses). The key: stream_id; private stream 1
 * the sub-stream id << 8 | 0xbd (after its sub-stream header: sub-pictures
 * 1 byte, audio 4, MLP 5); with a PES extension 2 its stream_id_extension
 * << 8 | stream_id. An LPCM packet (sub-stream 0xA0-0xAF) holding only its
 * 3-byte audio frame header counts as padding (0xbe); one shorter than the
 * header, or whose first access unit starts inside it, does not parse.
 * 1, 0 when the packet does not parse (why says why).
 */
typedef struct
{
  int         at;                     /* the packet's offset in the sector */
  int         len;                    /* PES_packet_length + 6; 0 when the field is 0 */
  int         key;
  int         payload;                /* the payload's offset in the packet */
  int         first_au;               /* the first access unit's offset in the payload, -1 none, -2 sub-picture */
  int         extra;                  /* LPCM: its audio frame header's bytes, kept before payload */
  const char *why;
} dvd_pack_packet_t;

int DVDPackPacket_rec( const uint8_t *sector, int *off, dvd_pack_packet_t *p );

/**
 * The stream probe of a title: one of its segments (extents: the segment's
 * extents, as libdvdread's DVDTitleSegments_rec() gives them; its sectors
 * read through the title set's own reading, DVDReadTitleVOBSector_rec())
 * read from its start, the
 * first two packets of each sector looked at, for what the title set's
 * attributes cannot tell:
 * - closed captions: the video packets (0xE0) of its first 8 MiB searched
 *   for the DVD caption user data (00 00 01 B2 'C' 'C' 0x01 0xF8, its next
 *   byte's bit 6 clear, nine bytes or more to the payload's end);
 * - each audio stream (stream number and audio_coding_mode in, whether the
 *   program chain's audio_control lists it): its packets (AC-3 0x80 + n, MPEG
 *   audio 0xC0 + n, LPCM 0xA0 + n, DTS 0x88 + n) looked for in the first
 *   48 MiB; none and not listed: DVD_PROBE_NOT_LISTED; listed: also the first
 *   200 MiB, the 100 MiB from the middle and the last 100 MiB (a segment
 *   shorter than 200 MiB: all of it), none there: DVD_PROBE_EMPTY. A coding
 *   mode that is not known: DVD_PROBE_UNKNOWN.
 * Each zone is read once (a later search goes on where an earlier one
 * stopped) and a search stops at the sector that has what it looks for.
 * 1 done; -1 no memory; -2 a sector cannot be read, -3 it is scrambled and
 * the title set has no title key (*failed_block: the segment's block).
 */
typedef enum
{
  DVD_PROBE_FOUND,                    /* data in the first 48 MiB */
  DVD_PROBE_FOUND_LATER,              /* data, not in the first 48 MiB (a listed stream) */
  DVD_PROBE_NOT_LISTED,
  DVD_PROBE_EMPTY,
  DVD_PROBE_UNKNOWN,
} dvd_probe_result_t;

typedef struct
{
  uint8_t            stream;          /* in: its stream number */
  uint8_t            coding;          /* in: its attributes' audio_coding_mode */
  int                listed;          /* in: audio_control lists it */
  dvd_probe_result_t result;          /* out */
} dvd_probe_audio_t;

int DVDProbeTitleStreams_rec( dvd_vob_reader_t *, const dvd_extent_t *extents, int nb_extents,
                              dvd_probe_audio_t *audio, int nb_audio, int *captions, uint32_t *failed_block );

#endif /* DISC_DVD_DVD_STREAM_PROBE_H */
