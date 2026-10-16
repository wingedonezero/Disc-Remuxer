/*
 * DVD-Video: title VOB sectors read for ripping (dvd_vob.c).
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

#ifndef DISC_DVD_DVD_VOB_H
#define DISC_DVD_DVD_VOB_H

#include <stdint.h>
#include <stddef.h>

#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_types.h>

struct DVDDisc;

/**
 * A title set's title VOBs read sector by sector for ripping, on a reader
 * opened with DVDOpenFiles(): the title VOBs are the title set's
 * VTS_nn_1..9.VOB files joined as bytes (up to the first missing one); on
 * a disc image (the file system has image_open and the IFO has a place on
 * it) the image's sectors from where they start to the end of the image.
 * Where they start is decided once per title set (DVDTitleVOBStart_rec()):
 * the IFO header's place (the IFO's sector + vtstt_vobs) and the file
 * system's VTS_nn_1.VOB when they agree or the file system has no single
 * place for that file; when they differ (reported as a finding of the IFO's
 * vtstt_vobs), the one whose first sector is a navigation pack giving its
 * own sector in the VOB as 0 (PCI and DSI nv_pck_lbn), the IFO header's
 * first; neither: the IFO header's.
 *
 * On an image the title VOBs can also be read through the VOB files (the
 * file view, DVDUseTitleVOBView_rec()): VTS_nn_1.VOB, _2, ... one after the
 * other from the start, until they hold the title VOBs' sectors by the IFO
 * (up to its BUP); each file must start where the previous one ends (by its
 * first piece on the image, or no place known) and be whole sectors;
 * VTS_nn_1 and _2 must be there unless the first holds them all; a missing
 * third or later one is taken from the start place to the end. A read
 * that fails at the start place is tried through the file view, then at
 * the start place once more.
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
 * again. The steps are said in the log as they happen.
 */
typedef struct dvd_vob_reader_s dvd_vob_reader_t;

/** A damaged range of title VOB sectors: where it lies (the files of its
 *  first and last sector and the byte offsets there, an image's files by
 *  their pieces on it (file_pieces, else their place when one piece); ""
 *  and -1 when the file is not known) and what it is. */
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
 * title set has no title VOBs or they cannot be opened (an error report of
 * its IFO saying why: a missing first VOB file, no place for them in the
 * IFO header or on the image, ...).
 */
dvd_vob_reader_t *DVDOpenTitleVOBs_rec( struct DVDDisc *, ifo_handle_t *ifo, int title );

/** The title VOBs' length in whole sectors. */
uint32_t DVDTitleVOBSectors_rec( const dvd_vob_reader_t * );

/** The VOB file holding title VOB sector (name into file, 16 bytes) and the
 *  byte offset of the sector there (a folder's files one after the other,
 *  an image's files by the pieces they lie in): 1, or 0 when not known
 *  (file "", *offset -1). */
int DVDTitleVOBSectorPlace_rec( const dvd_vob_reader_t *, uint32_t sector, char file[16],
                                int64_t *offset );

/**
 * Reads title VOB sector into buf (2048 bytes, descrambled): 1 the sector
 * is usable, 0 it is damaged and left out, -1 it cannot be read (past the
 * title VOBs, a read error), -2 it is scrambled and the title set has no
 * key. When this read ends a damaged range, the range is put in *ended
 * (nb_sectors 0: none); ended may be NULL.
 */
int DVDReadTitleVOBSector_rec( dvd_vob_reader_t *, uint32_t sector, uint8_t *buf,
                               dvd_vob_damage_t *ended );

/** Ends the damaged range being read, if any (the reading stops or goes
 *  elsewhere): reported and put in *ended (nb_sectors 0: none). */
void DVDEndTitleVOBDamage_rec( dvd_vob_reader_t *, dvd_vob_damage_t *ended );

/** Where title set title's title VOBs start on the image (absolute sector,
 *  decided once, see above); ifo: its ifoOpen_rec() handle. 0 on a folder or
 *  without title VOBs. */
int64_t DVDTitleVOBStart_rec( struct DVDDisc *, ifo_handle_t *ifo, int title );

/**
 * The file view for the segment read next (n extents: title VOB sectors
 * from sectors[i], counts[i] of them; replaces the previous segment's): 1
 * it is used when a read fails, 0 not (a folder, no file view, or: the
 * first and the last extent must lie in both, their first two sectors and,
 * of 3 or more, their last two must read the same through the file view
 * and at the start place, and every extent must lie in the file view).
 */
int DVDUseTitleVOBView_rec( dvd_vob_reader_t *, const uint32_t *sectors, const uint32_t *counts,
                            int n );

/** Title set title's CSS title key: 1 found (into key), 0 not looked for
 *  yet, -1 none. */
int DVDTitleVOBKey_rec( const struct DVDDisc *, int title, uint8_t key[5] );

/** Where title set title's menu VOBs start on the image (absolute sector,
 *  title 0: the video manager's): decided once as the title VOBs' start,
 *  from vtsm_vobs / vmgm_vobs and the file system's VTS_nn_0.VOB /
 *  VIDEO_TS.VOB; neither place starting with a VOB's first navigation
 *  pack: the IFO header's. 0 on a folder or without menu VOBs. */
int64_t DVDMenuVOBStart_rec( struct DVDDisc *, ifo_handle_t *ifo, int title );

/** Where an information file's menu VOBs (menu) or title VOBs start on an
 *  image, for libdvdread while it reads the disc's structure
 *  (DVDOpenDisc_rec()'s placement callback; opaque: the DVDDisc). */
int64_t ff_dvd_vob_place( void *opaque, ifo_handle_t *ifo, int title, int menu );

/* ---- for the VOBU and cell rules ---- */

/** The title VOBs' length in sectors (by the IFO / the files). */
uint32_t DVDTitleVOBBlocks_rec( const dvd_vob_reader_t * );

/** A title VOB sector's bytes as they are (no check, no descrambling: what
 *  the navigation packs are read from): 0, -1 when they cannot be read. */
int DVDReadTitleVOBRaw_rec( dvd_vob_reader_t *, uint32_t sector, uint8_t *buf );

/** Where title set title's title VOBs start on an image as decided so far
 *  (0: not known, a folder, none) and *blocks their length by the IFO (up
 *  to its BUP). */
int64_t DVDTitleVOBPlace_rec( const struct DVDDisc *, int title, uint32_t *blocks );

/** Closes the title VOBs; a damaged range still open is reported. */
void DVDCloseTitleVOBs_rec( dvd_vob_reader_t * );

#endif /* DISC_DVD_DVD_VOB_H */
