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

/* ---- cells ----
 *
 * A title set's program chain table as ifoRead_VTS_PGCIT_rec() read it,
 * handed to the reader (DVDKeepPGCIT_rec()); its cells (pgcn: the program
 * chain's search pointer index, cell: the cell's index, both from 0) are
 * walked and checked:
 *
 * - inside a cell the VOBU address map gives the next VOBU (a cell neither
 *   in an angle block nor interleaved; with the seamless-angle flag only
 *   when its first VOBUs and last VOBU follow each other by their
 *   navigation packs, checked once), else the navigation packs;
 * - a cell is rejected when a VOBU of it is known not to start one, it has
 *   no playback time, it lies outside the title VOBs, it is a cell of at
 *   most 1 s (unless a single VOBU right after the previous cell, without a
 *   cell command, not the next to last, that passes the full check) or of
 *   at most 5 s with a cell command, the only cell of its chain and a long
 *   run of tiny VOBUs of one size, or (the full test) fails the full check
 *   or its real length cannot be measured;
 * - the full check reads the navigation packs of its first and last VOBU,
 *   of every VOBU (or 50 evenly spread, or 50 at random: DVDSetCellCheck_rec())
 *   and their presentation times must be in order.
 */

typedef enum
{
    DVD_CHECK_ALL,                      /* every VOBU of the cell */
    DVD_CHECK_SPREAD,                   /* 50 VOBUs evenly spread (all of fewer) */
    DVD_CHECK_RANDOM,                   /* 50 VOBUs at random (all of fewer) */
} dvd_cell_check_t;

/** Title set title's program chain table: the reader keeps it (frees it
 *  when it closes). */
DVDREAD_API int DVDKeepPGCIT_rec( dvd_reader_t *, int title, pgcit_rec_t *pgcit );

/** Which VOBUs the full cell check reads; seed: the random sample's. */
DVDREAD_API void DVDSetCellCheck_rec( dvd_reader_t *, dvd_cell_check_t mode, uint64_t seed );

/** The next VOBU and the length of the VOBU at sector in the cell (*next
 *  DVD_VOBU_END after its last): 1, 0 none. */
DVDREAD_API int DVDCellNextVobu_rec( dvd_vob_reader_t *, int pgcn, int cell, uint32_t sector, uint32_t *next,
                                     uint32_t *len );

/** The same by the navigation packs alone (recorded, else read). */
DVDREAD_API int DVDCellNavNextVobu_rec( dvd_vob_reader_t *, int pgcn, int cell, uint32_t sector, uint32_t *next,
                                        uint32_t *len );

/** The seamless-angle cell's first VOBUs and last VOBU follow each other by
 *  their navigation packs (found once per cell). */
DVDREAD_API int DVDCellSeamlessAngleOk_rec( dvd_vob_reader_t *, int pgcn, int cell );

/** The presentation start and end of the VOBU at sector of the cell (read
 *  when not recorded; without read_anyway only when the cell passes its
 *  checks): 1, 0 none. */
DVDREAD_API int DVDCellVobuTimes_rec( dvd_vob_reader_t *, int pgcn, int cell, uint32_t sector, uint32_t *s_ptm,
                                      uint32_t *e_ptm, int read_anyway );

/** The cell's real length in 90 kHz ticks: its last VOBU's end against its
 *  first VOBU's start: 1, 0 not measurable. */
DVDREAD_API int DVDCellDuration_rec( dvd_vob_reader_t *, int pgcn, int cell, uint64_t *duration, int read_anyway );

/** The cell is rejected (see above; quick: without the full check and the
 *  measure); prev: the index of the cell played before it, -1 none. */
DVDREAD_API int DVDCellRejected_rec( dvd_vob_reader_t *, int pgcn, int cell, int quick, int prev );

/** The cell fails the full check. */
DVDREAD_API int DVDCellCheckFailed_rec( dvd_vob_reader_t *, int pgcn, int cell );

/** From VOBU start from to VOBU start to interleaved unit by interleaved
 *  unit, each unit's first VOBU starting it. */
DVDREAD_API int DVDIlvuWalk_rec( dvd_vob_reader_t *, uint32_t from, uint32_t to );

/**
 * The program chain check: the chain is broken or a decoy, the reason, -1
 * when it passes. 0 no cells; with title VOBs shared with another title set
 * (shared, the caller's answer from the title table): 1 a new timeline at
 * every cell after the first and 12 cells or more, 2 the same, out of
 * order on the disc and 7 cells or more (the last cell left out of the
 * order when rejected, or a short cell of another VOB starting it); 4
 * cells or more: 3 no playback time in the cells not rejected (quick), 4
 * fewer than 50 sectors per second; 5 a program map entry 0 or past the
 * cells, 6 entries out of order; programs 2 on (3 programs or more), cells
 * played seamlessly without cell commands: 7 angle blocks joined to a cell
 * whose places do not follow each other, 8 a plain cell and an interleaved
 * one that do not follow each other nor by interleaved units, 9 (shared)
 * an interleaved angle block whose cells' places disagree.
 */
DVDREAD_API int DVDPgcRejected_rec( dvd_vob_reader_t *, int pgcn, int shared );

/* ---- a title's cell list ---- */

/** The cell of angle (from 0; the title has angles) of the angle block
 *  starting at cell (a cell in no block: itself); -1 when the block is
 *  broken (reported). */
DVDREAD_API int DVDAngleCell_rec( dvd_vob_reader_t *, int pgcn, int cell, int angle, int angles );

/** Cells first..last of the chain, one per angle block (its cell of angle),
 *  into cells (room for the chain's cells), *nb of them: 1 done, 0 an angle
 *  block is broken (the cells before it given). */
DVDREAD_API int DVDCellList_rec( dvd_vob_reader_t *, int pgcn, int first, int last, int angle, int angles,
                                 int *cells, int *nb );

/**
 * CellWalk: the cells of the chain the navigation played (played: their
 * numbers from 1, in playing order) as one run of consecutive cells. Runs
 * at the start, then at the end, without a cell accepted (quick check) and
 * 5 seconds are dropped, then the shorter end run while two remain (a tie
 * keeps the first); the run left is the cell list (DVDCellList_rec(), its
 * first and last cell index in *first / *last). 1 done, 0 not possible
 * (nothing played, a run outside the chain, a broken angle block).
 */
DVDREAD_API int DVDCellWalk_rec( dvd_vob_reader_t *, int pgcn, const uint8_t *played, int nb_played, int angle,
                                 int angles, int *cells, int *nb, int *first, int *last );

/** What CellTrim chose besides the cells. */
typedef struct
{
  int first, last;                    /* the run's first and last cell index, -1: none chosen */
  int fake_percent;                   /* the cells were left out as fake cells making up this much of the
                                         chain's declared length (100: no length at all), -1: not so */
} dvd_cell_trim_t;

/**
 * CellTrim: the chain's cells without the navigation, decoys at the start
 * and end trimmed (nb_chapters: the title's chapters; first_cell /
 * middle_cell: the cells its first and middle chapter start at, -1 none).
 * First way, 4 chapters or fewer: a chain of 3-5 cells whose first and
 * last are rejected filler around one VOB keeps its inner cells, one of 5
 * cells or more with rejected ends and exactly one run of 10 s or more
 * between cell commands that jump away keeps that run (its filler ends
 * dropped). Else, with fewer than 5 chapters, the cells between the first
 * and the last that pass the full test; with 5 or more, the run around the
 * middle chapter between cell commands that jump away, widened back over
 * runs of 30 s or more before the program after its start, trimmed of
 * cells failing the full check and of short cells starting a new timeline
 * at its end, and of filler at both ends. A run whose cells in disc order
 * are 26 % or more short of the chain's declared length is fake: no cells.
 * 1 done (no cells when none are left), 0 failed.
 */
DVDREAD_API int DVDCellTrim_rec( dvd_vob_reader_t *, int pgcn, int nb_chapters, int first_cell, int middle_cell,
                                 int angle, int angles, int *cells, int *nb, dvd_cell_trim_t *result );

/* ---- a title's segments and chapter places ---- */

/** An extent of title VOB sectors: count sectors from sector, at logical
 *  (the segment's own sector count before it). */
typedef struct
{
  uint32_t logical, sector, count;
} dvd_extent_t;

/** A run of a title on one timeline: its extents in playing order, the
 *  cell of its last VOBU, its cells as runs of consecutive cells (from 1),
 *  its size in bytes. */
typedef struct
{
  dvd_extent_t *extents;
  int           nb_extents, alloc_extents;
  int           last_cell;
  uint16_t    (*runs)[2];
  int           nb_runs, alloc_runs;
  int64_t       size;
} dvd_segment_t;

/** A title's segments and what was found building them. */
typedef struct
{
  dvd_segment_t *segments;
  int            nb_segments, alloc_segments;
  int            counted;             /* the VOBUs to read one by one were counted: */
  unsigned       cells_to_scan, vobus_to_scan;
  uint32_t      *nav_invalid;         /* seconds from the title's start where the navigation is invalid inside a
                                         cell (a VOBU says the cell ends early), in the order found */
  int            nb_nav_invalid, alloc_nav_invalid;
} dvd_title_segments_t;

/**
 * The cells' VOBUs (cells: the title's cell list, indexes in the chain) as
 * extents of the title VOBs, in segments: a new segment at a cell that
 * starts a new timeline (stc_discontinuity; also when a cell skipped between
 * two of the list does, outside a block, or the list goes back); contiguous
 * VOBUs merged; interleaved cells walked unit by unit (the VOBU address map
 * gives a unit's last VOBU). First the VOBUs that must be read one by one
 * (angle blocks, interleaved cells, a cell's last VOBU) are counted. 1 done,
 * 0 failed (a VOBU outside the title VOBs or not found, presentation times
 * out of order where the navigation is invalid; what was found so far is
 * kept), -1 no memory. Free with DVDFreeTitleSegments_rec().
 */
DVDREAD_API int DVDTitleSegments_rec( dvd_vob_reader_t *, int pgcn, const int *cells, int nb_cells,
                                      dvd_title_segments_t *out );
DVDREAD_API void DVDFreeTitleSegments_rec( dvd_title_segments_t * );

/** A chapter: its program's entry cell (in; -1 none) and where it is in the
 *  segments (out; segment -1: not placed): the byte offset in the segment
 *  and its time (90 kHz) from the title's start to the end of its cell's
 *  first VOBU. */
typedef struct
{
  int     cell;
  int     segment;
  int64_t offset;
  int64_t time;
} dvd_chapter_place_t;

/** The chapters' places in the segments (a chapter before the previous one
 *  placed is not placed). */
DVDREAD_API void DVDChapterPlaces_rec( dvd_vob_reader_t *, int pgcn, const dvd_title_segments_t *segs,
                                       dvd_chapter_place_t *chapters, int nb );

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

DVDREAD_API int DVDPackPacket_rec( const uint8_t *sector, int *off, dvd_pack_packet_t *p );

/**
 * The stream probe of a title: one of its segments (extents: the segment's
 * extents, as DVDTitleSegments_rec() gives them) read from its start, the
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

DVDREAD_API int DVDProbeTitleStreams_rec( dvd_vob_reader_t *, const dvd_extent_t *extents, int nb_extents,
                                          dvd_probe_audio_t *audio, int nb_audio, int *captions,
                                          uint32_t *failed_block );

/** Closes the title VOBs; a damaged range still open is reported. */
DVDREAD_API void DVDCloseTitleVOBs_rec( dvd_vob_reader_t * );

#ifdef __cplusplus
};
#endif
#endif /* LIBDVDREAD_VOB_READ_H */
