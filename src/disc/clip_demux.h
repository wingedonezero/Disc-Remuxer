/*
 * The clip demuxer (the disc layer): reads a clip of an opened title — a
 * program-stream clip as 2048-byte sectors (mux/ps_disc.h), a Blu-ray clip
 * as 192-byte source packets (mux/ts_disc.h) — from the caller's block
 * reader and gives out raw PES payloads with their own times: no parsing,
 * no filled-in or corrected timestamps (the title timeline times every
 * unit).
 *
 * Program streams: each payload leaves with the id of its stream (the start
 * code 0x1xx of an MPEG stream, the sub-stream id of a private stream 1
 * stream, 0xFD00 | stream_id_extension for VC-1); what carries no stream
 * (padding, navigation, system headers, unused ids) is counted
 * (MpegPsDiscStats). Transport streams: each payload piece leaves as it
 * arrives, its id PID | PES stream key << 16; the stretches left out are
 * logged, or kept for dr_clip_gap() (gaps mode).
 */

#ifndef DISC_DISC_CLIP_DEMUX_H
#define DISC_DISC_CLIP_DEMUX_H

#include <stdint.h>

#include "base/err.h"
#include "mux/ps_disc.h"
#include "mux/ts_disc.h"
#include "timeline/disctl.h"

/** No time (== the processing library's AV_NOPTS_VALUE, so values cross the
 *  seam unchanged). */
#define DR_CLIP_NOPTS INT64_MIN

typedef struct DrClipDemux DrClipDemux;

/** A PES payload as the clip demuxer gives it. data stays valid until the
 *  next dr_clip_read() or dr_clip_close(); a caller that keeps it copies. */
typedef struct DrClipPacket {
    const uint8_t *data;
    int            size;
    int            id;      /**< PS: the stream's id; TS: PID | PES stream key << 16 */
    int64_t        pts;     /**< 33 bits as recorded, DR_CLIP_NOPTS none */
    int64_t        pos;     /**< byte of the stream read (what the block reader gave) */
    int            kind;    /**< PS: the stream's DR_KIND_*; TS: 0 */
    DRCodecID      codec;   /**< PS: the stream's codec; TS: DR_CODEC_NONE */
} DrClipPacket;

/** Bytes of the clip's stream into buf (any amount up to size):
 *  DR_ERR_EOF (or 0) at its end, < 0 an error. */
typedef int (*DrClipRead)(void *opaque, uint8_t *buf, int size);

/** A program-stream clip (2048-byte sectors, the disc reader's pack rules);
 *  lpcm_header: the format's LPCM audio frame header size (3 DVD-Video,
 *  5 HD DVD). logctx: the log context its lines carry. */
int dr_clip_ps_open(DrClipDemux **c, void *logctx, int lpcm_header, DrClipRead read, void *opaque);

/** A transport-stream clip (192-byte source packets): the PIDs read;
 *  dialog: they are text subtitles (HDMV TextST: a PES packet without a PTS
 *  that starts with a dialog presentation segment takes that dialog's start
 *  time); gaps: the stretches left out are kept for dr_clip_gap() instead
 *  of logged. */
int dr_clip_ts_open(DrClipDemux **c, void *logctx, const int *pids, int nb_pids, int dialog, int gaps,
                    DrClipRead read, void *opaque);

/** The next payload: 0, DR_ERR_EOF at the clip's end, < 0 an error (the
 *  reading's own are DR_ERR_* values; the block reader's pass through). */
int dr_clip_read(DrClipDemux *c, DrClipPacket *pkt);

/** Gaps mode: the oldest stretch left out that has ended (1, *gap filled),
 *  0 none. flush: the stream has ended, the stretches still open end with
 *  it. */
int dr_clip_gap(DrClipDemux *c, int flush, MpegTSDiscGap *gap);

/** What a program-stream clip's reading counted so far. */
void dr_clip_ps_stats(const DrClipDemux *c, MpegPsDiscStats *st);

void dr_clip_close(DrClipDemux **c);

#endif /* DISC_DISC_CLIP_DEMUX_H */
