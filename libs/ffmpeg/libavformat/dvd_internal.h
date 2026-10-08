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

#include "discio.h"

#define DVD_SECTOR          2048
#define DVD_MAX_TITLE_SETS  99
#define DVD_IFO_HEADER      0x400       /* the part of an IFO read for its header */
#define DVD_IFO_CACHE_MAX   (2 << 20)   /* an IFO read whole when it is no larger */
#define DVD_IFO_CACHE_TRIES 16          /* tries of that read (alternating IFO and BUP) */

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
    int          bup_only;              /**< the IFO's signature is wrong: only the BUP is read */
    uint8_t     *cache;                 /**< the whole IFO, read once (NULL: read from the copies) */
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
    uint32_t     title_vob_start;       /**< image: the title VOBs' first sector by the IFO header (0: not
                                             decided yet: the header and the file system disagree) */
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
    char        *volume_name;           /**< the volume label, else the provider identifier */
    char        *text_name;             /**< the disc's name in its text data, NULL when none */
} DVDDisc;

/* ---- dvd_ifo.c ---- */
/** Open the files of IFO index of the disc (index 0: VIDEO_TS) and load
 *  it: < 0 when it cannot be used (the caller keeps a placeholder for a
 *  title set). */
int  ff_dvd_ifo_load(DVDDisc *d, int index, DVDIfo **out);
void ff_dvd_ifo_free(DVDIfo **ifo);
/** Bytes of the IFO from sector on: from the whole-IFO read when there is
 *  one, else per 2048-byte piece from the IFO, the BUP when that fails. */
int  ff_dvd_ifo_read(DVDDisc *d, DVDIfo *f, uint32_t sector, uint8_t *buf, int len);
/** A table of the IFO at sector; its length from its first sector by
 *  len_of (0: empty, no error). 0 or < 0. */
int  ff_dvd_ifo_table(DVDDisc *d, DVDIfo *f, uint32_t sector, DVDTable *t,
                      uint32_t (*len_of)(const uint8_t *first, uint32_t field), uint32_t field);
uint32_t ff_dvd_table_len_last_byte(const uint8_t *first, uint32_t field);
void ff_dvd_table_free(DVDTable *t);

static inline uint32_t ff_dvd_header32(const DVDIfo *f, int off)
{
    const uint8_t *p = f->header + off;
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

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
