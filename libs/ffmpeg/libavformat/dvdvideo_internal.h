/*
 * DVD-Video demuxer, powered by libdvdnav and libdvdread
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * DVD-Video demuxer: definitions shared by its source files
 * (dvdvideodec.c and dvdvideo_*.c).
 */

#ifndef AVFORMAT_DVDVIDEO_INTERNAL_H
#define AVFORMAT_DVDVIDEO_INTERNAL_H

#include <inttypes.h>

#include <dvdnav/dvdnav.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include <dvdread/ifo_types.h>
#include <dvdread/nav_read.h>

#include "libavcodec/ac3_parser.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"

#include "avformat.h"
#include "avio_internal.h"
#include "discio.h"
#include "avlanguage.h"
#include "demux.h"
#include "dvdclut.h"
#include "internal.h"
#include "url.h"

#define DVDVIDEO_MAX_PS_SEARCH_BLOCKS                   128
#define DVDVIDEO_BLOCK_SIZE                             2048
#define DVDVIDEO_TIME_BASE_Q                            (AVRational) { 1, 90000 }
#define DVDVIDEO_PTS_WRAP_BITS                          32 /* VOBUs use 32 (PES allows 33) */
#define DVDVIDEO_LIBDVDX_LOG_BUFFER_SIZE                1024

#define PCI_START_BYTE                                  45 /* complement dvdread's DSI_START_BYTE */

enum DVDVideoSubpictureViewport {
    DVDVIDEO_SUBP_VIEWPORT_FULLSCREEN,
    DVDVIDEO_SUBP_VIEWPORT_WIDESCREEN,
    DVDVIDEO_SUBP_VIEWPORT_LETTERBOX,
    DVDVIDEO_SUBP_VIEWPORT_PANSCAN
};

typedef struct DVDVideoVTSVideoStreamEntry {
    int                                 startcode;
    enum AVCodecID                      codec_id;
    int                                 width;
    int                                 height;
    AVRational                          dar;
    AVRational                          framerate;
} DVDVideoVTSVideoStreamEntry;

typedef struct DVDVideoPGCAudioStreamEntry {
    int                                 startcode;
    enum AVCodecID                      codec_id;
    int                                 sample_fmt;
    int                                 sample_rate;
    int                                 bit_depth;
    int                                 nb_channels;
    AVChannelLayout                     ch_layout;
    int                                 disposition;
    const char                          *lang_iso;
} DVDVideoPGCAudioStreamEntry;

typedef struct DVDVideoPGCSubtitleStreamEntry {
    int                                 startcode;
    enum DVDVideoSubpictureViewport     viewport;
    int                                 disposition;
    uint32_t                            clut[FF_DVDCLUT_CLUT_LEN];
    const char                          *lang_iso;
} DVDVideoPGCSubtitleStreamEntry;

/* glibc's rand() sequence, as a source of random numbers of its own (dvdvideo_scan.c) */
typedef struct DVDVideoRand {
    uint32_t state[31];
    int      front;
} DVDVideoRand;

typedef struct DVDVideoPlaybackState {
    int                         celln;              /* ID of the active cell */
    int                         entry_pgn;          /* ID of the PG we are starting in */
    int                         in_pgc;             /* if our navigator is in the PGC */
    int                         in_ps;              /* if our navigator is in the program stream */
    int                         in_vts;             /* if our navigator is in the VTS */
    int                         is_seeking;         /* relax navigation path while seeking */
    int64_t                     nav_pts;            /* PTS according to IFO, not frame-accurate */
    int                         nb_vobu_skip;       /* number of VOBUs we should skip */
    uint64_t                    pgc_duration_est;   /* estimated duration as reported by IFO */
    uint64_t                    pgc_elapsed;        /* the elapsed time of the PGC, cell-relative */
    int                         pgc_nb_pg_est;      /* number of PGs as reported by IFOs */
    int                         pgcn;               /* ID of the PGC we are playing */
    int                         pgn;                /* ID of the PG we are in now */
    int                         ptm_discont;        /* signal that a PTM discontinuity occurred */
    int64_t                     ptm_offset;         /* PTM discontinuity offset (as NAV value) */
    int                         ptt;                /* ID of the chapter we are in now */
    uint32_t                    vobu_duration;      /* duration of the current VOBU */
    uint32_t                    vobu_e_ptm;         /* end PTM of the current VOBU */
    int                         vtsn;               /* ID of the active VTS (video title set) */
    uint64_t                    *pgc_pg_times_est;  /* PG start times as reported by IFO */
    pgc_t                       *pgc;               /* handle to the active PGC */
    dvdnav_t                    *dvdnav;            /* handle to the dvdnav VM */
    DVDVideoRand                rand;               /* the VM's random numbers (Rnd) */

    /* the following fields are only used for menu playback */
    int                         celln_start;        /* starting cell number */
    int                         celln_end;          /* ending cell number */
    int                         sector_offset;      /* current sector relative to the current VOB */
    uint32_t                    sector_end;         /* end sector relative to the current VOBU */
    uint32_t                    vobu_next;          /* the next VOBU pointer */
    uint32_t                    vobu_remaining;     /* remaining blocks for current VOBU */
    dvd_file_t                  *vob_file;          /* handle to the menu VOB (VMG or VTS) */
} DVDVideoPlaybackState;

typedef struct DVDVideoDemuxContext {
    const AVClass               *class;

    /* options */
    int                         opt_angle;          /* the user-provided angle number (1-indexed) */
    int                         opt_chapter_end;    /* the user-provided exit PTT (0 for last) */
    int                         opt_chapter_start;  /* the user-provided entry PTT (1-indexed) */
    int                         opt_menu;           /* demux menu domain instead of title domain */
    int                         opt_menu_lu;        /* the menu language unit (logical grouping) */
    int                         opt_menu_vts;       /* the menu VTS, or 0 for VMG (main menu) */
    int                         opt_pg;             /* the user-provided PG number (1-indexed) */
    int                         opt_pgc;            /* the user-provided PGC number (1-indexed) */
    int                         opt_preindex;       /* pre-indexing mode (2-pass read) */
    int                         opt_region;         /* the user-provided region digit */
    int                         opt_title;          /* the user-provided title number (1-indexed) */
    int                         opt_trim;           /* trim padding cells at beginning */
    int                         opt_read_attempts;  /* read attempts per request on the disc */
    int                         opt_udf_reader;     /* enum DiscIOUDFReader */
    int                         opt_prefer_iso;     /* DiscIOImageOptions.prefer_iso_for_old_udf102 */

    /* subdemux */
    AVFormatContext             *mpeg_ctx;          /* context for inner demuxer */
    uint8_t                     *mpeg_buf;          /* buffer for inner demuxer */
    FFIOContext                 mpeg_pb;            /* buffer context for inner demuxer */

    /* volume */
    struct DVDVideoSource       *source;            /* the disc: every read goes through it */
    dvd_reader_t                *dvdread;           /* handle to libdvdread */
    ifo_handle_t                *vmg_ifo;           /* handle to the VMG (VIDEO_TS.IFO) */
    ifo_handle_t                *vts_ifo;           /* handle to the active VTS (VTS_nn_n.IFO) */

    /* playback control */
    int64_t                     first_pts;          /* the PTS of the first video keyframe */
    int                         nb_angles;          /* number of angles in the current title */
    int                         play_started;       /* signal that playback has started */
    DVDVideoPlaybackState       play_state;         /* the active playback state */
    int64_t                     *prev_pts;          /* track the previous PTS emitted per stream */
    int64_t                     pts_offset;         /* PTS discontinuity offset (ex. VOB change) */
    int                         seek_warned;        /* signal that we warned about seeking limits */
    int                         subdemux_reset;     /* signal that subdemuxer should be reset */
} DVDVideoDemuxContext;

/* dvdvideo_source.c */
typedef struct DVDVideoSource DVDVideoSource;
/* Open the disc at path: a disc folder (holding VIDEO_TS, the VIDEO_TS folder,
 * or holding the DVD files directly) or a DVD-Video image file. */
int ff_dvdvideo_source_open(void *log, const char *path, const DiscIOImageOptions *opts, int attempts,
                            DVDVideoSource **out);
void ff_dvdvideo_source_close(DVDVideoSource **src);
/* A new set of file callbacks for DVDOpenFiles / dvdnav_open_files (on the
 * root "/"); the library that receives it closes it. NULL when out of memory. */
dvd_reader_filesystem_h *ff_dvdvideo_source_files(DVDVideoSource *src);
/* Descramble a 2048-byte block of title set vtsn (its menu VOBs when menu is
 * set) in place when it is CSS-scrambled; the key is looked for at the first
 * scrambled block. 0 = not scrambled, 1 = descrambled, < 0 = error (logged). */
int ff_dvdvideo_source_descramble(DVDVideoSource *src, int vtsn, int menu, uint8_t *block);
/* The path of a DVD file ("VTS_01_0.IFO") as libdvdread finds it: in the root
 * first, then in VIDEO_TS, names ignoring case. */
int ff_dvdvideo_source_find(DVDVideoSource *src, const char *name, char *path, size_t size);
/* A VOB group: the menu VOB of title set vtsn (menu set; VIDEO_TS.VOB for 0)
 * or its title VOBs. Disc folder: the files (title VOBs VTS_nn_1.VOB up to the
 * first missing one, read one after the other as libdvdread does). Disc image:
 * the image blocks the IFO places them at (a title set's menu VOBs up to its
 * title VOBs, the rest up to the end of the image). Reads block `sector` of the
 * group with the given read attempts (a partial last block of a file padded
 * with zeros): 0, AVERROR(ENOENT) when a folder has no file of the group,
 * AVERROR_EOF past its end, or a read error. */
int ff_dvdvideo_source_vob_read(DVDVideoSource *src, int vtsn, int menu, int64_t sector, uint8_t *buf,
                                int attempts);
/* The size in bytes of a VOB group (folder: its files; image: its range). */
int ff_dvdvideo_source_vob_bytes(DVDVideoSource *src, int vtsn, int menu, int64_t *bytes);
/* The image block of a file recorded as one extent (0xfffffffe for a run that
 * is not recorded); -1 for folders, a missing file or one in several extents. */
int64_t ff_dvdvideo_source_file_sector(DVDVideoSource *src, const char *name);

/* dvdvideo_css.c */
/* Offset of the scrambled PES of a sector: an MPEG-2 pack whose first PES is
 * private stream 1, video 0xE0 or MPEG audio 0xC0-0xC7 with its
 * PES_scrambling_control bits set; 0 for a usable pack that is not scrambled,
 * -1 for a sector that is not a usable pack. */
int ff_dvdvideo_css_scrambled_pes(const uint8_t *sec);
/* Whether a descrambled sector holds well-formed AC-3 or MPEG-2 video content. */
int ff_dvdvideo_css_content_valid(const uint8_t *sec);
/* Whether ff_dvdvideo_css_content_valid() can tell anything about a sector:
 * what it reads from the clear bytes is in place. */
int ff_dvdvideo_css_can_test(const uint8_t *sec);

/* dvdvideo_disc.c */
#define DVDVIDEO_NAV_READ_ATTEMPTS  3   /* read attempts of a NAV pack */
#define DVDVIDEO_IFO_READ_TRIES    16   /* tries per IFO block, alternating IFO / BUP */

typedef struct DVDVideoTitleSet {
    ifo_handle_t   *ifo;                /* NULL: not open (the title set cannot be used) */
    uint32_t       *vobu_starts;        /* the VOBU address map of the title VOBs, sorted, distinct */
    int             nb_vobu_starts;
    int             map_distrusted;     /* a cell or a lookup found the map incomplete */
    uint32_t        title_vobs_base;    /* images: image block of the title VOBs (0 = unknown, folders) */
    uint32_t        title_vobs_sectors; /* blocks of the title VOBs */
} DVDVideoTitleSet;

/* The whole disc as the navigation scan and the title plan see it. */
typedef struct DVDVideoDisc {
    void                *log;
    DVDVideoSource      *src;
    dvd_reader_t        *dvdread;
    DVDVideoTitleSet     vts[100];      /* [0] = the VMG (ifo only) */
    int                  nb_vts;
    struct AVTreeNode   *vobus;         /* VOBU records (dvdvideo_vobu.c) */
    struct AVTreeNode   *chains;        /* seamless-angle cells checked (dvdvideo_vobu.c) */
} DVDVideoDisc;

/* Open the VMG and every title set's IFO through the disc source (a title set
 * whose IFO cannot be opened is left out, with a warning), run the title-set
 * checks and gather what the scan and the title plan need. */
int ff_dvdvideo_disc_open(void *log, DVDVideoSource *src, DVDVideoDisc **disc);
void ff_dvdvideo_disc_close(DVDVideoDisc **disc);
/* Whether a block is a VOBU start of the title set's VOBU address map. */
int ff_dvdvideo_disc_is_vobu_start(const DVDVideoTitleSet *ts, uint32_t sector);
/* Program chain pgcn (by search pointer) of title set vtsn; NULL when none. */
const pgc_t *ff_dvdvideo_disc_pgc(const DVDVideoDisc *d, int vtsn, int pgcn);
/* Cell celln (1-based) of program chain pgcn (by search pointer) of title set
 * vtsn; NULL when there is none. */
const cell_playback_t *ff_dvdvideo_disc_cell(const DVDVideoDisc *d, int vtsn, int pgcn, int celln);
/* Whether the title VOBs of title set vts overlap those of another title set,
 * by the title search table's title set starts. */
int ff_dvdvideo_disc_title_vobs_overlap(const DVDVideoDisc *d, int vts);
/* A cell's first playback byte: block mode << 6 | block type << 4 | seamless
 * play << 3 | interleaved << 2 | STC discontinuity << 1 | seamless angle. */
int ff_dvdvideo_cell_flags(const pgc_t *pgc, int k);

/* dvdvideo_vobu.c */
#define DVDVIDEO_VOBU_NOT_A_NAV_PACK    0xffffffffU /* DVDVideoVobu.next of a block without a NAV pack */
#define DVDVIDEO_VOBU_NO_NEXT           0x10000000U /* distance part of next: the VOBU ends its cell */
#define DVDVIDEO_VOBU_END_OF_CELL       0xfffffffeU /* DVDVideoVobuStep.next of a cell's last VOBU */
#define DVDVIDEO_VOBU_ENDS_CELL         (-2)        /* ff_dvdvideo_vobu_register() next: end of cell */
#define DVDVIDEO_VOBU_POINTS_AT_ITSELF  (-3)        /* ff_dvdvideo_vobu_register() next: points at itself */

/* One VOBU as its NAV pack described it. */
typedef struct DVDVideoVobu {
    uint16_t vobu_ea;
    uint16_t ilvu_ea;
    uint32_t next;          /* bits 28..0 distance to the next VOBU (DVDVIDEO_VOBU_NO_NEXT: none), bits 30..29
                             * sml_pbi.category bits 14..13; DVDVIDEO_VOBU_NOT_A_NAV_PACK */
    uint32_t vobu_s_ptm;
    uint32_t vobu_e_ptm;
} DVDVideoVobu;

/* The NAV pack fields a VOBU record is made from. */
typedef struct DVDVideoNavFields {
    uint32_t next_vobu;     /* vobu_sri.next_vobu */
    uint32_t vobu_ea;       /* dsi_gi.vobu_ea */
    uint32_t ilvu_ea;       /* sml_pbi.ilvu_ea */
    uint16_t category;      /* sml_pbi.category */
    uint32_t vobu_s_ptm;
    uint32_t vobu_e_ptm;
} DVDVideoNavFields;

typedef struct DVDVideoVobuStep {
    uint32_t next;          /* the next VOBU's first block, or DVDVIDEO_VOBU_END_OF_CELL */
    uint32_t len;           /* blocks from this VOBU's start to the next */
} DVDVideoVobuStep;

void ff_dvdvideo_vobu_free(DVDVideoDisc *d);
/* The record of title-VOB block `sector` of title set vtsn: 1 found, 0 none. */
int ff_dvdvideo_vobu_get(const DVDVideoDisc *d, int vtsn, uint32_t sector, DVDVideoVobu *rec);
/* The first block above `sector` that has a record: 1 with *next, 0 none. */
int ff_dvdvideo_vobu_next_recorded(const DVDVideoDisc *d, int vtsn, uint32_t sector, uint32_t *next);
/* Store a record unless one is kept for the block. */
int ff_dvdvideo_vobu_put(DVDVideoDisc *d, int vtsn, uint32_t sector, const DVDVideoVobu *rec);
void ff_dvdvideo_nav_fields(const uint8_t *nav, DVDVideoNavFields *f);
/* Read the NAV pack at a title-VOB block from the disc (a blanked NAV pack that
 * still names its block counts); a block without one is recorded as such.
 * 1 = a NAV pack, 0 = none, < 0 = error. */
int ff_dvdvideo_vobu_read_nav(DVDVideoDisc *d, int vtsn, uint32_t sector, DVDVideoNavFields *f);
/* Record the VOBU whose NAV pack was read at `sector`: 1 with *next (the next
 * VOBU, DVDVIDEO_VOBU_ENDS_CELL, or DVDVIDEO_VOBU_POINTS_AT_ITSELF, which is
 * allowed only with allow_self and records nothing) and *len (vobu_ea + 1);
 * 0 when the NAV pack cannot be used (pointing at itself, or the VOBU after
 * one that ends its cell inside a continuing interleaved unit unreadable). */
int ff_dvdvideo_vobu_register(DVDVideoDisc *d, int vtsn, const DVDVideoNavFields *f, uint32_t sector,
                              int allow_self, int64_t *next, uint32_t *len);
/* The VOBU after `sector`: the own map while trusted, else two or more other
 * title sets covering the same VOB data when they agree, else the record, or
 * the NAV packs of this VOBU and the one after it. -1 = none, < -1 = error. */
int64_t ff_dvdvideo_vobu_next(DVDVideoDisc *d, int vtsn, uint32_t sector);
/* Whether `sector` is a VOBU start: by the own map while trusted (the first
 * miss makes it untrusted, with a warning), then by two or more covering title
 * sets, else by the NAV pack itself. */
int ff_dvdvideo_vobu_check_start(DVDVideoDisc *d, int vtsn, uint32_t sector);
/* The step from VOBU `sector` of a cell to the next one from the NAV packs. */
int ff_dvdvideo_vobu_step_from_nav(DVDVideoDisc *d, int vtsn, const cell_playback_t *cell, uint32_t sector,
                                   DVDVideoVobuStep *step);
/* For a plain seamless-angle cell: whether its first VOBUs follow each other
 * without gaps by their NAV packs (up to 5) and its last VOBU's NAV pack
 * reads; kept per cell. */
int ff_dvdvideo_vobu_chain_usable(DVDVideoDisc *d, int vtsn, const cell_playback_t *cell);
/* The step from VOBU `sector` of a cell to the next one: plain cells use the
 * VOBU map (seamless-angle cells only when their VOBU chain checks out), the
 * last VOBU, blocks and interleaved cells the NAV packs. 1 = a step, 0 = none. */
int ff_dvdvideo_vobu_step(DVDVideoDisc *d, int vtsn, const cell_playback_t *cell, uint32_t sector,
                          DVDVideoVobuStep *step);

/* dvdvideo_cells.c */
/* dvd_time_t as one 32-bit BCD value (hours in the top byte, the frame byte at
 * the bottom) and its whole seconds (frames dropped, nibbles not checked). */
uint32_t ff_dvdvideo_dvd_time(const dvd_time_t *t);
uint32_t ff_dvdvideo_bcd_secs(uint32_t t);
/* vobu_s_ptm / vobu_e_ptm of the VOBU at `sector` of cell i (index): from its
 * record, or its NAV pack read now; unless force, none for a cell that the
 * quick check rejects and the deep check fails. 1 / 0 none / < 0 error. */
int ff_dvdvideo_vobu_ptm(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i, uint32_t sector, int force,
                         uint32_t *s_ptm, uint32_t *e_ptm);
/* Duration of cell i by its NAV packs (vobu_e_ptm of its last VOBU minus
 * vobu_s_ptm of its first, 90 kHz): 1 / 0 when a time is missing or out of
 * order / < 0 error. */
int ff_dvdvideo_cell_duration(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i, int force, int quiet,
                              uint32_t *ticks);
/* The NAV-level check of cell i: its first, last and every VOBU between must be
 * NAV packs, and the first and last VOBU times in order. 1 = failed. */
int ff_dvdvideo_cell_deep_check_failed(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i);
/* Whether cell i (index) of pgc is rejected as content: sectors out of order or
 * outside the title VOBs, a VOBU recorded as no NAV pack, no playback time,
 * short cells with cell commands, a uniform filler cell, the deep check, no
 * NAV times. quick leaves out the checks that read NAV packs; prev is the cell
 * played before it (-1 none; used for a cell of one second or less). */
int ff_dvdvideo_cell_rejected(DVDVideoDisc *d, int vtsn, const pgc_t *pgc, int i, int quick, int prev);
/* Whether stepping interleaved unit by interleaved unit from `from` lands
 * exactly on `to`. */
int ff_dvdvideo_ilvu_chain_reaches(DVDVideoDisc *d, int vtsn, uint32_t from, uint32_t to);
/* Whether cell command cmd_nr (1-based) of pgc may link or jump away (not a
 * no-op link and not a compare that can never be true). */
int ff_dvdvideo_cell_cmd_may_link(const pgc_t *pgc, int cmd_nr);
/* Whether program chain pgcn of title set vtsn is rejected as not real
 * content: 1 with *reason = no cells (0), a protection pattern of many
 * discontinuous cells on shared title VOBs (1, 2), too little data per second
 * of play (3, 4), a broken program map (5, 6), cells that should join
 * seamlessly but do not (7, 8, 9); 0 accepted; < 0 error. */
int ff_dvdvideo_pgc_rejection(DVDVideoDisc *d, int vtsn, int pgcn, const pgc_t *pgc, int *reason);

/* dvdvideo_scan.c */
/* Whether a 2048-byte block is a NAV pack (pack header, system header, PCI and
 * DSI packets where DVD-Video puts them). */
int ff_dvdvideo_is_nav_pack(const uint8_t *block);
/* The sequence after srand(seed) (seed 0 counts as 1, as in glibc). */
void ff_dvdvideo_rand_init(DVDVideoRand *r, uint32_t seed);
/* The next value, 0..RAND_MAX (r is a DVDVideoRand); fits dvdnav_set_random_source(). */
int ff_dvdvideo_rand_next(void *r);

typedef struct DVDVideoScanResult {
    char     name[16];      /* the start point: letters for the position, then the button / title number */
    int      title;         /* title of the title search table (low byte) */
    int      pgcn;          /* program chain of the title set (low byte) */
    uint8_t *cells;         /* cell numbers in the order played, repeats removed */
    int      nb_cells;
} DVDVideoScanResult;

typedef struct DVDVideoScan {
    DVDVideoScanResult *results;    /* one per (title, pgcn), in that order */
    int                 nb_results;
    int                 failed;     /* a refused read or broken navigation data ended the scan */
    char                failure[512];
    uint8_t             entered[100]; /* title sets (and the VMG, [0]) whose cells the navigation played */
} DVDVideoScan;

/* Receives a trace of the scan: one line per result of a navigator call
 * ("nav <n>" after a copy, "set", "ev <code> <time_ms> <title> <pgc> <vtsn>
 * <cell> <still> <highlight> <vobu> <sprm flags>"), per sector read
 * ("read <vob id> <sector>", before the result of the call that read it),
 * per walker log line ("log pending <key>", "log path <key>") and
 * "fail <reason>". */
typedef void (*DVDVideoScanTrace)(void *opaque, const char *line);
/* Scan the disc's navigation. A scan that failed is returned with failed set
 * and no results; < 0 for an error that kept it from running. */
int ff_dvdvideo_scan(void *log, DVDVideoDisc *disc, DVDVideoScanTrace trace, void *trace_opaque,
                     DVDVideoScan **scan);
void ff_dvdvideo_scan_free(DVDVideoScan **scan);

/* dvdvideo_titles.c */
#define DVDVIDEO_INVALID_SEGMENT    0x7fffff01  /* DVDVideoChapter.segment of a chapter in no segment */

enum DVDVideoCellMode {             /* how a title's cells are chosen */
    DVDVIDEO_CELLS_AUTO,            /* cell walk with scan results, else cell trim */
    DVDVIDEO_CELLS_WALK,            /* the cells the navigation scan played */
    DVDVIDEO_CELLS_TRIM,            /* cells that do not look like content trimmed off (no scan) */
    DVDVIDEO_CELLS_WALK_TRIM,       /* cell walk, cell trim where it fails */
};

enum DVDVideoTitleOrder {
    DVDVIDEO_ORDER_AUTO,            /* scan-first with the automatic / cell-walk modes, else table */
    DVDVIDEO_ORDER_SCAN_FIRST,      /* the titles the scan reached first, then the others */
    DVDVIDEO_ORDER_TABLE,           /* the title search table's order */
};

enum DVDVideoTitleEventKind {       /* findings of the title stage */
    DVDVIDEO_EV_PTT_NO_TITLE,       /* ttn, vtsn: the title set has no parts of title for its title */
    DVDVIDEO_EV_PTT_UNRESOLVED,     /* vtsn, ttn, pgcn, pgn: a part of title is left out */
    DVDVIDEO_EV_TOO_MANY_AUDIO,     /* n, vtsn */
    DVDVIDEO_EV_TOO_MANY_SUBP,      /* n (the audio count), vtsn */
    DVDVIDEO_EV_TITLE_EMPTY,        /* title index (0-based) */
    DVDVIDEO_EV_TITLE_SET_MISSING,  /* title number */
    DVDVIDEO_EV_NO_CELL_LIST,       /* name, declared time */
    DVDVIDEO_EV_TITLE_UNUSABLE,     /* name */
    DVDVIDEO_EV_ANGLE_BLOCK_BROKEN, /* cell */
    DVDVIDEO_EV_ANGLE_COUNT,        /* cell, angles of the title, cells of the block */
    DVDVIDEO_EV_VOBUS_TO_READ,      /* cells, VOBUs */
    DVDVIDEO_EV_SHORT,              /* name, seconds, minimum */
    DVDVIDEO_EV_FAKE_LENGTH,        /* name, declared, measured */
    DVDVIDEO_EV_DUPLICATE,          /* vtsn, #kept, #dropped */
    DVDVIDEO_EV_TITLE,              /* name, cells, declared time */
    DVDVIDEO_EV_CELLWALK_FAILED,
    DVDVIDEO_EV_CELLS_CUT_START,    /* cells left out at the start */
    DVDVIDEO_EV_CELLS_CUT_END,      /* first cell left out at the end, cells of the chain */
    DVDVIDEO_EV_FAKE_CELLS,         /* percent */
    DVDVIDEO_EV_ANGLE,              /* angle, name */
    DVDVIDEO_EV_ANGLE_FAILED,       /* angle, name */
    DVDVIDEO_EV_NAV_INVALID,        /* playing time where it happens */
    DVDVIDEO_EV_TITLE_SET_INVALID,  /* title number, vtsn, title sets of the disc */
    DVDVIDEO_EV_TITLE_SET_START,    /* vtsn, this start sector (a title's, or the disc layout's), the first one */
    DVDVIDEO_EV_IFO_CORRUPT,        /* IFO file name, byte offset */
};

enum DVDVideoNotSelected {
    DVDVIDEO_TITLE_SELECTABLE,
    DVDVIDEO_TITLE_SHORT,           /* shorter than the minimum length */
    DVDVIDEO_TITLE_FAKE,            /* declared and measured lengths far apart */
};

typedef struct DVDVideoChapter {
    int      pgcn;
    int      pgn;
    int      cell;                  /* its first cell (index), -1 none */
    uint32_t segment;               /* the segment it starts in, DVDVIDEO_INVALID_SEGMENT */
    uint64_t offset;                /* byte offset of its start in that segment */
    uint64_t time;                  /* from the title start, 1/1,080,000,000 s */
} DVDVideoChapter;

typedef struct DVDVideoExtent {
    uint32_t logical;               /* block within the segment's data */
    uint32_t sector;                /* title-VOB block */
    uint32_t count;
} DVDVideoExtent;

/* One segment of a title: the title-VOB blocks played without a discontinuity. */
typedef struct DVDVideoSegment {
    DVDVideoExtent *extents;
    int             nb_extents;
    int             last_cell;      /* the last cell whose VOBUs were added */
    int           (*runs)[2];       /* runs of cell numbers, while building */
    int             nb_runs;
    uint64_t        size;           /* bytes */
    char            label[256];     /* the cells it plays: "1-19", "(1-3,5)" */
} DVDVideoSegment;

typedef struct DVDVideoTitle {
    int              index;         /* title search table entry (0-based) */
    char             name[16];      /* "3", or "3/1" for a further program chain of entry 3 */
    int              title_type;    /* title_info_t pb_ty byte */
    int              vtsn, vts_ttn;
    int              angle;         /* 0-based */
    int              angles;
    int              pgcn;
    DVDVideoChapter *chapters;
    int              nb_chapters;
    int              scan;          /* its result in the navigation scan, -1 none */
    uint8_t          audio[8];      /* audio stream numbers */
    int              nb_audio;
    int              audio_mask;
    uint8_t          subp[64];      /* subpicture stream numbers */
    int              nb_subp;
    int             *cells;         /* cells played (indices in the program chain) */
    int              nb_cells;
    DVDVideoSegment *segments;
    int              nb_segments;
    int              segments_built;
    uint32_t         declared_secs; /* sum of the cells' playback times */
    uint32_t         measured_secs; /* sum of the cells' NAV-pack durations */
    uint32_t         nav_invalid_count;
    int              not_selected;  /* enum DVDVideoNotSelected */
} DVDVideoTitle;

typedef struct DVDVideoTitleEvent {
    int  kind;                      /* enum DVDVideoTitleEventKind */
    char args[128];                 /* its arguments, tab-separated */
} DVDVideoTitleEvent;

typedef struct DVDVideoTitlePlan {
    DVDVideoTitle      *titles;
    int                 nb_titles;
    DVDVideoTitleEvent *events;
    int                 nb_events;
} DVDVideoTitlePlan;

typedef struct DVDVideoTitleOptions {
    int cell_mode;                  /* enum DVDVideoCellMode */
    int title_order;                /* enum DVDVideoTitleOrder */
    int min_length;                 /* seconds; shorter titles are listed, not selected */
} DVDVideoTitleOptions;

/* The titles of the disc (scan: the navigation scan, NULL when none ran). */
int ff_dvdvideo_titles_plan(void *log, DVDVideoDisc *disc, const DVDVideoScan *scan, const DVDVideoTitleOptions *opt,
                            DVDVideoTitlePlan **plan);
void ff_dvdvideo_titles_free(DVDVideoTitlePlan **plan);
const char *ff_dvdvideo_event_name(int kind);
/* Whether a title's declared and measured lengths are far apart: the declared
 * length is 0, or they differ by more than 300 s and more than 30 % of it. */
int ff_dvdvideo_lengths_disagree(uint32_t declared, uint32_t measured);

/* dvdvideo_ifo.c */
#define DVDVIDEO_VTS_MAP_DISTRUSTED  1  /* a cell does not start / end on a VOBU of the map */
#define DVDVIDEO_VTS_CELL_PAST_VOBS  2  /* a cell ends past the end of the title VOBs */
/* The checks of a title set's IFO: every cell of its PGCs must start
 * (first_sector) and end (last_vobu_start_sector) on VOBUs its VOBU address map
 * lists, and must end inside the title VOBs. Logs the first finding of each
 * kind; returns the DVDVIDEO_VTS_* flags found. */
int ff_dvdvideo_check_vts(void *log, dvd_reader_t *dvd, int vtsn, const ifo_handle_t *ifo);
void ff_dvdvideo_ifo_close(AVFormatContext *s);
int ff_dvdvideo_ifo_open(AVFormatContext *s);
int ff_dvdvideo_is_cell_promising(AVFormatContext *s, pgc_t *pgc, int celln);
int ff_dvdvideo_is_pgc_promising(AVFormatContext *s, pgc_t *pgc);

/* dvdvideo_play.c */
void ff_dvdvideo_menu_close(AVFormatContext *s, DVDVideoPlaybackState *state);
int ff_dvdvideo_menu_open(AVFormatContext *s, DVDVideoPlaybackState *state);
int ff_dvdvideo_menu_next_ps_block(AVFormatContext *s, DVDVideoPlaybackState *state,
                                       uint8_t *buf, int buf_size, int *p_is_nav_packet);
void ff_dvdvideo_play_close(AVFormatContext *s, DVDVideoPlaybackState *state);
int ff_dvdvideo_play_open(AVFormatContext *s, DVDVideoPlaybackState *state);
int ff_dvdvideo_play_next_ps_block(AVFormatContext *s, DVDVideoPlaybackState *state,
                                       uint8_t *buf, int buf_size, int *p_is_nav_packet);

/* dvdvideo_chapters.c */
int ff_dvdvideo_chapters_setup_simple(AVFormatContext *s);
int ff_dvdvideo_chapters_setup_preindex(AVFormatContext *s);

/* dvdvideo_streams.c */
int ff_dvdvideo_video_stream_setup(AVFormatContext *s);
int ff_dvdvideo_audio_stream_add_all(AVFormatContext *s);
int ff_dvdvideo_subp_stream_add_all(AVFormatContext *s);

#endif /* AVFORMAT_DVDVIDEO_INTERNAL_H */
