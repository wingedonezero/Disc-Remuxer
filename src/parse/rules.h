/*
 * The frame cutters' rules: how each codec's stream from a disc is cut into
 * units, as data (function tables) the splitter (parse/split.h) runs. Log
 * lines leave through base/vlog.h; rule errors are base/err.h values.
 */

#ifndef DISC_PARSE_RULES_H
#define DISC_PARSE_RULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A cut rule: units found by walking the stream from a known start.
 */
typedef struct DrCutRule {
    /** Size of the unit at the start of buf (avail bytes; final: no more
     *  bytes will come): > 0 its size, 0 more bytes needed, DR_CUT_NONE no
     *  unit starts there, another < 0 the stream cannot be cut (logged). */
    int (*unit_size)(void *logctx, const uint8_t *buf, int avail, int final, void *state);
    /** Where the next unit may start in buf (avail bytes), < 0 when not
     *  within them. */
    int (*resync)(const uint8_t *buf, int avail);
    int state_size;                     /**< bytes of the rule's own state (zeroed by the engine) */
} DrCutRule;

#define DR_CUT_NONE (-1)

/**
 * A frame rule, in the disc reader's four steps, for codecs whose frames
 * are found by a sync word: sync (0 no frame starts here, 1 one does, 2 too
 * few bytes to tell), size (> 0 the frame's size, <= 0 more bytes needed),
 * check (1 the frame is valid), resync (bytes to skip to the next frame
 * start, -2 more bytes needed, -1 give the kept bytes up), and limit: the
 * bytes kept while looking for a frame (more coming in: the kept bytes and
 * the new ones are given up).
 */
typedef struct DrFrameRule {
    int (*sync)(void *ctx, const uint8_t *buf, int avail);
    int (*size)(void *ctx, const uint8_t *buf, int avail);
    int (*check)(void *ctx, const uint8_t *buf, int size);
    int (*resync)(void *ctx, const uint8_t *buf, int avail);
    int limit;
} DrFrameRule;

/* The frame rules' own contexts (the caller owns one per track): */
typedef struct DrAc3RuleCtx {
    int kind;               /**< frames taken: 0 AC-3 and E-AC-3, 1 AC-3 only, 2 E-AC-3 only */
} DrAc3RuleCtx;
typedef struct DrDcaRuleCtx {
    int undecided;          /**< DTS-HD: whether an extension substream follows each core frame is still open */
    int ext;                /**< and it does */
} DrDcaRuleCtx;
typedef struct DrMpaRuleCtx {
    int dvd;                /**< a DVD-Video disc's stream: only Layer II, 48 kHz, not dual channel */
    int free_size;          /**< the free-format frame size found (0 none yet) */
} DrMpaRuleCtx;

/** dr_pcm_dvd_cut's state: the LPCM frame format the caller sets (while it
 *  is not set the bytes are kept). */
typedef struct DrPcmDvdCut {
    int channels, bits, samples;
} DrPcmDvdCut;

/* The rules (one file each; the cutting is described there): */
extern const DrCutRule   dr_mlp_cut;          /**< TrueHD / MLP access units */
extern const DrCutRule   dr_dvdsub_cut;       /**< DVD / HD DVD sub-picture units */
extern const DrCutRule   dr_vc1_cut;          /**< VC-1 frame units (start-code walk) */
extern const DrCutRule   dr_pgs_cut;          /**< PGS display sets */
extern const DrCutRule   dr_pcm_bluray_cut;   /**< Blu-ray LPCM units */
extern const DrCutRule   dr_pcm_dvd_cut;      /**< DVD / HD DVD LPCM frames (state: DrPcmDvdCut) */
extern const DrFrameRule dr_adts_frames;      /**< AAC ADTS frames */
extern const DrFrameRule dr_latm_frames;      /**< AAC LATM / LOAS frames */
extern const DrFrameRule dr_textst_frames;    /**< TextST segments (one per unit) */
extern const DrFrameRule dr_ac3_frames;       /**< AC-3 / E-AC-3 syncframes (ctx: DrAc3RuleCtx) */
extern const DrFrameRule dr_dts_frames;       /**< DTS core frames (ctx: DrDcaRuleCtx) */
extern const DrFrameRule dr_dts_hd_frames;    /**< DTS-HD: core + extension substream (ctx: DrDcaRuleCtx) */
extern const DrFrameRule dr_dts_exss_frames;  /**< DTS-HD without a core (DTS Express, lossless only):
                                                   extension substream frames */
extern const DrFrameRule dr_mpa_frames;       /**< MPEG audio frames (ctx: DrMpaRuleCtx) */

/** The stream header from an AC-3 / E-AC-3 frame: E-AC-3 bitstream
 *  information up to the channel map (the frame must hold it; a dependent
 *  stream's channel map must give the channels its audio coding mode and
 *  LFE give); AC-3: the syncinfo. 1 valid, 0 not. */
int dr_ac3_stream_header(const uint8_t *buf, int size);

/** An AC-3 / E-AC-3 syncframe's header (A/52 5.4.1, Annex E E.1.2), as far
 *  as the timeline reads it. */
typedef struct DrAc3Header {
    int bitstream_id;
    int frame_type;         /**< E-AC-3 strmtyp (0 independent, 1 dependent); an AC-3 frame reads as 2 */
    int frame_size;         /**< bytes */
    int rate;               /**< sample rate */
    int blocks;             /**< audio blocks of 256 samples */
} DrAc3Header;

/** The header of the frame at buf: 0, or DR_ERR_INVALID when it is not one
 *  the timeline takes (bad sync / sizes / rates; an E-AC-3 substream other
 *  than 0; a dependent frame whose channel map names too many channels). */
int dr_ac3_header(const uint8_t *buf, int size, DrAc3Header *h);

/** The core of an AC-3 track's unit (frames already cut): an independent or
 *  AC-3 frame first keeps that frame alone; a dependent frame first keeps
 *  the frame after it when that one is independent (else nothing).
 *  *off / *out_size: the kept bytes (0 kept is legal); DR_ERR_INVALID when
 *  a header cannot be read. */
int dr_eac3_core(const uint8_t *data, int size, int *off, int *out_size);

/** A DTS core frame's header (ETSI TS 102 114 5.4), as far as the timeline
 *  reads it. */
typedef struct DrDcaHeader {
    int rate;               /**< core sample rate */
    int samples;            /**< PCM samples (blocks x 32) */
    int frame_size;         /**< bytes */
} DrDcaHeader;

/** The header of the core frame at buf: 0, or DR_ERR_INVALID. */
int dr_dca_core_header(const uint8_t *buf, int size, DrDcaHeader *h);

/** What times a DTS-HD extension substream frame without a core. */
enum { DR_DCA_EXSS_LBR = 1, DR_DCA_EXSS_XLL = 2 };

/** A DTS-HD extension substream frame without a core: the sample rate and
 *  the samples of its first asset's LBR (DTS Express) or XLL (lossless)
 *  component, as the processing library's DTS parser reads them.
 *  *lbr_rate_code: the LBR rate code of the last frame that gave one (its
 *  decoder-init header), the caller's to keep across frames (-1 none yet).
 *  DR_DCA_EXSS_LBR or DR_DCA_EXSS_XLL, or DR_ERR_INVALID. */
int dr_dca_exss_timing(const uint8_t *buf, int size, int *lbr_rate_code, int *rate, int *samples);

/** The core of a DTS track's unit: a unit starting with a core frame is cut
 *  to the core frame's size (the DTS-HD extension substream after it goes);
 *  any other unit stays whole. Never fails. */
int dr_dca_core(const uint8_t *data, int size, int *off, int *out_size);

/** An MPEG audio frame's header (ISO/IEC 11172-3 2.4.1.3), as far as the
 *  timeline reads it. */
typedef struct DrMpaHeader {
    int rate;               /**< sample rate */
    int samples;            /**< samples per frame (384 / 1152 / 576) */
    int frame_size;         /**< bytes; 0 for a free-format frame */
} DrMpaHeader;

/** The header of the frame at buf: 0, or DR_ERR_INVALID. */
int dr_mpa_header(const uint8_t *buf, int size, DrMpaHeader *h);

/** The first frame of a DTS stream as the disc reader takes it: 0 not a
 *  valid first frame, 1 a core frame alone, 2 a core frame with a valid
 *  extension substream after it (DTS-HD). */
int dr_dca_first_frame(const uint8_t *buf, int size);

/** dr_pgs_cut's state: whether the chunk given out is no display set (the
 *  engine's caller marks it skipped). */
typedef struct PGSDiscCut {
    int no_unit;
} PGSDiscCut;

#ifdef __cplusplus
}
#endif

#endif /* DISC_PARSE_RULES_H */
