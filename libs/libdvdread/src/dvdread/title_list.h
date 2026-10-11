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

#ifndef LIBDVDREAD_TITLE_LIST_H
#define LIBDVDREAD_TITLE_LIST_H

#include <stdint.h>

#include "disc_read.h"
#include "vob_read.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The title list of a disc (dvd_disc_rec_t), from its structure and the
 * navigation scan's recordings (libdvdnav's dvdnav_scan_run(), handed in).
 *
 * Order. With the scan's cell lists in use, the titles the navigation
 * reached come first, then the others, each group in the order of the title
 * table (TT_SRPT); otherwise the title table's order.
 *
 * A title table entry. Left out: an entry without a title set, a VTS title
 * number or chapters; one whose title set could not be read; one whose
 * title set is suspicious (its title VOBs share sectors with another title
 * set's, it has more than 80 program chains, and every chain of two or more
 * cells either starts a new timeline at each cell after the first or fails
 * the program chain check), and, with the scan's cell lists in use, one the
 * navigation did not reach. Its chapters are the title's PTT entries
 * (VTS_PTT_SRPT), each resolved to its program's entry cell; every program
 * chain the chapters use gives a title of its own ("4", then "4/1", ...); a
 * chain failing the program chain check that the navigation did not play is
 * left out.
 *
 * A program chain's title. Its cells by the cell mode: walk (the cells the
 * navigation played, DVDCellWalk_rec()), trim (DVDCellTrim_rec()), walk
 * then trim for titles the navigation did not reach, or auto (walk when the
 * scan has results, falling back to trim when the navigation's cells do not
 * work); one cell per angle block (the title's angle). Its length: declared
 * (the cells' playback times) and real (DVDCellDuration_rec()); a title whose
 * real length differs from the declared one by more than 300 s and 30 % is
 * a fake; so is one not reached by the navigation whose cells on title VOBs
 * shared with another title set do not follow each other. Its sectors and
 * chapters: DVDTitleSegments_rec(), DVDChapterPlaces_rec() (chapters that
 * cannot be placed are dropped; a first one that cannot be placed is put at
 * the start when the next one is not there). An angle block gives one more
 * title per further angle (at most 8); the main angle is the one the
 * navigation played first in the program chain (its scan recording), else
 * angle 1 (the angle register's start value). A title of a program chain whose
 * playback time or declared length is shorter than the minimum length is
 * listed as short.
 *
 * Its streams: audio, those the program chain's audio_control lists in its
 * order, then the title set's others; sub-pictures, those subp_control
 * lists, each by its 4:3 stream number on a 4:3 title set and its wide one on
 * a 16:9 title set (a single stream whose wide number is 1 and letterbox
 * number 0 also as stream 0), their canvas the title set's video frame. A
 * title set with more than 8 audio or 32 sub-picture streams gives no title.
 *
 * Duplicates. A title playing an earlier one's sectors with no more streams
 * and chapters is left out; the search starts over after each.
 *
 * What the list finds and decides it says in the disc reader's log as it
 * happens (decisions and steps a user should see as notices).
 */

/** A recording of the navigation scan: the cells (from 1) the navigation
 *  played in title (title table number, low 8 bits) / program chain pgc, and
 *  the name of the path that played them; the angles (the angle register's,
 *  SPRM 3) the navigation played in its angle blocks, in the order played
 *  (this recording's, then those of the other recordings of the title /
 *  program chain). */
typedef struct
{
  char     name[16];
  uint8_t  title, pgc;
  uint8_t  angles[10];                /* from 1; 0 ends (none: no angle block played) */
  int      nb_cells;
  uint8_t *cells;
} dvd_nav_cells_t;

/** A title's audio or sub-picture stream: its stream number and the title
 *  set's attributes it is described by (vts_audio_attr / vts_subp_attr
 *  index). */
typedef struct
{
  uint8_t stream, attr;
} dvd_stream_ref_t;

#define DVD_TITLE_CHAPTER_NONE (-1)

typedef struct
{
  uint16_t pgcn, pgn;                 /* the PTT entry */
  int      cell;                      /* its program's entry cell (index in the program chain), -1 none */
  int      segment;                   /* DVD_TITLE_CHAPTER_NONE: not placed */
  int64_t  offset;                    /* bytes in the segment */
  int64_t  time;                      /* 90 kHz: from the title's start to the end of its cell's first VOBU */
} dvd_title_chapter_t;

typedef struct
{
  char                   name[24];    /* the title table entry ("4"), with its program chain ("4/1") */
  int                    title;       /* the title table entry, from 0 */
  uint8_t                pb_ty, vtsn, vts_ttn;
  uint8_t                nr_of_angles, angle;   /* angle from 0 */
  uint8_t                main_angle;  /* from 1: the angle a player plays (the navigation's, else angle 1, the
                                         angle register's start value); may be one beyond nr_of_angles */
  int                    pgcn;        /* its program chain's search pointer index, from 0 */
  const dvd_nav_cells_t *scan;        /* the navigation's cells of this title / program chain, NULL none */
  dvd_title_chapter_t   *chapters;
  int                    nb_chapters;
  int                   *cells;       /* the cells played, indexes in the program chain */
  int                    nb_cells, alloc_cells;
  uint32_t               declared;    /* seconds: the cells' playback times */
  uint32_t               measured;    /* seconds: the cells' VOBUs' presentation times */
  dvd_title_segments_t   segs;        /* its segments (DVDTitleSegments_rec()) */
  char                 **cell_maps;   /* per segment its cell runs as text: "1-3", "(1-3,5)" */
  int                    built;
  int                    nav_invalid; /* places with invalid navigation found */
  int                    short_title; /* shorter than the minimum length: listed, not selected */
  dvd_stream_ref_t       audio[8];    /* in the program chain's audio_control order, then the title set's others */
  int                    nb_audio;
  uint8_t                audio_listed;   /* the stream numbers audio_control lists (bit per stream) */
  dvd_stream_ref_t       subp[33];    /* in the program chain's subp_control order */
  int                    nb_subp;
  uint16_t               subp_width, subp_height;   /* the sub-pictures' canvas */
  /* the stream probe's (DVDTitleStreams_rec()) */
  int                    captions;    /* closed captions in the video */
  uint8_t                audio_dropped;  /* audio[] entries left out (bit per entry) */
} dvd_title_t;

typedef struct
{
  dvd_title_t **titles;
  int           nb_titles;
} dvd_title_list_t;

enum { DVD_CELL_MODE_AUTO_REC, DVD_CELL_MODE_WALK_REC, DVD_CELL_MODE_TRIM_REC, DVD_CELL_MODE_WALK_TRIM_REC };

typedef struct
{
  int      cell_mode;                 /* DVD_CELL_MODE_*_REC */
  int      cell_check;                /* dvd_cell_check_t: the VOBUs a full cell check reads */
  uint64_t seed;                      /* of a random cell check */
  int      min_length;                /* seconds */
} dvd_title_options_t;

/**
 * The title list (see above). scan: the navigation's recordings (nb_scan of
 * them, 0: the scan failed or did not run); they must outlive the list.
 * 0, -1 no memory (*out empty). Free with DVDFreeTitleList_rec().
 */
DVDREAD_API int DVDTitleList_rec( dvd_disc_rec_t *, const dvd_nav_cells_t *scan, int nb_scan,
                                  const dvd_title_options_t *, dvd_title_list_t *out );
DVDREAD_API void DVDFreeTitle_rec( dvd_title_t * );

/**
 * The stream probe of title t (DVDProbeTitleStreams_rec() on its largest
 * segment, the first of the largest, on its title set's own reading): its
 * captions and the audio streams it leaves out (t->captions,
 * t->audio_dropped), each audio stream by its stream number, its attributes'
 * coding mode and whether audio_control lists it by its place in the title's
 * audio list (audio_control's streams come first). Left out: an unknown
 * coding mode, no data in the first 48 MiB when not listed, no data in any
 * zone the probe reads when listed. What it finds is said in the log. 1 done,
 * -1 no memory, -2 a sector cannot be read, -3 it is scrambled without a
 * title key (said; the title cannot be used).
 */
DVDREAD_API int DVDTitleStreams_rec( dvd_disc_rec_t *, dvd_title_t *t );
DVDREAD_API void DVDFreeTitleList_rec( dvd_title_list_t * );

#ifdef __cplusplus
};
#endif
#endif /* LIBDVDREAD_TITLE_LIST_H */
