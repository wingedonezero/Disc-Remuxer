/*
 * Blu-ray transport streams as a disc reader reads them (ts_disc.c):
 * 192-byte source packets, and for each PID asked for the PES packets put
 * together the way the reference disc reader does, their payload pieces
 * given out as they arrive. What cannot be read is reported as stretches of
 * the stream file (MpegTSDiscGap) through the caller's events.
 *
 * The clip demuxer (src/disc/clip_demux.c) runs this; the tests are
 * tests/unit/test_mpegts_disc.py.
 */

#ifndef DISC_MUX_TS_DISC_H
#define DISC_MUX_TS_DISC_H

#include <stdint.h>

#define TS_DISC_SRC_PACKET 192

/* "no PTS": the same value as FFmpeg's AV_NOPTS_VALUE, so the wrapper
 * assigns it straight through. */
#define TS_DISC_NOPTS INT64_MIN

/** Disc mode, a stretch of the stream file left out: TS packets that cannot
 *  be read (one after the other, for one reason), all-zero source packets,
 *  or a PID's data dropped from a PES packet that cannot be read to its next
 *  PES start. */
typedef struct MpegTSDiscGap {
    int         kind;           /**< MPEGTS_DISC_GAP_* */
    int         pid;            /**< MPEGTS_DISC_GAP_PES: the PID */
    int64_t     pos, end;       /**< the stream's bytes [pos, end): its source packets; PES: from the packet
                                     where reading failed to the one with the next PES start */
    int64_t     packets;        /**< source packets in it (BAD, ZERO) */
    int64_t     bytes;          /**< PES: the PID's payload bytes left out; CUT: the PES bytes missing */
    const char *why;            /**< BAD, PES: why (a static string) */
    int         to_end;         /**< PES: the stream ended before a next PES start */
    int         cc_from, cc_to; /**< CC: the PID's continuity counter before and in the packet at pos */
} MpegTSDiscGap;

/** MPEGTS_DISC_GAP_CUT: a PES packet whose next PES start comes before its
 *  end (pos: that start's packet; bytes: the bytes of it that did not come);
 *  nothing is left out, its rest is missing. */
/** MPEGTS_DISC_GAP_PTS: a PES header (at pos) whose flags say it has a PTS
 *  but whose marker bits are not set: read as having no time (nothing left
 *  out). */
/** MPEGTS_DISC_GAP_CC: the TS packet of a read PID at pos carries a payload
 *  and a continuity counter (cc_to) that does not follow the PID's last one
 *  (cc_from) by one, without a discontinuity indicator: packets <= 15
 *  (cc_to - cc_from - 1 modulo 16; 0 when the counter did not advance but the
 *  bytes differ) are missing before it (ISO/IEC 13818-1 2.4.3.3); its data
 *  is kept. */
/** MPEGTS_DISC_GAP_DUP: the TS packet at pos repeats the PID's packet before
 *  it byte for byte (a duplicate packet, ISO/IEC 13818-1 2.4.3.3): left out. */
enum { MPEGTS_DISC_GAP_BAD = 1, MPEGTS_DISC_GAP_ZERO, MPEGTS_DISC_GAP_PES, MPEGTS_DISC_GAP_CUT,
       MPEGTS_DISC_GAP_PTS, MPEGTS_DISC_GAP_CC, MPEGTS_DISC_GAP_DUP };

/**
 * A PES header read with the disc reader's rules: 1 and its stream key
 * (stream_id, or 0xFD | stream_id_extension << 8) and PTS (33 bits as
 * recorded, TS_DISC_NOPTS none), 0 not usable. avail: the bytes at p.
 */
int ff_mpegts_disc_pes_header(const uint8_t *p, unsigned avail, uint16_t *key, int64_t *pts);

/** The reading's events; a negative return from emit or gap stops the
 *  reading with that error. */
typedef struct TsDiscEvents {
    /** A PES packet of a read PID begins (its stream key is known; its
     *  pieces follow through emit). */
    int  (*pes)(void *opaque, int pid, uint16_t key);
    /** A payload piece of a PES packet of a read PID: the PES packet's PTS
     *  comes with its first piece that has bytes (TS_DISC_NOPTS none, or
     *  else 33 bits as recorded); pos is the byte of the source packet the
     *  piece is in. */
    int  (*emit)(void *opaque, int pid, uint16_t key, const uint8_t *data, unsigned len, int64_t pts, int64_t pos);
    /** A stretch left out that has ended (kinds BAD / ZERO / PES), or an
     *  instantaneous record (CUT, PTS). */
    int  (*gap)(void *opaque, const MpegTSDiscGap *g);
    /** A PID's data starts being dropped until its next PES start (the
     *  stretch comes as a gap when that start arrives). */
    void (*drop)(void *opaque, int pid, const char *why);
    /** A note of the reading, in words (pid -1: none). */
    void (*note)(void *opaque, int pid, const char *text);
    /** What stops the reading, in words (the error is then returned). */
    void (*error)(void *opaque, const char *text);
    void  *opaque;
} TsDiscEvents;

typedef struct TsDisc TsDisc;

/** The reading of one stream file: the PIDs read, of them the text-subtitle
 *  PIDs (HDMV TextST: a PES packet without a PTS that starts with a dialog
 *  presentation segment takes that dialog's start time). NULL when out of
 *  memory. */
TsDisc *ts_disc_new(const int *pids, unsigned nb_pids, const int *dialog_pids, unsigned nb_dialog,
                    const TsDiscEvents *ev);

/** One 192-byte source packet at byte pos of the stream file. 0, or a
 *  negative error (the events' own, or the data's). */
int ts_disc_source_packet(TsDisc *t, const uint8_t *p, int64_t pos);

/** The stream has ended at byte end: the stretches still open end with it
 *  (their gap events come now). May be called again (nothing then). */
int ts_disc_flush(TsDisc *t, int64_t end);

/** The caller left source packets out before the next one (blocks that
 *  cannot be read): the continuity counters start again. */
void ts_disc_break(TsDisc *t);

void ts_disc_free(TsDisc **t);

#endif /* DISC_MUX_TS_DISC_H */
