/*
 * The details probe and the end check of a written stream file. The stream details
 * (MediaInfo-like: format, profile, size, aspect, frame rate, scan, colour,
 * HDR; audio format, channels, layout, rate, depth, ...) are read with
 * FFmpeg's decoders once from the disc's stream while the title is ripped
 * and once from the written file afterwards. Every field the disc's stream
 * gives must be in the file with the same value.
 */

#ifndef DISC_ANALYZE_DETAILS_H
#define DISC_ANALYZE_DETAILS_H

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#define DETAILS_MAX 40

/* Named fields, each a value in words; a field the stream does not give is
 * not in the list. */
typedef struct Details {
    int  valid;                           /* a frame was decoded */
    int  n;
    char name[DETAILS_MAX][40];
    char value[DETAILS_MAX][160];
} Details;

typedef struct DetailsProbe DetailsProbe;

struct DrDiscTrack;

/* A decoder over one stream's packets until its first frame. Returns 0, or
 * an AVERROR when the codec has no decoder. */
int  details_probe_open(DetailsProbe **p, const AVCodecParameters *par);
/* The same over a track of an opened disc title (disc/disc.h): the track's
 * codec and, as far as the track knows them, its audio format. */
int  details_probe_open_track(DetailsProbe **p, const struct DrDiscTrack *t);
/* One packet of the stream; 1 once the details are known (or given up). */
int  details_probe_packet(DetailsProbe *p, const AVPacket *pkt);
/* One frame's bytes (the disc side); 1 as details_probe_packet(). */
int  details_probe_data(DetailsProbe *p, const uint8_t *data, int size);
/* The details read (valid = 0 when no frame was decoded). */
void details_probe_get(DetailsProbe *p, Details *d);
void details_probe_close(DetailsProbe **p);

/* The details of the first stream of `type` in the file at path. */
int  details_of_file(const char *path, enum AVMediaType type, Details *d);

/* Fields of disc that the file does not have, or has with another value:
 * "name: disc -> file" items, "; " between them, into diff. Returns their
 * number. */
int  details_compare(const Details *disc, const Details *file, char *diff, size_t size);

/* ---- the channel-layout probe (the writer's file names) ---- */

typedef struct DrLayoutProbe DrLayoutProbe;

/* A decoder over an audio stream's packets until its first frame gives the
 * channel layout: 0 opened, < 0 no decoder (or no memory). */
int  dr_layout_probe_open(DrLayoutProbe **p, const AVCodecParameters *par);
/* The same over a track of an opened disc title (disc/disc.h). */
int  dr_layout_probe_open_track(DrLayoutProbe **p, const struct DrDiscTrack *t);
/* One packet in: 1 when a frame was decoded (the layout is known). */
int  dr_layout_probe_packet(DrLayoutProbe *p, const AVPacket *pkt);
/* One frame's bytes in (the disc side): 1 as dr_layout_probe_packet(). */
int  dr_layout_probe_data(DrLayoutProbe *p, const uint8_t *data, int size);
/* The decoded frame's channel layout, NULL before one. */
const AVChannelLayout *dr_layout_probe_layout(const DrLayoutProbe *p);
/* The decoded frame's channel layout in words into buf: 1 when a frame
 * gave one, 0 when none (yet). */
int  dr_layout_probe_name(const DrLayoutProbe *p, char *buf, size_t size);
/* The profile the decoder found, or fallback. */
int  dr_layout_probe_profile(const DrLayoutProbe *p, int fallback);
void dr_layout_probe_close(DrLayoutProbe **p);

/* A channel layout in words into buf (may be NULL): layout mask when
 * given, else the default layout of `channels`. Returns the mask written
 * to buf (the default layout's when layout is 0), 0 when none is known. */
uint64_t dr_layout_of(uint64_t layout, int channels, char *buf, size_t size);

#endif
