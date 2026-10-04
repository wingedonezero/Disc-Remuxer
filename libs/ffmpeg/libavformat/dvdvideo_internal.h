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

    /* subdemux */
    AVFormatContext             *mpeg_ctx;          /* context for inner demuxer */
    uint8_t                     *mpeg_buf;          /* buffer for inner demuxer */
    FFIOContext                 mpeg_pb;            /* buffer context for inner demuxer */

    /* volume */
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

/* dvdvideo_ifo.c */
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
