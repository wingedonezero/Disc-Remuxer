/*
 * The frame cutters' rules: how each codec's stream from a disc is cut into
 * units, as data (function tables) a generic engine runs. Today the engine
 * is FFmpeg's parser.c disc mode (ff_parser_disc_cut / ff_parser_disc_frames,
 * which keep the av_parser_parse2() contract); the native splitter replaces
 * it when the title reader leaves FFmpeg. Log lines leave through
 * base/vlog.h; rule errors are base/err.h values (the engine translates at
 * its edge).
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

/* The rules (one file each; the cutting is described there): */
extern const DrCutRule   dr_mlp_cut;          /**< TrueHD / MLP access units */
extern const DrCutRule   dr_dvdsub_cut;       /**< DVD / HD DVD sub-picture units */
extern const DrCutRule   dr_vc1_cut;          /**< VC-1 frame units (start-code walk) */
extern const DrCutRule   dr_pgs_cut;          /**< PGS display sets */
extern const DrCutRule   dr_pcm_bluray_cut;   /**< Blu-ray LPCM units */
extern const DrFrameRule dr_adts_frames;      /**< AAC ADTS frames */
extern const DrFrameRule dr_latm_frames;      /**< AAC LATM / LOAS frames */
extern const DrFrameRule dr_textst_frames;    /**< TextST segments (one per unit) */

/** dr_pgs_cut's state: whether the chunk given out is no display set (the
 *  engine's caller marks it skipped). */
typedef struct PGSDiscCut {
    int no_unit;
} PGSDiscCut;

#ifdef __cplusplus
}
#endif

#endif /* DISC_PARSE_RULES_H */
