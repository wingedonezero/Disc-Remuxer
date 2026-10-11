/*
 * DVD-Video: the disc's structure as the DVD format part of the disc layer
 * reads it (dvd_disc.c, dvd_ifo.c). Field names follow libdvdread's public
 * headers (dvdread/ifo_types.h); the layouts are those of the DVD-Video
 * information files as libdvdread documents them.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef AVFORMAT_DVD_INTERNAL_H
#define AVFORMAT_DVD_INTERNAL_H

#include <stdint.h>

#include "libavutil/bprint.h"

#include <dvdread/ifo_types.h>
#include <dvdread/vob_read.h>

#include "discio.h"
#include "discinfo.h"

#define DVD_SECTOR          2048
#define DVD_MAX_TITLE_SETS  99
#define DVD_IFO_HEADER      0x400       /* the part of an IFO read for its header */

/* VMGI_MAT / VTSI_MAT fields (byte offsets in the IFO's first sector) */
#define DVD_LAST_SECTOR         0x0c    /* vmg_last_sector / vts_last_sector */
#define DVD_I_LAST_SECTOR       0x1c    /* vmgi_last_sector / vtsi_last_sector */
#define DVD_NR_OF_TITLE_SETS    0x3e    /* VMG: vmg_nr_of_title_sets (16 bits) */
#define DVD_PROVIDER_ID         0x40    /* VMG: provider_identifier (32 bytes) */
#define DVD_TT_SRPT             0xc4    /* VMG: tt_srpt; VTS: vtstt_vobs */
#define DVD_VTS_PTT_SRPT        0xc8
#define DVD_VTS_PGCIT           0xcc
#define DVD_TXTDT_MGI           0xd4    /* VMG: txtdt_mgi */
#define DVD_M_VOBU_ADMAP        0xdc    /* vmgm_vobu_admap / vtsm_vobu_admap */
#define DVD_VTS_VOBU_ADMAP      0xe4

/** A cell of a program chain (cell_playback_t / cell_position_t). */
typedef struct DVDCell {
    uint8_t  category[4];               /**< the first 4 bytes of cell_playback_t, as recorded */
    uint32_t playback_time;             /**< BCD hh:mm:ss:ff as recorded */
    uint32_t first_sector;
    uint32_t first_ilvu_end_sector;
    uint32_t last_vobu_start_sector;
    uint32_t last_sector;
    uint16_t vob_id_nr;                 /**< cell_position_t */
    uint8_t  cell_nr;
    uint8_t  seamless_angle_checked;    /**< title list: 0 not yet, 1 its VOBUs disagree, 2 they agree */
} DVDCell;

/** A program chain (pgc_t) of a title set's VTS_PGCIT. */
typedef struct DVDPgc {
    int       number;                   /**< its search-pointer index, from 0 */
    uint32_t  entry;                    /**< the search pointer's first field (entry_id, ptl mask) */
    uint8_t   nr_of_programs;
    uint8_t   nr_of_cells;
    uint32_t  playback_time;            /**< BCD as recorded */
    uint32_t  prohibited_ops;
    uint8_t   audio_control[16];        /**< 8 x 16 bits as recorded */
    uint8_t   subp_control[128];        /**< 32 x 32 bits as recorded */
    uint32_t  palette[16];
    uint16_t  nr_of_pre, nr_of_post, nr_of_cell;
    uint8_t  *commands;                 /**< (pre + post + cell) x 8 bytes, NULL when none */
    uint8_t  *program_map;              /**< nr_of_programs entry cells */
    DVDCell  *cells;                    /**< nr_of_cells */
} DVDPgc;

/** What a title-VOB sector's NAV pack says about its VOBU (recorded when
 *  the navigation reads it, dvd_scan.c), or that the sector is no NAV pack. */
typedef struct DVDVobuRec {
    uint32_t key;                       /**< the sector, relative to the title VOBs (+ DVDIfo.title_vob_base) */
    uint16_t vobu_ea;
    uint16_t ilvu_ea;
    uint32_t next;                      /**< sml_pbi category & 0x6000 << 16 | distance to the next VOBU
                                             (0x10000000: none); DVD_VOBU_NO_NAV: the sector is no NAV pack */
    uint32_t vobu_s_ptm, vobu_e_ptm;
} DVDVobuRec;

#define DVD_VOBU_NO_NAV 0xffffffffu     /**< DVDVobuRec.next: the sector is no NAV pack */
#define DVD_VOBU_END    0xfffffffeu     /**< a next VOBU: none, the cell ends */
#define DVD_VOBU_SELF   0xfffffffdu     /**< a next VOBU: the VOBU names itself */

/** VOBU records sorted by key, each key once (the first one recorded). */
typedef struct DVDVobuRecs {
    DVDVobuRec *recs;
    int         nb, alloc;
} DVDVobuRecs;

/** A table of an IFO read whole (its length from its header). */
typedef struct DVDTable {
    uint8_t  *data;
    uint32_t  size;
} DVDTable;

/** One information file: the VMG (index 0) or title set index. */
typedef struct DVDIfo {
    int          index;
    int          failed;                /**< not loaded: a placeholder (the disc goes on without it) */
    /* the two copies: [0] the IFO, [1] the BUP. In a folder the files
     * themselves (a missing one replaced by the other); in an image the
     * image from the file's position to its end (the IFO's tables are read
     * by their sector from there). */
    DiscIOFile  *copy[2];
    ifo_handle_t *rec;                  /**< libdvdread's handle (ifoOpen_rec()): it chose IFO / BUP and read them */
    int          bup_only;              /**< the IFO's signature is wrong: only the BUP is read */
    const uint8_t *cache;               /**< the whole IFO as libdvdread read it (NULL: read in pieces) */
    int          cache_size;
    uint8_t      header[DVD_IFO_HEADER];

    /* where its files are */
    int64_t      ifo_sector, bup_sector;        /**< image: file system positions (0 in a folder) */
    int64_t      menu_vob_sector, title_vob_sector; /**< image: file system positions, 0 when not one extent */
    int64_t      ifo_bytes, menu_vob_bytes, title_vob_bytes, bup_bytes; /**< folder: file sizes */
    DiscIOFile  *menu_vob;              /**< folder: the menu VOB (NULL: none) */
    DiscIOFile **title_vobs;            /**< folder: VTS_nn_1..9.VOB up to the first missing one */
    int          nb_title_vobs;

    uint32_t     title_vob_blocks;      /**< the title VOBs' length in sectors */
    uint32_t     title_vob_start;       /**< image: the title VOBs' first sector as libdvdread decided it (the IFO
                                             header's or the file system's place; 0: none) */
    uint32_t     menu_vobu_last;        /**< the last VOBU start of the menu VOBU address map */
    int          no_pgcit;              /**< VTS without a program chain table */
    int          vobu_map_distrusted;   /**< a cell does not start on the VOBU address map */

    DVDPgc     **pgcs;                  /**< VTS_PGCIT: NULL for a search pointer whose program chain is
                                             left out (outside the table, a repeated start, broken) */
    int         *pgc_same_as;           /**< for a repeated start: the earlier search pointer, else -1 */
    int          nb_pgcs;
    DVDTable     ptt_srpt;              /**< VTS_PTT_SRPT as recorded */
    uint32_t    *vobu_map;              /**< VTS_VOBU_ADMAP: the VOBU starts, sorted, without repeats */
    int          nb_vobu_map;

    /* the navigation scan (dvd_scan.c) */
    uint32_t     title_vob_base;        /**< image: the title VOBs' absolute first sector (VOBU records of all
                                             such title sets are kept on the disc by absolute sector); 0 */
    DVDVobuRecs  vobus;                 /**< VOBU records of its title VOBs (title_vob_base 0) */
    int          entered_by_navigation; /**< the navigation played one of its cells (title or menu) */

    /* the title list (dvd_titles.c) */
    int          suspicious;            /**< 0 not decided, 1 its titles are left out, 2 they are not */

    /* reading (dvd_read.c) */
    dvd_vob_reader_t *vobs;             /**< libdvdread's reading of its title VOBs (NULL: not opened yet) */
    int          vobs_failed;           /**< they cannot be opened */
} DVDIfo;

/** A node of the disc's text data (TXTDT_MG): an item kind below 0x30
 *  with the texts recorded under it. */
typedef struct DVDTextNode {
    struct DVDTextNode  *parent;
    uint16_t             offset;        /**< its entry's offset in the language unit */
    uint8_t              kind;
    int                  nb_texts;
    uint8_t             *text_kinds;    /**< sorted; the first text of a kind is kept */
    char               **texts;
    int                  nb_children;
    struct DVDTextNode **children;
} DVDTextNode;

typedef struct DVDText {
    char          language[4];          /**< as recorded */
    uint16_t      title_offsets[99];    /**< each title's node offset (0: none) */
    DVDTextNode  *root;
    DVDTextNode **nodes;                /**< every node (owned) */
    int           nb_nodes;
} DVDText;

/** A finding of libdvdread / libdvdnav about the disc's data (their
 *  dvd_report_t), kept once. */
typedef struct DVDReport {
    int          level;                 /**< dvd_report_level_t: info (harmless), warning, error */
    char         file[16];              /**< "VTS_01_0.IFO", "": not a file's data (navigation) */
    char         table[24];
    char        *text;
    int64_t      offset;                /**< byte offset in the file, -1 */
    int64_t      sector;                /**< VOB sector, -1 */
    uint8_t      bytes[16];             /**< the field's bytes (at most 16) */
    int          nb_bytes;
} DVDReport;

typedef struct DVDDisc {
    void        *log;
    DiscIOFS    *fs;
    const char  *dir;                   /**< the folder of VIDEO_TS.IFO: "/VIDEO_TS", or "" */
    int          folder;                /**< a disc folder (else an image) */
    DVDIfo     **ifos;                  /**< [0] the VMG, [n] title set n */
    int          nb_ifos;
    int          nr_of_title_sets;
    DVDTable     tt_srpt;               /**< TT_SRPT as recorded */
    uint32_t    *title_set_start;       /**< per title set, the start the title table gives (0: none) */
    DVDText     *text;                  /**< text data (NULL: none) */
    DVDVobuRecs  vobus;                 /**< VOBU records of title sets with a title_vob_base */
    char        *volume_name;           /**< the volume label, else the provider identifier */
    char        *text_name;             /**< the disc's name in its text data, NULL when none */
    struct dvd_reader_s *reader;        /**< libdvdread on the disc as read here (ff_dvd_reader_open()) */
    DVDReport   *reports;               /**< what libdvdread / libdvdnav found, each once */
    int          nb_reports, reports_cap;
    int          reports_said;          /**< the reports logged so far */
} DVDDisc;

/* ---- dvd_ifo.c ---- */
/** Open the files of IFO index of the disc (index 0: VIDEO_TS) and load
 *  it: < 0 when it cannot be used (the caller keeps a placeholder for a
 *  title set). */
int  ff_dvd_ifo_load(DVDDisc *d, int index, DVDIfo **out);
/** IFO f's tables by libdvdread (d->reader): the menus' VOBU address map;
 *  of a title set also its program chains (read with the title list's
 *  rules), chapter table (as recorded) and title VOBs' VOBU address map, its
 *  cells checked against that map. A VOBU address map that cannot be read
 *  leaves the VOBU starts untrusted. < 0 when a needed table cannot be
 *  read. */
int  ff_dvd_ifo_tables(DVDDisc *d, DVDIfo *f);
/** A table of IFO f as recorded, by libdvdread (d->reader): at sector, its
 *  last-byte field at last_byte_at; last_byte_at = -n: the VMG's title table
 *  with at most n entries. 0, or < 0 when it cannot be read. */
int  ff_dvd_ifo_raw_table(DVDDisc *d, DVDIfo *f, uint32_t sector, int last_byte_at, const char *table,
                          DVDTable *t);
/** A file of title set index (0: VIDEO_TS) by its extension ("IFO", "BUP",
 *  "VOB"; part: the title VOB number, 0 for the IFO / BUP / menu VOB),
 *  through the disc layer's file system. */
int  ff_dvd_set_file(DVDDisc *d, int index, int part, const char *ext, DiscIOFile **out);
void ff_dvd_ifo_free(DVDIfo **ifo);
/** Bytes of the IFO from sector on, as libdvdread chose and read them:
 *  from the whole-IFO read when there is one, else per 2048-byte piece from
 *  the IFO, the BUP when that fails. */
int  ff_dvd_ifo_read(DVDDisc *d, DVDIfo *f, uint32_t sector, uint8_t *buf, int len);
void ff_dvd_table_free(DVDTable *t);

static inline uint32_t ff_dvd_header32(const DVDIfo *f, int off)
{
    const uint8_t *p = f->header + off;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* ---- dvd_nav.c ---- */
struct dvdnav_s;
/** What the navigator's reads are handed to. */
typedef struct DVDNavHooks {
    void *opaque;
    /** A VOB sector the navigator reads: of information file set (0: the
     *  VMG), its menu VOB or title VOBs, sector relative to them. < 0 makes
     *  the read fail. */
    int (*vob_read)(void *opaque, int set, int menu, uint32_t sector, const uint8_t *data);
} DVDNavHooks;
/** A libdvdnav navigator on the disc: libdvdread reads its files through
 *  callbacks serving the disc's structure (the IFOs as read here, the VOBs
 *  from the disc layer). hooks may be NULL. The disc must outlive the
 *  navigator (dvdnav_close). */
int  ff_dvd_nav_open(DVDDisc *d, const DVDNavHooks *hooks, struct dvdnav_s **out);
/** The IFO check: every information file read once by libdvdread (all its
 *  tables), what it finds kept as reports of the disc. */
void ff_dvd_ifo_check(DVDDisc *d);
/** libdvdread on the disc as read here (every loaded IFO, the VOBs from the
 *  disc layer), reporting to the disc: d->reader. */
int  ff_dvd_reader_open(DVDDisc *d);
/** The reports not logged yet: the findings that break a rule as warnings,
 *  the harmless ones as detail lines, one summary line per file. */
void ff_dvd_reports_log(DVDDisc *d);
/** Counts of the reports: harmless ones and rule breaks. */
void ff_dvd_reports_count(const DVDDisc *d, int *harmless, int *breaks);

/** Read sector of a title set's title VOBs (menu 0) or menu VOB through
 *  the disc layer, as the navigator reads it. */
int  ff_dvd_vob_read(DVDDisc *d, DVDIfo *f, int menu, uint32_t sector, uint8_t *buf);

/* ---- dvd_vobu.c ---- */
/** The NAV pack fields of a VOBU (pci_gi, dsi_gi, sml_pbi, vobu_sri). */
typedef struct DVDNav {
    uint32_t vobu_s_ptm, vobu_e_ptm, vobu_ea, ilvu_ea, next_vobu;
    uint16_t category;
} DVDNav;

int  ff_dvd_is_nav_pack(const uint8_t *b);
void ff_dvd_nav_parse(DVDNav *n, const uint8_t *b);
/** The NAV pack at title-VOB sector of f: 1 with *n; 0 when the sector is
 *  no (usable) NAV pack or cannot be read (recorded as such). A NAV pack
 *  whose headers were blanked is accepted (0.5 s). */
int  ff_dvd_nav_read(DVDDisc *d, DVDIfo *f, uint32_t sector, DVDNav *n);
/** Record the VOBU whose NAV pack n is at sector: *next = the next VOBU
 *  (DVD_VOBU_END, DVD_VOBU_SELF), *len = its length in sectors. 0 when it
 *  cannot be (it names itself and allow_self is 0; a following VOBU of its
 *  interleaved unit cannot be read). */
int  ff_dvd_vobu_register(DVDDisc *d, DVDIfo *f, const DVDNav *n, uint32_t sector, int allow_self,
                          uint32_t *next, uint32_t *len);
/** Record title-VOB sector of f as no NAV pack. */
int  ff_dvd_vobu_mark_bad(DVDDisc *d, DVDIfo *f, uint32_t sector);
void ff_dvd_vobu_recs_free(DVDVobuRecs *v);
/** The VOBU record of title-VOB sector of f (NULL: none); *next_key (when
 *  not NULL): the key after it, -1 when none. */
const DVDVobuRec *ff_dvd_vobu_find(const DVDDisc *d, const DVDIfo *f, uint32_t sector, int64_t *next_key);
/** Title set vts's title VOBs share sectors with another title set's. */
int  ff_dvd_title_vobs_overlap(const DVDDisc *d, int vts);
/** The first VOBU address map entry not below sector (*exact: it is sector). */
int  ff_dvd_map_find(const DVDIfo *f, uint32_t sector, int *exact);
/** sector starts a VOBU: by the VOBU address map while it is trusted (a
 *  sector missing from it ends that), else by the maps of title sets
 *  sharing the title VOBs, else by its NAV pack. */
int  ff_dvd_vobu_start_check(DVDDisc *d, DVDIfo *f, uint32_t sector);
/** The VOBU after the one at sector: by the map while trusted, else by the
 *  maps of title sets sharing the title VOBs, else by the NAV packs. */
int  ff_dvd_vobu_next(DVDDisc *d, DVDIfo *f, uint32_t sector, uint32_t *next);
/** sector holds no usable NAV pack (outside the title VOBs, unreadable, no
 *  NAV pack: recorded); a NAV pack read here is recorded. */
int  ff_dvd_vobu_bad(DVDDisc *d, DVDIfo *f, uint32_t sector);

/* ---- dvd_scan.c ---- */
/** One result of the navigation scan: the cells the navigation played in
 *  title (TT_SRPT number, low 8 bits) / program chain pgc (low 8 bits),
 *  and the name of the path that played them ("A1", "B2", ...). */
typedef struct DVDScanResult {
    char      name[16];
    uint8_t   title, pgc;
    int       nb_cells;
    uint8_t  *cells;
} DVDScanResult;

typedef struct DVDScan {
    DVDScanResult *results;             /**< sorted by title, then pgc */
    int            nb_results;
} DVDScan;

/** The navigation scan: the disc played from its start and every title,
 *  every reachable menu button pressed, per region the disc allows, and
 *  the cells played per title / program chain recorded. trace (may be
 *  NULL) receives every navigator command, event, sector read and path
 *  line. < 0 when the scan failed (no results: the titles keep the order
 *  of the title table). VOBU records and DVDIfo.entered_by_navigation are
 *  left on the disc. */
int  ff_dvd_scan(DVDDisc *d, void (*trace)(void *opaque, const char *line), void *opaque, DVDScan *out);
void ff_dvd_scan_free(DVDScan *s);

/* ---- dvd_titles.c ---- */
/** A piece of the title VOBs: count sectors from sector (relative to the
 *  title VOBs), at logical in its segment. */
typedef struct DVDExtent {
    uint32_t logical, sector, count;
} DVDExtent;

/** A run of a title on one timeline: its extents, in playing order. */
typedef struct DVDSegment {
    DVDExtent *extents;
    int        nb_extents, alloc_extents;
    int        last_cell;               /**< the cell of the last VOBU added */
    uint16_t (*runs)[2];                /**< runs of consecutive cells (1-based first, last) */
    int        nb_runs, alloc_runs;
    int64_t    size;                    /**< bytes */
    char      *cell_map;                /**< the cell runs as text: "1-3", "(1-3,5)" */
} DVDSegment;

#define DVD_CHAPTER_NONE (-1)

typedef struct DVDChapter {
    uint16_t pgcn, pgn;                 /**< the PTT entry */
    int      cell;                      /**< its program's entry cell (index in the program chain), -1 none */
    int      segment;                   /**< DVD_CHAPTER_NONE: not placed */
    int64_t  offset;                    /**< bytes in the segment */
    int64_t  time;                      /**< 90 kHz: from the title's start to the end of its first VOBU */
} DVDChapter;

/** A title's audio or sub-picture stream: its stream number and the title
 *  set's attributes it is described by (VTSI_MAT vts_audio_attr /
 *  vts_subp_attr index). */
typedef struct DVDStreamRef {
    uint8_t stream, attr;
} DVDStreamRef;

#define DVD_VTS_VIDEO_ATTR  0x200       /* VTSI_MAT vts_video_attr (2 bytes) */
#define DVD_VTS_AUDIO_ATTR  0x204       /* VTSI_MAT vts_audio_attr[8] (8 bytes each) */
#define DVD_VTS_SUBP_ATTR   0x256       /* VTSI_MAT vts_subp_attr[32] (6 bytes each) */

typedef struct DVDTitle {
    char           name[24];            /**< the title table entry ("4"), with its program chain ("4/1") */
    int            title;               /**< the title table entry, from 0 */
    uint8_t        pb_ty, vtsn, vts_ttn;
    uint8_t        nr_of_angles, angle; /**< angle from 0 */
    DVDIfo        *vts;
    DVDPgc        *pgc;
    const DVDScanResult *scan;          /**< the navigation's cells of this title / program chain, NULL none */
    DVDChapter    *chapters;
    int            nb_chapters;
    int           *cells;               /**< the cells played, indexes in the program chain */
    int            nb_cells, alloc_cells;
    uint32_t       declared;            /**< seconds: the cells' playback times */
    uint32_t       measured;            /**< seconds: the cells' VOBUs' presentation times */
    DVDSegment    *segments;
    int            nb_segments, alloc_segments;
    int            built;
    int            nav_invalid;         /**< places with invalid navigation found */
    int            short_title;         /**< shorter than the minimum length: listed, not selected */
    char           source[96];          /**< what the title is made from, for the disc info */
    DVDStreamRef   audio[8];            /**< in the program chain's audio_control order, then the title set's
                                             other audio streams */
    int            nb_audio;
    uint8_t        audio_listed;        /**< the stream numbers audio_control lists (bit per stream) */
    uint8_t        audio_dropped;       /**< audio[] entries the stream pass left out (bit per entry) */
    int            captions;            /**< the stream pass found closed captions in the video */
    DVDStreamRef   subp[33];            /**< in the program chain's subp_control order */
    int            nb_subp;
    uint16_t       subp_width, subp_height; /**< the sub-pictures' canvas */
} DVDTitle;

typedef struct DVDTitleList {
    DVDTitle **titles;
    int        nb_titles;
} DVDTitleList;

enum { DVD_CELL_MODE_AUTO, DVD_CELL_MODE_WALK, DVD_CELL_MODE_TRIM, DVD_CELL_MODE_WALK_TRIM };
enum { DVD_CELL_CHECK_ALL, DVD_CELL_CHECK_SPREAD, DVD_CELL_CHECK_RANDOM };

typedef struct DVDTitleOptions {
    int cell_mode;                      /**< DVD_CELL_MODE_*: auto = walk with the navigation's cells, else trim */
    int cell_check;                     /**< DVD_CELL_CHECK_*: the VOBUs a full cell check reads */
    int min_length;                     /**< seconds */
} DVDTitleOptions;

/** The title list from the disc's structure and the navigation scan (a
 *  failed scan: no results). */
int   ff_dvd_titles_build(DVDDisc *d, const DVDScan *scan, const DVDTitleOptions *o, DVDTitleList *out);
void  ff_dvd_titles_free(DVDTitleList *l);
void  ff_dvd_title_free(DVDTitle **t);
/** The full cell check of cell (1-based) of program chain pgcn of title
 *  set vts, with mode (DVD_CELL_CHECK_*): 1 failed, 0 passed, < 0 no such
 *  cell. */
int   ff_dvd_cell_check(DVDDisc *d, int vts, int pgcn, int cell, int mode);
/** One line per title (tests, debug log); av_free() it. */
char *ff_dvd_titles_dump(const DVDTitleList *l);

/* ---- dvd_read.c ---- */
/** A reading of title set f's title VOBs of its own (libdvdread's, with its
 *  own damaged ranges); NULL when they cannot be opened.
 *  DVDCloseTitleVOBs_rec() it. */
dvd_vob_reader_t *ff_dvd_title_vobs_open(DVDDisc *d, DVDIfo *f);
/** Sector of title set f's title VOBs, read, checked and CSS descrambled by
 *  libdvdread (see dvd_read.c): 1 read, 0 left out (damaged: no MPEG-2 pack
 *  with a packet), < 0 cannot be read (a read error; AVERROR_INVALIDDATA:
 *  scrambled without a title key). vr: a reading of its own (ff_dvd_title_vobs_open()),
 *  a damaged range that this read ends into *ended (may be NULL); NULL: f's
 *  own reading (its ranges are not given back). */
int      ff_dvd_title_vob_read(DVDDisc *d, DVDIfo *f, dvd_vob_reader_t *vr, uint32_t sector, uint8_t *buf,
                               dvd_vob_damage_t *ended);
/** Title set f's CSS title key as libdvdread found it: 1 found (into key,
 *  5 bytes, may be NULL), 0 not looked for yet, -1 none. */
int      ff_dvd_css_state(DVDDisc *d, const DVDIfo *f, uint8_t *key);
/** ff_dvd_title_vob_read() on f's own reading. */
int      ff_dvd_title_vob_sector(DVDDisc *d, DVDIfo *f, uint32_t sector, uint8_t *buf);
/** The sectors of a segment. */
uint32_t ff_dvd_segment_blocks(const DVDSegment *g);
/** Sector block of segment seg of title t, as ff_dvd_title_vob_read(). */
int      ff_dvd_segment_read_vobs(DVDDisc *d, const DVDTitle *t, dvd_vob_reader_t *vr, int seg, uint32_t block,
                                  uint8_t *buf, dvd_vob_damage_t *ended);
/** The file view (vob_read.h) of reading vr for segment seg of title t, read
 *  next: 1 used when a read fails, 0 not, < 0 an error. */
int      ff_dvd_segment_view(const DVDTitle *t, dvd_vob_reader_t *vr, int seg);
/** Sector block of segment seg of title t, as ff_dvd_title_vob_sector(). */
int      ff_dvd_segment_read(DVDDisc *d, const DVDTitle *t, int seg, uint32_t block, uint8_t *buf);

/* ---- dvd_streams.c ---- */
/** The probe of title t's streams (its largest segment read from the
 *  start): closed captions (t->captions) and the audio streams without
 *  data (t->audio_dropped). < 0: a sector cannot be read, the title cannot
 *  be used. The disc layer's stream pass follows (discpass.c). */
int ff_dvd_title_streams(DVDDisc *d, DVDTitle *t);

/* ---- dvd_disc.c ---- */
/** Read the disc's structure: the VMG, its title table and text data, and
 *  every title set (a title set that cannot be loaded stays a placeholder).
 *  dir: the folder of VIDEO_TS.IFO ("/VIDEO_TS" or ""); label: the volume
 *  label (NULL for a folder). */
int  ff_dvd_disc_open(void *log, DiscIOFS *fs, const char *dir, int folder, const char *label, DVDDisc *d);
void ff_dvd_disc_free(DVDDisc *d);
/** The structure as text (debug log). */
void ff_dvd_disc_dump(const DVDDisc *d, AVBPrint *bp);

#endif /* AVFORMAT_DVD_INTERNAL_H */
