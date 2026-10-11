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
 * again. The steps are reported (table "CSS", without a file).
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

/** Where title set title's title VOBs start on the image (absolute sector,
 *  decided once, see above); ifo: its ifoOpen_rec() handle. 0 on a folder or
 *  without title VOBs. */
DVDREAD_API int64_t DVDTitleVOBStart_rec( ifo_handle_t *ifo, int title );

/**
 * The file view for the segment read next (n extents: title VOB sectors
 * from sectors[i], counts[i] of them; replaces the previous segment's): 1
 * it is used when a read fails, 0 not (a folder, no file view, or: the
 * first and the last extent must lie in both, their first two sectors and,
 * of 3 or more, their last two must read the same through the file view
 * and at the start place, and every extent must lie in the file view).
 */
DVDREAD_API int DVDUseTitleVOBView_rec( dvd_vob_reader_t *, const uint32_t *sectors, const uint32_t *counts,
                                        int n );

/** Title set title's CSS title key: 1 found (into key), 0 not looked for
 *  yet, -1 none. */
DVDREAD_API int DVDTitleVOBKey_rec( dvd_reader_t *, int title, uint8_t key[5] );

/* ---- VOBUs ----
 *
 * A VOBU starts with a navigation pack (DVD-Video; nav_types.h pci_t /
 * dsi_t): its DSI gives the VOBU's length (dsi_gi.vobu_ea), the end of its
 * interleaved unit (sml_pbi.ilvu_ea), its category (sml_pbi.category) and
 * where the cell's next VOBU starts (vobu_sri.next_vobu); its PCI its
 * presentation start and end (pci_gi.vobu_s_ptm / vobu_e_ptm). They are read
 * with navRead_PCI_ctx() / navRead_DSI_ctx(), whose checks report.
 *
 * VOBU records: what a navigation pack says is recorded per sector the first
 * time it is read, a sector that is no navigation pack (or cannot be read)
 * as such; later questions are answered from the record. Title sets whose
 * title VOBs lie at a known place of an image keep their records together
 * by absolute sector (title sets may share title VOBs), the others per title
 * set.
 *
 * VOBU starts: the title set's VOBU address map (VTS_VOBU_ADMAP, handed in
 * with DVDSetVobuMap_rec()) answers "is this a VOBU start" and "where is the
 * next VOBU" while it is trusted; at the first sector that should start a
 * VOBU and is missing from it, it is not trusted any more (reported: the
 * VOBs must be scanned) and the maps of two or more title sets sharing these
 * title VOBs, when they agree, else the navigation packs answer.
 */

/** What a VOBU's navigation pack says. */
typedef struct
{
  uint32_t vobu_s_ptm, vobu_e_ptm, vobu_ea, ilvu_ea, next_vobu;
  uint16_t category;
} dvd_vobu_nav_t;

/** A VOBU record: key = the sector (+ the title VOBs' start on an image);
 *  next = sml_pbi category & 0x6000 << 16 | distance to the next VOBU
 *  (0x10000000: none), DVD_VOBU_NO_NAV: the sector is no navigation pack. */
typedef struct dvd_vobu_rec_s
{
  uint32_t key;
  uint16_t vobu_ea;
  uint16_t ilvu_ea;
  uint32_t next;
  uint32_t vobu_s_ptm, vobu_e_ptm;
} dvd_vobu_rec_t;

#define DVD_VOBU_NO_NAV 0xffffffffu     /* dvd_vobu_rec_t.next: the sector is no navigation pack */
#define DVD_VOBU_END    0xfffffffeu     /* a next VOBU: none, the cell ends */
#define DVD_VOBU_SELF   0xfffffffdu     /* a next VOBU: the VOBU names itself */

/** A navigation pack exactly as DVD-Video lays it out: pack header, system
 *  header, PCI and DSI packets. */
DVDREAD_API int DVDIsNavPack_rec( const uint8_t *sector );

/** Title set title's VOBU address map (its VOBU starts, sorted, each once;
 *  copied) and whether it is trusted. */
DVDREAD_API int DVDSetVobuMap_rec( dvd_reader_t *, int title, const uint32_t *starts, int nb, int trusted );

/** Whether title set title's VOBU address map is trusted. */
DVDREAD_API int DVDVobuMapTrusted_rec( dvd_reader_t *, int title );

/** The navigation pack of title VOB sector, read (a sector that is no
 *  navigation pack is recorded as such; one whose headers were blanked, the
 *  first 32 bytes 0xff, both nv_pck_lbn the sector, is taken as a VOBU of
 *  0.5 s): 1, 0 none. */
DVDREAD_API int DVDReadVobuNav_rec( dvd_vob_reader_t *, uint32_t sector, dvd_vobu_nav_t * );

/** A sector some other reading read (the navigation): recorded as the VOBU
 *  its navigation pack says, else as no navigation pack. < 0 on no memory. */
DVDREAD_API int DVDNoteVobuSector_rec( dvd_vob_reader_t *, uint32_t sector, const uint8_t *buf );

/** The VOBU at sector from its navigation pack recorded: *next its next
 *  VOBU (DVD_VOBU_END, DVD_VOBU_SELF: allow_self), *len its sectors; a
 *  cell ending inside an interleaved unit goes on to the unit's next VOBU
 *  (one ending before its own VOBU: reported, worked around). 0 when the
 *  VOBU names itself and allow_self is 0, or the unit's next VOBU cannot be
 *  read. */
DVDREAD_API int DVDRegisterVobu_rec( dvd_vob_reader_t *, const dvd_vobu_nav_t *, uint32_t sector, int allow_self,
                                     uint32_t *next, uint32_t *len );

/** The record of sector (NULL: none); *next_key (may be NULL): the next
 *  recorded sector, -1 none. */
DVDREAD_API const dvd_vobu_rec_t *DVDFindVobu_rec( dvd_vob_reader_t *, uint32_t sector, int64_t *next_key );

/** Records sector as no navigation pack. < 0 on no memory. */
DVDREAD_API int DVDMarkVobuBad_rec( dvd_vob_reader_t *, uint32_t sector );

/** Whether sector starts a VOBU (see above). */
DVDREAD_API int DVDVobuStart_rec( dvd_vob_reader_t *, uint32_t sector );

/** The VOBU after the one at sector: 1 (*next), 0 not known. */
DVDREAD_API int DVDNextVobu_rec( dvd_vob_reader_t *, uint32_t sector, uint32_t *next );

/** Whether sector is no VOBU start of the title VOBs (past them, recorded
 *  or read as no navigation pack). */
DVDREAD_API int DVDVobuBad_rec( dvd_vob_reader_t *, uint32_t sector );

/** Where title set title's menu VOBs start on the image (absolute sector,
 *  title 0: the video manager's): decided once as the title VOBs' start,
 *  from vtsm_vobs / vmgm_vobs and the file system's VTS_nn_0.VOB /
 *  VIDEO_TS.VOB; neither place starting with a VOB's first navigation
 *  pack: the IFO header's. 0 on a folder or without menu VOBs. */
DVDREAD_API int64_t DVDMenuVOBStart_rec( ifo_handle_t *ifo, int title );

/** Closes the title VOBs; a damaged range still open is reported. */
DVDREAD_API void DVDCloseTitleVOBs_rec( dvd_vob_reader_t * );

#ifdef __cplusplus
};
#endif
#endif /* LIBDVDREAD_VOB_READ_H */
