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

#ifndef LIBDVDREAD_DISC_READ_H
#define LIBDVDREAD_DISC_READ_H

#include <stdint.h>

#include "dvd_reader.h"
#include "ifo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A DVD-Video disc's structure read once, on a reader opened with
 * DVDOpenFiles() (the disc's own files; on a disc image the file system has
 * image_open), every step checked and every finding reported (pf_report):
 *
 * - the video manager (VIDEO_TS.IFO, ifoOpen_rec()): a VMG that cannot be
 *   read is no DVD-Video (NULL); more than 99 title sets: NULL;
 * - every title set's information file (ifoOpen_rec()); one that cannot be
 *   read stays out (its ifo NULL) and the disc goes on without it;
 * - the title table (TT_SRPT, at most 101 entries read; NULL when it
 *   cannot be read): each entry with a title set, a title number in it and
 *   chapters must name a title set the disc has (1..99 and at most
 *   vmg_nr_of_title_sets, else NULL); entries that give one title set
 *   different start sectors are reported;
 * - the text data (TXTDT_MGI), when the VMG has one: a tree of item nodes
 *   (kinds below 0x30) with their texts (kinds 0x30 and up). Texts are kept
 *   for the character sets 0x01 and 0x11 only, and of each text only the
 *   bytes 0x20-0x7A (others are dropped, reported); any other character
 *   set, or a structure that does not hold together, leaves the disc
 *   without text data. Its name: item 0x30, else 0x39, 0x38, 0x3A, 0x3B of
 *   the first node;
 * - where each title set starts (on an image its IFO's place, in a folder
 *   the sum of the sets before it by their headers), against the title
 *   table;
 * - each information file's tables: the menus' VOBU address map; of a title
 *   set its program chain table (ifoRead_VTS_PGCIT_rec(), kept here,
 *   checked against the cell address table), its chapter table as recorded
 *   and its title VOBs' VOBU address map. A VOBU address map that cannot be
 *   read, or a cell whose first sector or last VOBU start is not on it,
 *   leaves the title set's VOBU starts untrusted (map_trusted). A program
 *   chain table or chapter table that cannot be read leaves the title set
 *   out (the VMG's: NULL);
 * - the video manager's copy of each title set's attributes against the
 *   title set's own (ifoCheckVtsAttributes_rec()), and every information
 *   file read once more by ifoOpen() for its own checks.
 * Where the menu VOBs and the title VOBs start on an image is asked of the
 * application while the files are read (DVDOpenDisc_rec()'s place).
 */

/** A node of the text data: an item kind below 0x30 with the texts
 *  recorded under it. */
typedef struct dvd_text_node_s
{
  struct dvd_text_node_s  *parent;
  uint16_t                 offset;      /* its entry's offset in the language unit */
  uint8_t                  kind;
  int                      nb_texts;
  uint8_t                 *text_kinds;  /* sorted; the first text of a kind is kept */
  char                   **texts;
  int                      nb_children;
  struct dvd_text_node_s **children;
} dvd_text_node_t;

typedef struct
{
  char              language[4];        /* as recorded */
  uint16_t          title_offsets[99];  /* each title's node offset (0: none) */
  dvd_text_node_t  *root;
  dvd_text_node_t **nodes;              /* every node */
  int               nb_nodes;
} dvd_text_t;

/** One information file: the VMG (index 0) or a title set. */
typedef struct
{
  ifo_handle_t      *ifo;               /* ifoOpen_rec(); NULL: not read, the disc goes on without it */
  uint8_t            header[0x400];     /* its VMGI_MAT / VTSI_MAT as read (the chosen copy's bytes) */
  int                bup_only;          /* the IFO's signature is wrong: only its BUP is read */
  int                whole;             /* the whole IFO was read once (else its tables piece by piece) */
  int64_t            ifo_sector;        /* image: the IFO's place; 0 in a folder */
  int64_t            bup_sector;        /* image: the BUP's place (the IFO's when the BUP is not one piece) */
  uint32_t           title_vob_sectors; /* the title VOBs' length (the VMG: its menu VOB's); image: by the
                                           places of the IFO and BUP, a folder: by the files */
  int64_t            menu_vob_start;    /* image: where its menu VOBs start (the application's place), 0 */
  int64_t            title_vob_start;   /* image: where its title VOBs start (the application's place), 0 */
  int                no_pgcit;          /* a title set without a program chain table */
  pgcit_rec_t       *pgcit;             /* its program chain table (freed with the disc), NULL */
  uint8_t           *ptt_srpt;          /* VTS_PTT_SRPT as recorded, NULL */
  uint32_t           ptt_srpt_size;
  uint32_t          *vobu_map;          /* VTS_VOBU_ADMAP: the VOBU starts, sorted, each once */
  int                nb_vobu_map;
  int                map_trusted;       /* its VOBU address map holds every cell (set when the tables were read) */
  uint32_t           menu_vobu_last;    /* the last VOBU start of the menus' VOBU address map */
} dvd_ifo_rec_t;

typedef struct
{
  dvd_reader_t  *ctx;                   /* the reader it was read with */
  int            nr_of_title_sets;
  uint8_t       *tt_srpt;               /* TT_SRPT as recorded (at most 101 entries) */
  uint32_t       tt_srpt_size;
  uint32_t       title_set_start[99];   /* per title set, the start the title table gives (0: none) */
  dvd_text_t    *text;                  /* the text data, NULL: none */
  char          *text_name;             /* the disc's name in the text data, NULL: none */
  char           provider_id[33];       /* the VMG's provider_identifier */
  dvd_ifo_rec_t  ifos[100];             /* [0] the VMG, [n] title set n (up to nr_of_title_sets) */
} dvd_disc_rec_t;

/**
 * Where an information file's menu VOBs (menu 1) or title VOBs (menu 0)
 * start on a disc image (absolute sector, 0: none), decided by the
 * application, which reads them; asked while the structure is read, for
 * every information file on an image (the title VOBs of a title set with a
 * program chain table): dvd_ifo_rec_t menu_vob_start / title_vob_start.
 */
typedef int64_t (*dvd_vob_place_f)( void *opaque, ifo_handle_t *ifo, int title, int menu );

/**
 * Reads the disc's structure (see above); place (may be NULL: none) is
 * asked where the VOBs start. NULL when it is no usable DVD-Video disc
 * (reported). Close with DVDCloseDisc_rec() before DVDClose(): it closes the
 * information files.
 */
DVDREAD_API dvd_disc_rec_t *DVDOpenDisc_rec( dvd_reader_t *, dvd_vob_place_f place, void *opaque );
DVDREAD_API void DVDCloseDisc_rec( dvd_disc_rec_t * );

/**
 * Title set title's title VOBs share sectors with another title set's: by
 * each one's start in the title table and its IFO header (vtstt_vobs up to
 * vts_last_sector - vtsi_last_sector).
 */
DVDREAD_API int DVDDiscTitleVOBsOverlap_rec( const dvd_disc_rec_t *, int title );

#ifdef __cplusplus
};
#endif
#endif /* LIBDVDREAD_DISC_READ_H */
