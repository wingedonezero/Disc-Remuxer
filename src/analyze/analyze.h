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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void dr_analyze_use(void);

/* Routes the processing library's own log lines into ours (base/log.h,
 * log_init() first): the source named from the library context, one call
 * to log_library_line() per line. The test bridge installs its own
 * callback instead. */
void dr_analyze_log_install(void);

/* While on, the library's lines below error level are detail lines: what
 * it says while a written file is read back (the end check) is about
 * reading that file, not about the disc. details_of_file() sets it around
 * its read. */
void dr_analyze_log_detail(int on);

/* The processing library's version in buf: "FFmpeg <version> (libavformat
 * <major>.<minor>.<micro>)"; returns buf. */
const char *dr_analyze_version(char *buf, size_t size);

/* internal: the stream pass's details probe (host.c registers it too) */
void dr_analyze_use_probe(void);

#ifdef __cplusplus
}
#endif

#endif /* DISC_ANALYZE_ANALYZE_H */
