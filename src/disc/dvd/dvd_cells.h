/*
 * DVD-Video: a title set's cells walked and checked, a title's cell list,
 * segments and chapter places (dvd_cells.c).
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

#ifndef DISC_DVD_DVD_CELLS_H
#define DISC_DVD_DVD_CELLS_H

#include <stdint.h>

#include <dvdread/ifo_types.h>
#include <dvdread/vob_read.h>

struct DVDDisc;

/* ---- cells ----
 *
 * A title set's program chain table as libdvdread's ifoRead_VTS_PGCIT_rec()
 * read it (kept with the disc's structure); its cells (pgcn: the program
 * chain's search pointer index, cell: the cell's index, both from 0) are
 * walked and checked, on the title set's own reading of its title VOBs
 * (libdvdread, vob_read.h: its VOBU records, map and navigation packs):
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

/** A title set's cells as the DVD part walks and checks them: the disc
 *  (its log, the full check's mode and random sample), the title set's own
 *  reading of its title VOBs (libdvdread), its number and title VOBs'
 *  length, its program chain table and per cell what was found once
 *  (seamless angle checked / its result). One per title set, kept by the
 *  disc (DVDDisc.cells). */
typedef struct DVDCells {
  struct DVDDisc    *d;
  dvd_vob_reader_t  *r;
  int                title;
  uint32_t           blocks;
  const pgcit_rec_t *pgcit;
  uint8_t          **cell_state;
} DVDCells;

/** The cells' state of every title set with a program chain table (at the
 *  disc's open): 0, < 0 no memory. */
int  ff_dvd_cells_open( struct DVDDisc *d );
void ff_dvd_cells_close( struct DVDDisc *d );
/** Title set vts's cells on its own reading of the title VOBs; NULL when
 *  they cannot be read (libdvdread says why). */
DVDCells *ff_dvd_cells( struct DVDDisc *d, int vts );

/** Which VOBUs the full cell check reads; seed: the random sample's. */
void DVDSetCellCheck_rec( struct DVDDisc *, dvd_cell_check_t mode, uint64_t seed );

/** The next VOBU and the length of the VOBU at sector in the cell (*next
 *  DVD_VOBU_END after its last): 1, 0 none. */
int DVDCellNextVobu_rec( DVDCells *, int pgcn, int cell, uint32_t sector, uint32_t *next,
                         uint32_t *len );

/** The same by the navigation packs alone (recorded, else read). */
int DVDCellNavNextVobu_rec( DVDCells *, int pgcn, int cell, uint32_t sector, uint32_t *next,
                            uint32_t *len );

/** The seamless-angle cell's first VOBUs and last VOBU follow each other by
 *  their navigation packs (found once per cell). */
int DVDCellSeamlessAngleOk_rec( DVDCells *, int pgcn, int cell );

/** The presentation start and end of the VOBU at sector of the cell (read
 *  when not recorded; without read_anyway only when the cell passes its
 *  checks): 1, 0 none. */
int DVDCellVobuTimes_rec( DVDCells *, int pgcn, int cell, uint32_t sector, uint32_t *s_ptm,
                          uint32_t *e_ptm, int read_anyway );

/** The cell's real length in 90 kHz ticks: its last VOBU's end against its
 *  first VOBU's start: 1, 0 not measurable. */
int DVDCellDuration_rec( DVDCells *, int pgcn, int cell, uint64_t *duration, int read_anyway );

/** The cell is rejected (see above; quick: without the full check and the
 *  measure); prev: the index of the cell played before it, -1 none. */
int DVDCellRejected_rec( DVDCells *, int pgcn, int cell, int quick, int prev );

/** The cell fails the full check. */
int DVDCellCheckFailed_rec( DVDCells *, int pgcn, int cell );

/** From VOBU start from to VOBU start to interleaved unit by interleaved
 *  unit, each unit's first VOBU starting it. */
int DVDIlvuWalk_rec( DVDCells *, uint32_t from, uint32_t to );

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
int DVDPgcRejected_rec( DVDCells *, int pgcn, int shared );

/* ---- a title's cell list ---- */

/** The cell of angle (from 0; the title has angles) of the angle block
 *  starting at cell (a cell in no block: itself); -1 when the block is
 *  broken (reported). */
int DVDAngleCell_rec( DVDCells *, int pgcn, int cell, int angle, int angles );

/** Cells first..last of the chain, one per angle block (its cell of angle),
 *  into cells (room for the chain's cells), *nb of them: 1 done, 0 an angle
 *  block is broken (the cells before it given). */
int DVDCellList_rec( DVDCells *, int pgcn, int first, int last, int angle, int angles,
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
int DVDCellWalk_rec( DVDCells *, int pgcn, const uint8_t *played, int nb_played, int angle,
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
int DVDCellTrim_rec( DVDCells *, int pgcn, int nb_chapters, int first_cell, int middle_cell,
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
int DVDTitleSegments_rec( DVDCells *, int pgcn, const int *cells, int nb_cells,
                          dvd_title_segments_t *out );
void DVDFreeTitleSegments_rec( dvd_title_segments_t * );

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
void DVDChapterPlaces_rec( DVDCells *, int pgcn, const dvd_title_segments_t *segs,
                           dvd_chapter_place_t *chapters, int nb );

#endif /* DISC_DVD_DVD_CELLS_H */
