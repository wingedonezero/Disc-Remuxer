/*
 * This file is part of libdvdnav, a DVD navigation library.
 *
 * libdvdnav is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libdvdnav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with libdvdnav; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef LIBDVDNAV_SCAN_H
#define LIBDVDNAV_SCAN_H

#include <stdint.h>

#include <dvdnav/dvdnav.h>
#include <dvdread/disc_read.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The navigation scan: the disc's navigation played the way a viewer would
 * play it, per region the disc allows, from the disc's start and from every
 * title, every menu button reachable from a menu's starting button pressed,
 * and the cells each path plays recorded per title and program chain. The
 * results name the recordings ("A1", ...) and give each title / program
 * chain its cells.
 *
 * The disc's structure comes from libdvdread (dvd_disc_rec_t: the title
 * table, the program chains, the regions, where the title VOBs start); every
 * title-VOB sector the navigators read is recorded as a VOBU in its title
 * set's own reading (DVDDiscTitleVOBs_rec(), DVDNoteVobuSector_rec()).
 *
 * Navigators. The first is the one the application opened on the disc for
 * the scan (dvdnav_scan_run()); it gets no read-ahead, navigation-only
 * reading, title play following the title's pre-command jumps and the C
 * library's rand() sequence (glibc, seed 1) for the Rnd operation; every
 * other one is a copy of an earlier one (dvdnav_dup()), kept to the end.
 * Stepping one returns its next event; finite stills and waits are skipped
 * right away, an endless still is returned again until a button is
 * pressed. The application hands every VOB sector its navigators read to
 * dvdnav_scan_sector() (its file callbacks see them).
 *
 * Reads. Every VOB sector the navigators read is looked at once (later reads
 * of it come from the first): a NAV pack of a menu VOB, or of title VOBs with
 * highlight information, keeps its PCI (the menus' buttons); a title-VOB
 * sector of a title set records its VOBU.
 *
 * Paths. A stack of navigator states, each with a key (region, menu / title
 * set, NAV pack sector or start kind, button or title): a state is played
 * until it stops, reaches an endless still, a menu it has explored before,
 * 5 hours of playback time or 4096 cell changes. While it plays, the cells
 * of each title / program chain are collected (an angle block counts
 * whole); a menu whose buttons have been highlighted for 2 seconds after
 * 8.2 seconds of playback, or an endless still with buttons, has its
 * reachable buttons pressed, each in a copy pushed on the stack. Collected
 * cells: a sequence that runs through 1..n from any cell is turned to 1..n;
 * a stretch repeated at the start or end is kept once. Titles are started
 * from a copy of a navigator that stopped or played a title past 8.2 s (not
 * on discs with more than 96 titles, or 16 and more when title sets share
 * title VOBs).
 *
 * Results. The recordings are sorted by title, program chain, length
 * (longest first), then by how many steps from a start their path lies (a
 * title start counts 1000) and by key; the first of each title / program
 * chain is kept. A region pass after the first one runs only when the
 * navigation read the region register (SPRM 20). Any failure fails the
 * scan.
 */
typedef struct dvdnav_scan_s dvdnav_scan_t;

/** One recording: the cells (from 1) the navigation played in title (title
 *  table number, low 8 bits) / program chain pgc (low 8 bits), and the name
 *  of the path that played them. */
typedef struct
{
  char     name[16];
  uint8_t  title, pgc;
  int      nb_cells;
  uint8_t *cells;
} dvdnav_scan_result_t;

typedef struct
{
  dvdnav_scan_result_t *results;          /* sorted by title, then pgc */
  int                   nb_results;
  uint8_t               entered[100];     /* per title set: the navigation played one of its cells (also
                                             when the scan failed) */
} dvdnav_scan_results_t;

/**
 * A scan of disc. Its lines go to logcb (with priv; may be NULL); trace
 * (may be NULL) receives every navigator command, event, sector read and
 * path line. NULL on no memory.
 */
DVDNAV_API dvdnav_scan_t *dvdnav_scan_new( dvd_disc_rec_t *disc, void *priv, const dvdnav_logger_cb *logcb,
                                           void (*trace)( void *opaque, const char *line ), void *opaque );

/**
 * A VOB sector a navigator of the scan read: of information file set (0: the
 * VMG), its menu VOB or its title VOBs, sector relative to them. < 0 makes
 * the read fail.
 */
DVDNAV_API int dvdnav_scan_sector( dvdnav_scan_t *, int set, int menu, uint32_t sector, const uint8_t *data );

/**
 * Runs the scan on nav (the navigator opened on the disc for it, its reads
 * handed to dvdnav_scan_sector(); the scan closes it). 0 with *out, -1 the
 * scan failed, -2 no memory. Free *out with dvdnav_scan_free_results().
 */
DVDNAV_API int dvdnav_scan_run( dvdnav_scan_t *, dvdnav_t *nav, dvdnav_scan_results_t *out );

DVDNAV_API void dvdnav_scan_free_results( dvdnav_scan_results_t * );
DVDNAV_API void dvdnav_scan_close( dvdnav_scan_t * );

#ifdef __cplusplus
}
#endif
#endif /* LIBDVDNAV_SCAN_H */
