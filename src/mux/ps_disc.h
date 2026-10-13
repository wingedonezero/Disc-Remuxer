/*
 * MPEG-2 program stream packs as a disc reader reads them (ps_disc.c).
 */

#ifndef DISC_MUX_PS_DISC_H
#define DISC_MUX_PS_DISC_H

#include <stdint.h>

/* ---- Disc sectors: DVD-Video VOBs, HD DVD EVOBs ----
 * One 2048-byte sector = one MPEG-2 program stream pack, read with the rules
 * of the reference's disc reader. */

#define MPEGPS_DISC_SECTOR 2048

typedef struct MpegPsDiscPacket {
    int         len;            /**< PES_packet_length + 6; 0 when the field is 0 */
    int         key;            /**< stream_id; private stream 1: sub-stream id << 8 | 0xbd; with a PES
                                     extension 2: stream_id_extension << 8 | stream_id */
    int         pts_present;    /**< PTS_DTS_flags say there is a PTS */
    int         pts_valid;      /**< and its marker bits are set (pts is 0 otherwise) */
    uint64_t    pts;            /**< 33 bits, 90 kHz */
    int         payload;        /**< payload offset in the packet (after the sub-stream header and an
                                     LPCM audio frame header) */
    int         first_au;       /**< offset of the first access unit in the payload, -1 none, -2 sub-picture */
    int         extra;          /**< LPCM: bytes of the audio frame header just before payload */
    int         at;             /**< offset of the packet in the sector */
    const char *why;            /**< when the packet does not parse: why */
} MpegPsDiscPacket;

/** The packet at *off of sector s (0 = the sector's start: the pack header
 *  first). *off becomes the next packet's offset, MPEGPS_DISC_SECTOR (or more)
 *  when the sector is done: after private stream 2, at fill bytes (0x00 or
 *  0xff to the end) and after padding packets. lpcm5: the LPCM audio frame
 *  header is 5 bytes (HD DVD), else 3 (DVD-Video). 0 when it does not parse
 *  (p->why says why): an LPCM packet shorter than its header or whose first
 *  access unit starts inside the header does not parse; one that holds only
 *  the header is a padding packet. */
int ff_mpegps_disc_packet(const uint8_t *s, int *off, MpegPsDiscPacket *p, int lpcm5);

/** A packet of a sector: < 0 stops the sector with that error. */
typedef int (*MpegPsDiscPacketCb)(void *opaque, const uint8_t *sec, const MpegPsDiscPacket *p);

enum MpegPsDiscStatus {
    MPEGPS_DISC_OK,               /**< every packet given to the callback */
    MPEGPS_DISC_EMPTY,            /**< all 0x00 or all 0xff: no packets */
    MPEGPS_DISC_BAD,              /**< its first packet runs past its end (a short header): left out */
    MPEGPS_DISC_REST_DROPPED,     /**< no packet starts after a video / private stream 1 packet: the rest
                                     of the sector is left out */
    MPEGPS_DISC_FAIL,             /**< not readable as a disc pack */
};

/** The packets of one sector to cb (padding, private stream 2 and system
 *  header packets too), with the reader's rules: an empty sector has none;
 *  at most two packets unless one is MPEG audio (stream_id 0xD0-0xDF); a
 *  packet that runs past the sector's end leaves the sector out (a short
 *  header) or fails; bytes that are no packet start after a video (0xE0)
 *  or private stream 1 packet drop the rest of the sector, elsewhere they
 *  fail. Returns a MpegPsDiscStatus (*why, *at: what and where), or cb's
 *  error. */
int ff_mpegps_disc_sector(const uint8_t *sec, int lpcm5, MpegPsDiscPacketCb cb, void *opaque, const char **why,
                           int *at);


/** What a program-stream disc reading counts: the sectors, and the packets
 *  given to no stream (filled by the caller that maps packets to streams,
 *  e.g. our FFmpeg's mpeg.c disc mode). */
typedef struct MpegPsDiscStats {
    int64_t sectors;                 /**< sectors read */
    int64_t empty;                   /**< of them empty (all 0x00 or all 0xff) */
    int64_t bad;                     /**< left out: the first packet runs past the sector (a short header) */
    int64_t rest_dropped, rest_bytes;/**< sectors whose rest was left out, and those bytes */
    int64_t nav, nav_bytes;          /**< private stream 2 packets (DVD navigation), to the sector's end */
    int64_t padding, padding_bytes;  /**< padding packets, to the sector's end */
    int64_t system, system_bytes;    /**< system headers */
    int64_t other, other_bytes;      /**< other packets of no stream (unused stream ids) */
    int64_t discarded, discarded_bytes; /**< packets of streams the caller discards */
} MpegPsDiscStats;

#endif /* DISC_MUX_PS_DISC_H */
