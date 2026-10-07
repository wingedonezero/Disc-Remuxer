# Architecture

Status: **plan agreed, being carried out** (the order of work is below;
done: steps 1-5, step 6's title reader; DVD later). Each step moves a part to its place and is checked against
the output from before the move.

## Words

One name per job, used everywhere (code comments, logs, docs):

| Word | Job | Example |
|---|---|---|
| reader | gets bytes off the source: opens the image or folder, reads its file system, reads sectors with retries | `discio_*` |
| decryption | makes encrypted sectors usable | libdvdcss (DVD), libaacs (HD DVD, Blu-ray) |
| disc layer | opens a source once, tells which kind of disc it is, gives the disc info (every title, its streams and chapters), opens a title | `discdec.c`, `disc.h`, `discinfo.h` |
| title reader | the disc layer's part that reads the opened title: its clips one after another through FFmpeg's line into the title timeline, the clip's time reference, the timeline's frames out as packets, chapters and track results at the end; the format part only says what the title is (clips, how a block is read, tracks, chapter marks, its own values) | `disctitle.c` |
| disc info | the description of a disc for the caller's overview (not the streams a title is ripped from) | `discinfo.h` |
| format part | knows one disc format's structure: titles, clips, tracks, chapters, keys; runs the line below for a title | DVD (`dvdvideo*`), HD DVD (`hddvd*`), Blu-ray (`bluray*`) |
| demuxer | splits a container into packets, one pile per stream | FFmpeg's `mpeg.c` (VOB / EVOB packs), `mpegts.c` (Blu-ray) |
| parser | finds where each frame of one stream starts and ends | FFmpeg's `*_parser.c` (disc mode: `PARSER_FLAG_DISC`) |
| frame step | gives a track's payloads to its parser and the PES times to the frames | `disctl_frames.c` |
| bitstream filter | changes frames that are already found (e.g. keeps only the DTS frame of a DTS-HD frame) | FFmpeg's `bsf/*.c` |
| joiner | joins a title's clips into one timeline and fixes the joins (gaps, overlaps, frames at both sides) | `disctl_join.c`, `disctl_junction.c`, `disctl_pcm.c` |
| title timeline | puts a title's frames on one timeline: clips joined, frame timing, audio gaps and overlaps, chapters, checks of the output | `disctl_*` (what remains of them, see below) |
| decoder | turns frames into pictures and sound: not used to rip; only the end check reads our output back with it | FFmpeg's decoders |
| writer | writes frames into files: stream files, Matroska | `src/disc-remuxer/writer.c` |

## The shape

```
disc-remuxer (src/disc-remuxer)   commands, settings, log, writing files
  ▼
disc layer                         one FFmpeg demuxer, "disc": open the source once,
  │                                which kind of disc, title list (info), open a title
  ├─ format parts                  DVD / HD DVD / Blu-ray: disc structure only
  ├─ readers + decryption          discio_*, libdvdcss, libaacs, later libbluray
  └─ title reader                  the opened title's clips through the line below,
  ▼                                its frames out as packets
FFmpeg's own line                  mpeg.c / mpegts.c -> parsers (FFmpeg's read loop runs them)
                                   our fixes inside these files, behind disc options
  ▼
title timeline                     ours, one module for every format
```

Rules:

1. **FFmpeg's own files are fixed in place.** A behaviour discs need (the
   reference's sector rules, frame rules, timestamp rules) goes into the
   FFmpeg file that does that job, behind an option or flag that is off for
   ordinary files, so FFmpeg behaves as upstream everywhere else. Every change
   is listed in `libs/README.md` with its test. No copies of FFmpeg files
   under a format prefix.
2. **A format part knows only its disc's structure.** Packets, codecs and
   timing are the line's job and live once, for every format.
3. **Format details at the reading end, everything after shared.** E.g.
   LPCM: the header in each packet differs (DVD 3 bytes, HD DVD 5, Blu-ray 4
   with another layout) and is read by the demuxer; the samples on the
   timeline are handled once by the title timeline.
4. **The title timeline is the one module of ours beside FFmpeg's line**,
   because FFmpeg has no place for it (its concat demuxer joins files, but
   none of the timing). It takes the frames FFmpeg's line gives out.

## Today's files and where they go

| Today | Goes to |
|---|---|
| `discio_*.c`, `discio.h` | stay: the readers (`discio_folder.c`: a disc folder on the host as a file system) |
| `disclang.c` | stays: language codes for every format part |
| `discdec.c`, `disc.h` (step 1, done) | the disc layer, the only disc demuxer the program opens; `disc.h` is what a format part gives it |
| `disctitle.c` (step 6, done) | the disc layer's title reader: what `hddvddec.c` did that is not HD DVD (the clips through `mpeg.c`, the time reference, the payloads to the title timeline, its frames out, chapters, results); a format part fills `DiscTitleSource` (clips, block reading, tracks, chapter marks, LPCM header size, lead-in tolerance) |
| `dvdvideodec.c`, `dvdvideo_*.c` | the DVD format part (no longer a demuxer of its own); its playback reading through libdvdnav (`dvdvideo_play.c`) gives way to the navigation scan + title plan, with sectors fed into `mpeg.c` |
| `hddvddec.c`, `hddvd_*.c` | the HD DVD format part `ff_disc_hddvd` (step 1, done: no demuxer of its own any more; step 6, done: only HD DVD's structure, the reading is the title reader's) |
| `bluraydec.c`, `bluray_*.c` (Blu-ray step B1, done) | the Blu-ray format part `ff_disc_bluray` (BDMV, BDAV, AVCHD): the disc's structure and name (B1), its title list (B2, `bluray_titles.c`) AACS (B3, `bluray_aacs.c`: keys at the open, `ff_bluray_aacs_unit()` per aligned unit for the title reader) and the tracks (B4a, `bluray_tracks.c`); the stream probes (B4b) and title checks (B4c) next. Not supported yet: recordable discs' AACS, the segment keys of .fmts clips, BD+. Waiting for a disc that needs it (user): names recorded in the Japanese broadcast character code (ARIB STD-B24, BDAV discs recorded off Japanese TV) are not decoded yet |
| `hddvd_tracks.c` | stays (stream table, languages); step 4, done: its content probe reads through the line (`mpeg.c` disc mode, FFmpeg's AC-3 / DTS parsers in disc mode) instead of its own pack reader and frame finding |
| `discrip_pack.c`, `discrip_ps.c` (step 2, done) | moved into `mpeg.c`: option `disc` (one pack per 2048-byte sector, the sector rules) and `lpcm_header` (3 / 5 bytes, always LPCM) |
| `discrip_units.c` (was the cutter; now `disctl_frames.c`, the frame step) | stays, shrunk to the time pairing (a packet's time goes to the first frame whose picture header lies in it, the MPEG rule FFmpeg's read loop does not have) in front of FFmpeg's parsers (decided in step 3: FFmpeg's read loop pairs times with where a frame begins and keeps only 4 packets' times) |
| `disctl_mpv.c` | stays: picture rules for the video timing (FFmpeg's `mpegvideo_parser.c` already cuts the frames) |
| `disctl_vc1.c` | step 3, done: the frame cutting rules are in `vc1_parser.c` (disc mode); the picture timing stays |
| `disctl_mlp.c` | step 3, done: the access unit cutting is in `mlp_parser.c` (disc mode); the unit check, timing and the seamless join stay |
| `disctl_spu.c` | step 3, done: the unit cutting is in `dvdsub_parser.c` (disc mode); the unit check and the units' times stay |
| `disctl_lpcm.c` | stays: LPCM has no frames in the bitstream; the frame size (samples per output frame) is the title timeline's choice |
| `disctl_codecs.c` (codec table) | stays: which frames belong to a track (the checks after the parser), durations, anchors, output checks; the frame cutting it named is in FFmpeg's parsers (`disc_parser`) |
| `discrip_mix.c` (copied FFmpeg downmix) | step 5, done: replaced by libswresample (the seamless search's mono mix; the same numbers on every TrueHD layout) |
| `disctl_cc.c`, `disctl_ccx.c` | stay with the title timeline (caption track; the caption converter is a helper program) |
| `disctl_title.c`, `_join.c`, `_video.c`, `_audio.c`, `_junction.c`, `_pcm.c`, `_seamless.c`, `_chapters.c`, `_verify.c` | the title timeline (stay) |
| `bluray.c` (FFmpeg's libbluray protocol) | removed (user, 2026-10-07: never used; libbluray is not part of the program); the Blu-ray format part takes the name: `bluraydec.c`, `bluray_*.c` |

## Order of work

Each step ends with every test passing, the moved part's `libs/README.md`
rows updated, and the same output as before the step: the corpus HD DVD
titles ripped before and after and their files compared byte for byte (and
their logs for changed warnings).

0. **Baseline:** today's program kept, and its output for the corpus HD DVD
   titles recorded, as the reference for every later comparison.
1. **Disc layer (done):** `discdec.c` opens the source once, tells the disc
   kind and opens a title through its format part; the program opens only
   "disc"; HD DVD is the first format part. The disc info (`discinfo.h`: the
   disc, every title, its streams and chapters) comes from one open; the
   stream details in it (bit rate, channels, frame size, ...) are filled once
   the start of each title is read for them.
2. **Demuxer (done):** `mpeg.c` got the disc options; the HD DVD part feeds
   it; `discrip_pack.c` and `discrip_ps.c` are gone.
3. **Parsers (done):** the title timeline's own frame cutting is in FFmpeg's parsers,
   behind `PARSER_FLAG_DISC` (`parser.c`'s disc cut under the parser
   contract): sub-pictures (`dvdsub_parser.c`), VC-1 (`vc1_parser.c`), TrueHD
   / MLP (`mlp_parser.c`); MPEG video and the audio codecs were already cut by
   FFmpeg's parsers. Kept with the title timeline (decided): the time pairing,
   the checks of which frames belong to a track, LPCM's frame size.
4. **HD DVD track probe (done)** reads through the line (corpus track lists
   equal). With it: FFmpeg's AC-3 and DTS parsers got the disc reader's own
   frame rules (used when ripping too); `mpeg.c`'s disc mode splits an audio
   payload at its first access unit, as the reader does.
5. **Title timeline tidied (done):** what is left of `disctl_*` is the
   timeline; codec table slimmed (leftovers of the cutter removed); downmix
   through libswresample.
6. **Title reader (done):** what the HD DVD part did that every format
   needs moved into the disc layer (`disctitle.c`); the rules it holds are
   the shared ones (the program-stream reader's time reference, the last
   64 MiB of a clip), the format's own values come from the format part
   (LPCM header size, audio lead-in tolerance). Then the rip core's files
   were renamed to the title timeline's words (`disctl_`, done).
   **DVD on the same line** comes later (user, 2026-10-06), done as HD DVD
   was (decomp check first): the DVD part becomes a format part filling
   `DiscTitleSource` (its segments, CSS, `lpcm_header` 3, its tolerance), its
   titles through the title timeline, libdvdnav playback reading removed;
   checked against the DVD corpus checks.
7. **Blu-ray** (TS demuxer decided there), then the **Matroska writer** once
   every format's streams are right (user, 2026-10-06: Blu-ray adds many
   pieces, e.g. more than one video stream and HDR, so the writer is built
   once and tested on every format's streams). Blu-ray
   items noted on the way: an E-AC-3 stream (Dolby Digital Plus, the improved
   AC-3) that carries an AC-3 inside it gives two linked tracks, like DTS-HD
   MA and its DTS: the E-AC-3 whole, and the AC-3 taken out of it (FFmpeg's
   `eac3_core`; `inner_track` / `from_track`). HD DVD never gives that pair.
   Also: the seamless search's mono mix leaves the height speakers (and the
   wide, surround direct and second LFE channels) out, as the reference does;
   test with them in and out on TrueHD that carries them and see whether the
   chosen join changes.

Points decided on the way: the parsers' disc flag is a new
`PARSER_FLAG_DISC` bit (step 3); the title timeline's files, functions and
log lines are named `disctl_` / `ff_disctl_` / "Title timeline:" (step 6,
user; they were `discrip_` / "Rip core:", the "rip core"; the types keep
their `DR` prefix).

## How a disc is read today (the order above part-way done)

The program opens `disc`, which hands HD DVD images to the HD DVD part. The
HD DVD part describes the disc and the opened title; the disc layer's title
reader reads the title's blocks through the HD DVD part (readers and
decryption), feeds them into FFmpeg's `mpeg.c` as disc sectors (option
`disc`), and runs the packets through the title timeline (`disctl_*`): its frame step gives each
track's payloads to the track's FFmpeg parser (in disc mode where the parser
has the disc reader's rule: VC-1, TrueHD / MLP, sub-pictures, LPCM) and
pairs PES times with the frames; then the timing, the joins and the checks. The DVD-Video
demuxer (FFmpeg's, split into helper files, every read through the readers,
CSS in the demuxer) still reads by playback through libdvdnav into FFmpeg's
`mpeg.c` and is not opened by the program.

## The program (`src/disc-remuxer/`, C)

| File | Part |
|---|---|
| `main.c` | commands (`info`, `demux`, `settings`, `version`), options (`--config`, `--set`, `--json`, `-v`), the run |
| `settings.c` | the settings registry and `disc-remuxer.toml` next to the program (written with every setting and its help; new settings added, unknown ones removed, values kept) |
| `writer.c` | opening a title and writing its stream files, chapters and VobSub pairs |
| `check.c` | the end check: each written video / audio file is read back with FFmpeg's decoders and its stream details (format, profile, size, aspect, frame rate, scan, colour, HDR; audio format, channels, layout, rate, depth, ...) must equal those read from the disc's stream while ripping; a missing or different field fails the title |
| `names.c` | file names from `output.file_name_template` |
| `log.c` | the log: terminal (colours), `--json` records, a log file per disc in the output folder |
| `msg.h` | the numbers of the program's own messages |

The demuxer contract the program relies on: `docs/FORMATS.md`. Finding
discs, job folders and batches are the GUI's job; the program handles one
disc per run.

## Tests (`tests/`, Python)

`./build.sh test` builds the program and a bridge (cffi, compiled against
our headers and linked with the static libraries of the mode), then runs
pytest: `tests/unit/` calls our C code directly on data built byte by byte
(`tests/synth/`, helpers in `tests/helpers/`); tests marked `disc` need a
real disc named by an environment variable and show as skipped without one.
`make coverage` and `tests/tools/coverage.py` show which lines a test run
reaches.

The Rust crates that came before the C program, and the tests still to port
from them, are in `legacy/rust/` (see its `TODO.md`).
