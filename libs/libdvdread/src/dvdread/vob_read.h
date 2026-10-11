/*
 * This file is part of libdvdread.
 *
 * libdvdread is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libdvdread is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with libdvdread; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef LIBDVDREAD_VOB_READ_H
#define LIBDVDREAD_VOB_READ_H

#include <stdint.h>
#include <stddef.h>

#include "dvd_reader.h"
#include "ifo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A title set's title VOBs read sector by sector for ripping, on a reader
 * opened with DVDOpenFiles(): the title VOBs are the title set's
 * VTS_nn_1..9.VOB files joined as bytes (up to the first missing one); on
 * a disc image (the file system has image_open and the IFO has a place on
 * it) the image's sectors from where the IFO header puts them (the IFO's
 * sector + vtstt_vobs) to the end of the image.
 *
 * Each sector read is checked: one that is no MPEG-2 pack (pack_start_code
 * with the '01' marker) followed by a packet start code (the pack header is
 * 14 bytes plus pack_stuffing_length, the stuffing counted only when its
 * first byte is not zero) is left out as damaged. Damaged sectors one after
 * the other in reading order and of one kind make one damaged range; it is
 * reported once (dvd_report_t with nb_sectors and damage, level error) when
 * it ends: when a sector that is not part of it is read, or the caller ends
 * it (DVDEndTitleVOBDamage_rec()), and it is given back to the caller.
 *
 * CSS (DVD-Video, CSS): a packet of the video stream 0xE0, private stream 1
 * or MPEG audio 0xC0-0xC7 whose PES_scrambling_control is set is
 * descrambled with its title set's title key and the bits cleared. The key
 * is looked for when the title set's first scrambled sector is read (by any
 * of its readings; the reader keeps one key per title set): a title set
 * whose title VOBs lie inside those of a title set that has its key takes
 * that key (an image); otherwise libdvdcss cracks it from the scrambled
 * sectors from there on. The key is checked on the first sector after it
 * (within 8192) that can tell a right key from a wrong one: an AC-3 frame
 * whose sync information lies in the sector's clear part and which ends in
 * its scrambled part (its CRC), or an MPEG-2 sequence header in the clear
 * part whose sequence_extension start code lies in the scrambled part; none:
 * used unchecked. Without a key (none found, the check fails) the title
 * set's scrambled sectors cannot be read, and the key is not looked for
 * again. The steps are reported (table "CSS", without a file).
 */
typedef struct dvd_vob_reader_s dvd_vob_reader_t;

/** A damaged range of title VOB sectors: where it lies (the files of its
 *  first and last sector and the byte offsets there; "" and -1 when the
 *  file is not known) and what it is. */
typedef struct
{
  dvd_damage_t kind;
  uint32_t     sector;            /* its first title VOB sector */
  uint32_t     nb_sectors;        /* 0: no range */
  char         file[16];
  int64_t      offset;
  char         last_file[16];
  int64_t      last_offset;
} dvd_vob_damage_t;

/**
 * Opens title set title's title VOBs; ifo is its information file as
 * ifoOpen_rec() opened it (its header gives vtstt_vobs). NULL when the
 * title set has no title VOBs.
 */
DVDREAD_API dvd_vob_reader_t *DVDOpenTitleVOBs_rec( ifo_handle_t *ifo, int title );

/** The title VOBs' length in whole sectors. */
DVDREAD_API uint32_t DVDTitleVOBSectors_rec( const dvd_vob_reader_t * );

/**
 * Reads title VOB sector into buf (2048 bytes, descrambled): 1 the sector
 * is usable, 0 it is damaged and left out, -1 it cannot be read (past the
 * title VOBs, a read error), -2 it is scrambled and the title set has no
 * key. When this read ends a damaged range, the range is put in *ended
 * (nb_sectors 0: none); ended may be NULL.
 */
DVDREAD_API int DVDReadTitleVOBSector_rec( dvd_vob_reader_t *, uint32_t sector, uint8_t *buf,
                                           dvd_vob_damage_t *ended );

/** Ends the damaged range being read, if any (the reading stops or goes
 *  elsewhere): reported and put in *ended (nb_sectors 0: none). */
DVDREAD_API void DVDEndTitleVOBDamage_rec( dvd_vob_reader_t *, dvd_vob_damage_t *ended );

/** Title set title's CSS title key: 1 found (into key), 0 not looked for
 *  yet, -1 none. */
DVDREAD_API int DVDTitleVOBKey_rec( dvd_reader_t *, int title, uint8_t key[5] );

/** Closes the title VOBs; a damaged range still open is reported. */
DVDREAD_API void DVDCloseTitleVOBs_rec( dvd_vob_reader_t * );

#ifdef __cplusplus
};
#endif
#endif /* LIBDVDREAD_VOB_READ_H */
