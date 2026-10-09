# What every format demuxer gives the program

`disc-remuxer` has no format-specific code. It opens every source with one
demuxer of our FFmpeg, `disc` (the disc layer, `libavformat/discdec.c`): it
tells the kind of disc and hands it to that kind's format part (HD DVD and
Blu-ray with UHD and 3D now; DVD-Video to come), which describes the disc and the
opened title; the disc layer's title reader (`libavformat/disctitle.c`) runs
that title through FFmpeg's line and the title timeline
(`libavformat/disctl_*.c`). Every format part gives the program the same
things, listed here.

## Options the program sets

| Option | Meaning |
|---|---|
| `title` | the title to open (index in the disc's title list, from 0); -1: only the disc info. The program opens with -1 and then opens each title on the open disc with `disc_title_open()` (below) |
| `second_view` | 3-D titles: both views are read (the stream files interleaved at their extent start points) and the second view is a stream of its own (H.264, disposition dependent): each picture with the base-view frame of the same time; streams carry `eye` (`left` / `right`, from the playlist's MVC_Base_view_R_flag). The program sets it for `--second-view` / setting `bluray.second_view` |
| `dolby_vision` | Dolby Vision titles (UHD Blu-ray): `merged` (default) the enhancement layer and its metadata go into the video's frames (each frame: the base picture, then the enhancement picture's units behind the NAL unit header 7E 01, the RPU as it is); `separate` the enhancement layer is a stream of its own (metadata `dolby_vision` = `EL`; the video's is `BL`, merged: `BL+EL+RPU`). The program sets it from setting `bluray.dolby_vision` |
| `part` | with `title`: only that part of it (from 0, as the title's parts list them), opened as a title of its own; -1 (default): the whole title. The program gives it to `disc_title_open()` for `--split` / setting `output.split_joined_titles` |
| `read_attempts`, `udf_reader` | from the settings `read.attempts`, `read.udf_reader` |
| `keydb` | key files (setting `aacs.key_files`), where the format has keys |
| `cc_helper` | formats with caption tracks: the caption converter (CCExtractor); unset: the `ccextractor` next to the running program |

## The disc info (`libavformat/discinfo.h`, `disc_info(ctx)`)

Filled on open, whichever title is opened; with `title` -1 the disc is only
described (no title opened, nothing read): the program's `info` and the
overview at the start of `demux` come from that one open. The disc layer
fills the disc, the format part its titles.

| Level | Fields |
|---|---|
| disc (`DiscInfo`) | kind (`HD DVD`, ...), volume label, file system and UDF revision, encrypted, the titles |
| title (`DiscTitleInfo`) | number in the title list, name, source (what it is made from, e.g. `00800.mpls(1)`; Blu-ray), angle (Blu-ray, 0 none), language, duration, size in bytes, selected (0: shorter than the minimum length, listed only), the disc's files it plays in order, chapters (start, name), streams, parts |
| part (`DiscPartInfo`) | a title that plays whole stream files back to back (Blu-ray: items of connection condition 1 that each cover their whole STC sequence, e.g. the episodes of a series in one playlist) has one part per run of them: start in the title, duration, its files (a range of the title's), its own chapter count (the marks of its items, from its own start, without those in its last second, as a title of its own has them). A title of one piece has none |
| stream (`DiscStreamInfo`) | kind, codec, language, the stream taken out of it / it is taken out of (a core, the forced-only copy of a subtitle, the second view of a 3-D video), forced (subtitles: only the forced ones), second view (video: of a 3-D video), second layer (video: an enhancement layer on its own PID read with it, e.g. Dolby Vision), offset sequence (subtitles of a 3-D title: the disc's depth data for it, -1 none), frame / canvas size; the stream details (aspect, frame rate, channels, sample rate, bits, bit rate) are 0 until the start of each title is read for them (to come) |

## Opening titles on the open disc

The disc is read once: `disc_title_open(ctx, title, part, second_view)`
opens a title on a disc opened with `title` -1 (the title list and each
title's stream pass are not done again), `av_read_frame()` gives its
packets, `disc_title_close(ctx)` removes its streams, chapters and results
(opening the next title closes the one before too). `part` and
`second_view` are as the options above.

## The opened title

Streams carry `language` (ISO 639-2) in their metadata. Subtitle streams
also carry `picture`: what the units are, in words for the log (e.g. `HD DVD
sub-pictures: 256 colours, each with its own palette`); their
`codecpar->width` / `height` is the canvas the units are drawn on. A
closed-caption track (line-21 captions taken out of the MPEG-2 video, made
into text: `AV_CODEC_ID_SUBRIP` packets, one per cue; DVD) carries
`caption` instead, in words for the log. The video stream carries `line21`
when the disc marks line-21 captions (`field 1`, `field 2`, `fields 1 and
2`). The title's duration is
`AVFormatContext.duration`.

## While reading

The read-only, exported option `progress` (0..10000) says how much of the
title has been read.

## At the end (after the last packet)

| Where | Key | Value |
|---|---|---|
| each stream | `frames` | frames given out |
| each stream | `warnings` | warnings logged about the track |
| each stream | `delay_us` | audio: its start delay (µs) |
| subtitle streams | `no_stop` | units without a stop time, each shown until the next one starts (the title's last one: until the video's end) |
| the video stream | `cc_blocks`, `cc_text` | a format without caption tracks (HD DVD): caption blocks taken out of the video and not kept, and their entries that carry caption bytes (0: the caption slot was empty; more: a warning) |
| caption streams | `cc_blocks` | caption blocks given to the converter |
| caption streams | `cues` | cues it made (0: the caption data carries no caption text) |
| subtitle streams | `outside` | units drawn outside the canvas (each segment that has some also counts one warning) |
| the context | `untested` | features met that no real disc has tested (the job is reported failed) |

Chapters are `AVFormatContext.chapters`, set at the end (the title timeline marks
them on the video's key frames).

## Log

Everything the disc layer, the format part and the title timeline have to say goes through
`av_log`: errors and warnings a reader must see at `AV_LOG_ERROR` /
`AV_LOG_WARNING` in plain language, normal steps at `AV_LOG_INFO`, details
at `AV_LOG_VERBOSE` / `AV_LOG_DEBUG`. Messages that come again on every title
open (the disc's file system, its encryption) are verbose: the program
shows the disc once from the metadata above.
