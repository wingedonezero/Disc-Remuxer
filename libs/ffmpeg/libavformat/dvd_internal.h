/*
 * DVD-Video: the disc's structure as the DVD format part of the disc layer
 * has it from libdvdread (dvd_disc.c, dvd_ifo.c). Field names follow libdvdread's public
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

#include <dvdnav/dvdnav.h>
#include <dvdnav/scan.h>
#include <dvdread/disc_read.h>
#include <dvdread/ifo_types.h>
#include <dvdread/vob_read.h>

#include "io/discio.h"
#include "discinfo.h"

#define DVD_SECTOR          2048
#define DVD_MAX_TITLE_SETS  99

/* VMGI_MAT / VTSI_MAT fields (byte offsets in the IFO's first sector) */
#define DVD_LAST_SECTOR         0x0c    /* vmg_last_sector / vts_last_sector */
#define DVD_I_LAST_SECTOR       0x1c    /* vmgi_last_sector / vtsi_last_sector */
#define DVD_VMG_CATEGORY        0x22    /* VMG: vmg_category (bits 16-23: the regions it is not for) */
#define DVD_TT_SRPT             0xc4    /* VMG: tt_srpt; VTS: vtstt_vobs */

/** One of the disc's own files, looked up once (ff_dvd_disc_file()). */
typedef struct DVDDiscFile {
    int          looked;
    DiscIOFile  *file;                  /**< NULL: the disc has no such file */
    DiscIOFile  *region;                /**< an image's IFO / BUP in one piece: the image from its place on */
} DVDDiscFile;

#define DVD_FILE_SLOTS 12               /* per information file: IFO, BUP, menu VOB, title VOB files 1..9 */

/** The disc for the DVD format part: libdvdread's structure (rec: its
 *  title sets rec->ifos[0..nr_of_title_sets], the title table, the text
 *  data, the program chains) and what the format part adds: the disc's own
 *  files, its name, the readers. */
typedef struct DVDDisc {
    void        *log;
    DiscIOFS    *fs;
    const char  *dir;                   /**< the folder of VIDEO_TS.IFO: "/VIDEO_TS", or "" */
    int          folder;                /**< a disc folder (else an image) */
    char        *volume_name;           /**< the volume label, else the provider identifier */
    struct dvd_reader_s *reader;        /**< libdvdread on the disc's own files (ff_dvd_reader_open()) */
    dvd_disc_rec_t *rec;                /**< the disc's structure as libdvdread read it (DVDOpenDisc_rec()) */
    DVDDiscFile  files[DVD_MAX_TITLE_SETS + 1][DVD_FILE_SLOTS];
    dvd_findings_t *findings;           /**< what libdvdread / libdvdnav found, each said once (both log into
                                             the program's log; dvd_nav.c) */
} DVDDisc;

/** The information files: the VMG and the title sets. */
static inline int ff_dvd_nb_sets(const DVDDisc *d)
{
    return d->rec ? d->rec->nr_of_title_sets + 1 : 0;
}

/** Information file set (0: the VMG) as libdvdread read it; NULL when the
 *  disc has none or it could not be read. */
static inline const dvd_ifo_rec_t *ff_dvd_set(const DVDDisc *d, int set)
{
    return set >= 0 && set < ff_dvd_nb_sets(d) && d->rec->ifos[set].ifo ? &d->rec->ifos[set] : NULL;
}

static inline uint32_t ff_dvd_header32(const dvd_ifo_rec_t *f, int off)
{
    const uint8_t *p = f->header + off;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* ---- dvd_ifo.c: the disc's own files ---- */
/** The disc's own file of information file set (0: VIDEO_TS), slot 0 its
 *  IFO, 1 its BUP, 2 its menu VOB, 3.. its title VOB files 1..9, looked up
 *  once through the disc layer's file system; NULL when there is none. */
const DVDDiscFile *ff_dvd_disc_file(DVDDisc *d, int set, int slot);
/** Bytes of information file set from sector on, as libdvdread chose and
 *  read them: from the whole-IFO read when there is one, else per 2048-byte
 *  piece from the IFO, the BUP when that fails. */
int  ff_dvd_ifo_read(DVDDisc *d, int set, uint32_t sector, uint8_t *buf, int len);

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
/** libdvdread on the disc's own files (and the image), reporting to the
 *  disc: d->reader. */
int  ff_dvd_reader_open(DVDDisc *d);
/** The summary of the findings since the last one: per file its counts
 *  (each finding was said when it was found). */
void ff_dvd_reports_log(DVDDisc *d);
/** Counts of the findings: harmless ones and rule breaks. */
void ff_dvd_reports_count(const DVDDisc *d, int *harmless, int *breaks);
/** The logger the DVD engine's navigators use (the program's log, the
 *  disc's findings book). < 0 on no memory. */
int  ff_dvd_nav_logger(DVDDisc *d, dvdnav_logger_cb *cb);

/* ---- reading (dvd_read.c, on libdvdread, vob_read.h) ---- */
/** Title set vts's own reading of its title VOBs (libdvdread's, kept with
 *  the disc: DVDDiscTitleVOBs_rec(); NULL: they cannot be read, said). */
dvd_vob_reader_t *ff_dvd_own_reading(DVDDisc *d, int vts);
/** The NAV pack at title-VOB sector of title set vts, read by libdvdread
 *  (its checks report): 1 with *n; 0 when the sector is no (usable) NAV pack
 *  or cannot be read (recorded as such). */
int  ff_dvd_nav_read(DVDDisc *d, int vts, uint32_t sector, dvd_vobu_nav_t *n);
/** Title set vts's title VOBs share sectors with another title set's. */
int  ff_dvd_title_vobs_overlap(const DVDDisc *d, int vts);
/** Whether title set vts's VOBU address map is trusted (libdvdread). */
int  ff_dvd_map_trusted(const DVDDisc *d, int vts);
/** sector starts a VOBU: by the VOBU address map while it is trusted (a
 *  sector missing from it ends that, reported), else by the maps of title
 *  sets sharing the title VOBs, else by its NAV pack. */
int  ff_dvd_vobu_start_check(DVDDisc *d, int vts, uint32_t sector);

/* ---- dvd_scan.c ---- */
/** The navigation scan: the disc played from its start and every title,
 *  every reachable menu button pressed, per region the disc allows, and
 *  the cells played per title / program chain recorded. trace (may be
 *  NULL) receives every navigator command, event, sector read and path
 *  line. < 0 when the scan failed (no results: the titles keep the order
 *  of the title table). The results are libdvdnav's
 *  (dvdnav_scan_free_results()); the VOBU records are left on the disc. */
int  ff_dvd_scan(DVDDisc *d, void (*trace)(void *opaque, const char *line), void *opaque, dvdnav_scan_results_t *out);

/* ---- dvd_titles.c: the title list is libdvdread's (dvdread/title_list.h:
 * dvd_title_list_t, dvd_title_t, its segments dvd_segment_t, chapters
 * dvd_title_chapter_t, streams dvd_stream_ref_t) ---- */

#define DVD_VTS_VIDEO_ATTR  0x200       /* VTSI_MAT vts_video_attr (2 bytes) */
#define DVD_VTS_AUDIO_ATTR  0x204       /* VTSI_MAT vts_audio_attr[8] (8 bytes each) */
#define DVD_VTS_SUBP_ATTR   0x256       /* VTSI_MAT vts_subp_attr[32] (6 bytes each) */

/** Title t's program chain. */
static inline const pgc_rec_t *ff_dvd_title_pgc(const DVDDisc *d, const dvd_title_t *t)
{
    return d->rec->ifos[t->vtsn].pgcit->pgcs[t->pgcn];
}

enum { DVD_CELL_MODE_AUTO, DVD_CELL_MODE_WALK, DVD_CELL_MODE_TRIM, DVD_CELL_MODE_WALK_TRIM };
enum { DVD_CELL_CHECK_ALL, DVD_CELL_CHECK_SPREAD, DVD_CELL_CHECK_RANDOM };

typedef struct DVDTitleOptions {
    int cell_mode;                      /**< DVD_CELL_MODE_*: auto = walk with the navigation's cells, else trim */
    int cell_check;                     /**< DVD_CELL_CHECK_*: the VOBUs a full cell check reads */
    int min_length;                     /**< seconds */
} DVDTitleOptions;

/** The title list from the disc's structure and the navigation scan (a
 *  failed scan: no results), libdvdread's (DVDFreeTitleList_rec()). */
int   ff_dvd_titles_build(DVDDisc *d, const dvdnav_scan_results_t *scan, const DVDTitleOptions *o,
                          dvd_title_list_t *out);
/** The full cell check of cell (1-based) of program chain pgcn of title
 *  set vts, with mode (DVD_CELL_CHECK_*): 1 failed, 0 passed, < 0 no such
 *  cell. */
int   ff_dvd_cell_check(DVDDisc *d, int vts, int pgcn, int cell, int mode);
/** One line per title (tests, debug log); av_free() it. */
char *ff_dvd_titles_dump(const dvd_title_list_t *l);

/* ---- dvd_read.c ---- */
/** A reading of title set vts's title VOBs of its own (libdvdread's, with
 *  its own damaged ranges); NULL when they cannot be opened (said).
 *  DVDCloseTitleVOBs_rec() it. */
dvd_vob_reader_t *ff_dvd_title_vobs_open(DVDDisc *d, int vts);
/** Sector of title set vts's title VOBs, read, checked and CSS descrambled by
 *  libdvdread (see dvd_read.c): 1 read, 0 left out (damaged: no MPEG-2 pack
 *  with a packet), < 0 cannot be read (a read error; AVERROR_INVALIDDATA:
 *  scrambled without a title key). vr: a reading of its own (ff_dvd_title_vobs_open()),
 *  a damaged range that this read ends into *ended (may be NULL); NULL: the
 *  title set's own reading (its ranges are not given back). */
int      ff_dvd_title_vob_read(DVDDisc *d, int vts, dvd_vob_reader_t *vr, uint32_t sector, uint8_t *buf,
                               dvd_vob_damage_t *ended);
/** Title set vts's CSS title key as libdvdread found it: 1 found (into key,
 *  5 bytes, may be NULL), 0 not looked for yet, -1 none. */
int      ff_dvd_css_state(DVDDisc *d, int vts, uint8_t *key);
/** ff_dvd_title_vob_read() on the title set's own reading. */
int      ff_dvd_title_vob_sector(DVDDisc *d, int vts, uint32_t sector, uint8_t *buf);
/** The sectors of a segment. */
/** The VOB file holding block (from 0) of segment seg of title t and the
 *  byte offset there (libdvdread's reading vr names it); < 0 not known. */
int ff_dvd_segment_place(const dvd_title_t *t, const dvd_vob_reader_t *vr, int seg, uint32_t block, char *file,
                         int size, int64_t *offset);
uint32_t ff_dvd_segment_blocks(const dvd_segment_t *g);
/** Sector block of segment seg of title t, as ff_dvd_title_vob_read(). */
int      ff_dvd_segment_read_vobs(DVDDisc *d, const dvd_title_t *t, dvd_vob_reader_t *vr, int seg, uint32_t block,
                                  uint8_t *buf, dvd_vob_damage_t *ended);
/** The file view (vob_read.h) of reading vr for segment seg of title t, read
 *  next: 1 used when a read fails, 0 not, < 0 an error. */
int      ff_dvd_segment_view(const dvd_title_t *t, dvd_vob_reader_t *vr, int seg);
/** Sector block of segment seg of title t, as ff_dvd_title_vob_sector(). */
int      ff_dvd_segment_read(DVDDisc *d, const dvd_title_t *t, int seg, uint32_t block, uint8_t *buf);

/* ---- dvd_disc.c ---- */
/** The disc's structure as libdvdread reads and checks it (DVDOpenDisc_rec():
 *  the VMG, its title table and text data, every title set; one that cannot
 *  be read stays a placeholder); what it finds logged. dir: the folder of
 *  VIDEO_TS.IFO ("/VIDEO_TS" or ""); label: the volume label (NULL for a
 *  folder). */
int  ff_dvd_disc_open(void *log, DiscIOFS *fs, const char *dir, int folder, const char *label, DVDDisc *d);
void ff_dvd_disc_free(DVDDisc *d);
/** The structure as text (debug log). */
void ff_dvd_disc_dump(const DVDDisc *d, AVBPrint *bp);

#endif /* AVFORMAT_DVD_INTERNAL_H */
