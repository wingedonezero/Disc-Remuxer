/*
 * Linear PCM of DVD-Video and HD DVD (private stream 1 sub-streams 0xA0..):
 * the cut rule.
 *
 * The samples of a disc's LPCM stream come in audio frames: 1/600 s on
 * DVD-Video, 1/1200 s on HD DVD (the frame count and first access unit
 * pointer of each packet's header count these frames). The stream is cut
 * into those frames; the caller gives their format through the rule's state
 * (dr_split_pcm_format): the samples per channel in a frame, the bits per
 * sample (16, 20 or 24) and the number of channels; while the format is not
 * known the bytes are kept. Bytes left at the end of the stream that make
 * no whole frame are no frame.
 */

#include <limits.h>
#include <stdint.h>

#include "parse/rules.h"

static int disc_unit_size(void *logctx, const uint8_t *buf, int avail, int final, void *state)
{
    const DrPcmDvdCut *f = state;
    int64_t frame = (int64_t)f->bits * f->channels * f->samples / 8;

    if (frame <= 0 || frame > INT_MAX)
        return 0;
    return avail >= frame ? (int)frame : 0;
}

static int disc_resync(const uint8_t *buf, int avail)
{
    return avail;
}

const DrCutRule dr_pcm_dvd_cut = { disc_unit_size, disc_resync, sizeof(DrPcmDvdCut) };
