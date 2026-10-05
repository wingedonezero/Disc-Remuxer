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
 * or its title VOBs VTS_nn_1.VOB up to the first missing one, read one after
 * the other as libdvdread does. Reads block `sector` of the group with the
 * given read attempts (a partial last block of a file padded with zeros):
 * 0, AVERROR(ENOENT) when the group has no file, AVERROR_EOF past its end, or
 * a read error. */
int ff_dvdvideo_source_vob_read(DVDVideoSource *src, int vtsn, int menu, int64_t sector, uint8_t *buf,
                                int attempts);
/* A VOB group's size in blocks and, on an image, the image block it starts at
 * when the whole group is one run of image blocks (else -1; -1 for folders). */
int ff_dvdvideo_source_vob_layout(DVDVideoSource *src, int vtsn, int menu, int64_t *sectors,
                                  int64_t *image_sector);
/* The image block a file of the disc starts at; -1 for folders, a missing file
 * or one without recorded data. */
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

/* dvdvideo_scan.c */
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
int ff_dvdvideo_scan(void *log, DVDVideoSource *src, DVDVideoScanTrace trace, void *trace_opaque,
                     DVDVideoScan **scan);
void ff_dvdvideo_scan_free(DVDVideoScan **scan);

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
