/*
 * DVD-Video: the VOBUs of a title set's title VOBs: their records, the
 * VOBU address map, where VOBUs start (dvd_vobu.c).
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

#ifndef DISC_DVD_DVD_VOBU_H
#define DISC_DVD_DVD_VOBU_H

#include <stdint.h>

struct DVDDisc;
typedef struct DVDCells DVDCells;

/* ---- VOBUs ----
 *
 * A VOBU starts with a navigation pack (DVD-Video; nav_types.h pci_t /
 * dsi_t): its DSI gives the VOBU's length (dsi_gi.vobu_ea), the end of its
 * interleaved unit (sml_pbi.ilvu_ea), its category (sml_pbi.category) and
 * where the cell's next VOBU starts (vobu_sri.next_vobu); its PCI its
 * presentation start and end (pci_gi.vobu_s_ptm / vobu_e_ptm). They are read
 * with libdvdread's navRead_PCI_ctx() / navRead_DSI_ctx(), whose checks
 * report, from the title VOBs as they are (DVDReadTitleVOBRaw_rec()).
 *
 * VOBU records: what a navigation pack says is recorded per sector the first
 * time it is read, a sector that is no navigation pack (or cannot be read)
 * as such; later questions are answered from the record. Title sets whose
 * title VOBs lie at a known place of an image keep their records together
 * by absolute sector (title sets may share title VOBs), the others per title
 * set.
 *
 * VOBU starts: the title set's VOBU address map (VTS_VOBU_ADMAP, read and
 * checked against the cells by libdvdread: dvd_ifo_rec_t vobu_map /
 * map_trusted, taken at the disc's open, ff_dvd_cells_open()) answers "is this a VOBU start" and "where is the
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
int DVDIsNavPack_rec( const uint8_t *sector );

/** Whether title set title's VOBU address map is trusted (a title set
 *  without one, or with no information file: trusted, empty). */
int DVDVobuMapTrusted_rec( const struct DVDDisc *, int title );

/** The title set's VOBU address map (its VOBU starts, ascending, *nb of
 *  them): 1 while it is trusted, else 0. */
int DVDVobuMap_rec( const DVDCells *, const uint32_t **map, int *nb );

/** The navigation pack of title VOB sector, read (a sector that is no
 *  navigation pack is recorded as such; one whose headers were blanked, the
 *  first 32 bytes 0xff, both nv_pck_lbn the sector, is taken as a VOBU of
 *  0.5 s): 1, 0 none. */
int DVDReadVobuNav_rec( DVDCells *, uint32_t sector, dvd_vobu_nav_t * );

/** A sector some other reading read (the navigation): recorded as the VOBU
 *  its navigation pack says, else as no navigation pack. < 0 on no memory. */
int DVDNoteVobuSector_rec( DVDCells *, uint32_t sector, const uint8_t *buf );

/** The VOBU at sector from its navigation pack recorded: *next its next
 *  VOBU (DVD_VOBU_END, DVD_VOBU_SELF: allow_self), *len its sectors; a
 *  cell ending inside an interleaved unit goes on to the unit's next VOBU
 *  (one ending before its own VOBU: reported, worked around). 0 when the
 *  VOBU names itself and allow_self is 0, or the unit's next VOBU cannot be
 *  read. */
int DVDRegisterVobu_rec( DVDCells *, const dvd_vobu_nav_t *, uint32_t sector, int allow_self,
                         uint32_t *next, uint32_t *len );

/** The record of sector (NULL: none); *next_key (may be NULL): the next
 *  recorded sector, -1 none. */
const dvd_vobu_rec_t *DVDFindVobu_rec( DVDCells *, uint32_t sector, int64_t *next_key );

/** Records sector as no navigation pack. < 0 on no memory. */
int DVDMarkVobuBad_rec( DVDCells *, uint32_t sector );

/** Whether sector starts a VOBU (see above). */
int DVDVobuStart_rec( DVDCells *, uint32_t sector );

/** The VOBU after the one at sector: 1 (*next), 0 not known. */
int DVDNextVobu_rec( DVDCells *, uint32_t sector, uint32_t *next );

/** Whether sector is no VOBU start of the title VOBs (past them, recorded
 *  or read as no navigation pack). */
int DVDVobuBad_rec( DVDCells *, uint32_t sector );

/** VOBU records, sorted by key (one per key, the first kept). */
typedef struct DVDVobuStore {
  dvd_vobu_rec_t *recs;
  int             nb, alloc;
} DVDVobuStore;

#endif /* DISC_DVD_DVD_VOBU_H */
