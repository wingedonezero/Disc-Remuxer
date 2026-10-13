/*
 * src/analyze: the one module of ours that links the processing library.
 * It is handed COPIES of data and answers questions; nothing on the byte
 * path from disc sector to output file passes through it. If it were
 * absent, every rip would be byte-identical; only reports and checks would
 * degrade.
 *
 * dr_analyze_use() registers it as the title timeline's host
 * (timeline/disctl.h DRTimelineHost): the stock frame parser for the codecs
 * without a rule of their own (MPEG video, E-AC-3), the LATM stream header
 * and the TrueHD / MLP end-of-stream decode.
 */

#ifndef DISC_ANALYZE_ANALYZE_H
#define DISC_ANALYZE_ANALYZE_H

#ifdef __cplusplus
extern "C" {
#endif

void dr_analyze_use(void);

/* internal: the stream pass's details probe (host.c registers it too) */
void dr_analyze_use_probe(void);

#ifdef __cplusplus
}
#endif

#endif /* DISC_ANALYZE_ANALYZE_H */
