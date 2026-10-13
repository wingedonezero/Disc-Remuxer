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
extern const DrFrameRule dr_mpa_frames;       /**< MPEG audio frames (ctx: DrMpaRuleCtx) */

/** The stream header from an AC-3 / E-AC-3 frame: E-AC-3 bitstream
 *  information up to the channel map (the frame must hold it; a dependent
 *  stream's channel map must give the channels its audio coding mode and
 *  LFE give); AC-3: the syncinfo. 1 valid, 0 not. */
int dr_ac3_stream_header(const uint8_t *buf, int size);

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
