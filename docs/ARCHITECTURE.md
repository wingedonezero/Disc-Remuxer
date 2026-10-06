# Architecture

Status: **plan agreed, being carried out** (the order of work is below).
Today's code is still laid out the old way; each step of the order moves a
part to its place and is checked against the output from before the move.

## Words

One name per job, used everywhere (code comments, logs, docs):

| Word | Job | Example |
|---|---|---|
| reader | gets bytes off the source: opens the image or folder, reads its file system, reads sectors with retries | `discio_*` |
| decryption | makes encrypted sectors usable | libdvdcss (DVD), libaacs (HD DVD, Blu-ray) |
| disc layer | opens a source once, tells which kind of disc it is, gives the title list, opens a title | `discdec.c` (to add) |
| format part | knows one disc format's structure: titles, clips, tracks, chapters, keys; runs the line below for a title | DVD (`dvdvideo*`), HD DVD (`hddvd*`), Blu-ray (to add) |
| demuxer | splits a container into packets, one pile per stream | FFmpeg's `mpeg.c` (VOB / EVOB packs), `mpegts.c` (Blu-ray) |
| parser | cuts a stream's packets into whole frames, each with its time | FFmpeg's `*_parser.c` |
| title timeline | puts a title's frames on one timeline: clips joined, frame timing, audio gaps and overlaps, chapters, checks of the output | `discrip_*` (what remains of them, see below) |
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
  ▼
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
| `discio_*.c`, `discio.h` | stay: the readers |
| `disclang.c` | stays: language codes for every format part |
| (new) `discdec.c` | the disc layer, the only disc demuxer the program opens |
| `dvdvideodec.c`, `dvdvideo_*.c` | the DVD format part (no longer a demuxer of its own); its playback reading through libdvdnav (`dvdvideo_play.c`) gives way to the navigation scan + title plan, with sectors fed into `mpeg.c` |
| `hddvddec.c`, `hddvd_*.c` | the HD DVD format part (no longer a demuxer of its own) |
| `hddvd_tracks.c` | stays (stream table, languages); its content probe reads through the line instead of its own pack reader and frame cutter |
| `discrip_pack.c`, `discrip_ps.c` | into `mpeg.c`: option for disc sectors (one pack per 2048-byte sector, the sector rules) and the LPCM header length (3 / 5 bytes, always LPCM) |
| `discrip_units.c` (the cutter stage) | goes: FFmpeg's read loop (`demux.c`) runs the parsers; where the frame / timestamp pairing must differ for discs, that goes into `demux.c` / `parser.c` behind the disc flag |
| `discrip_mpv.c` | the picture rules into `mpegvideo_parser.c` where they decide frames; what only the video timing needs stays with the title timeline |
| `discrip_vc1.c` | the frame cutting rules (end-of-sequence, user data with its frame) into `vc1_parser.c`; picture timing stays with the title timeline |
| `discrip_mlp.c` | the access unit rules (what follows a broken unit) into `mlp_parser.c` |
| `discrip_spu.c` | the unit rules and checks into `dvdsub_parser.c` (it already reads HD DVD's 32-bit unit size); the units' times stay with the title timeline |
| `discrip_lpcm.c` | the header check into `mpeg.c`; frames of the samples: the title timeline |
| `discrip_codecs.c` (codec table) | the frame checks move to their parsers; the table keeps what the title timeline needs per codec (durations, anchors, output checks) |
| `discrip_mix.c` (copied FFmpeg downmix) | replaced by libswresample |
| `discrip_cc.c`, `discrip_ccx.c` | stay with the title timeline (caption track; the caption converter is a helper program) |
| `discrip_title.c`, `_join.c`, `_video.c`, `_audio.c`, `_junction.c`, `_pcm.c`, `_seamless.c`, `_chapters.c`, `_verify.c` | the title timeline (stay) |
| `bluray.c` | FFmpeg's libbluray protocol, untouched until Blu-ray |

## Order of work

Each step ends with every test passing, the moved part's `libs/README.md`
rows updated, and the same output as before the step: the corpus HD DVD
titles ripped before and after and their files compared byte for byte (and
their logs for changed warnings).

0. **Baseline:** today's program kept, and its output for the corpus HD DVD
   titles recorded, as the reference for every later comparison.
1. **Disc layer:** `discdec.c` opens the source once, tells the disc kind,
   lists titles and opens one through its format part; the program opens
   only "disc"; HD DVD plugged in as the first format part.
2. **Demuxer:** `mpeg.c` gets the disc options; the HD DVD part feeds it
   directly; `discrip_pack.c` and `discrip_ps.c` go.
3. **Parsers, one codec at a time:** AC-3, E-AC-3, DTS, MPEG audio (FFmpeg's
   parsers already cut these; only where they run changes), then MPEG-2
   video, VC-1, TrueHD / MLP, sub-pictures, LPCM; `discrip_units.c` goes at
   the end.
4. **HD DVD track probe** reads through the line (corpus track lists equal).
5. **Title timeline tidied:** what is left of `discrip_*` is the timeline;
   codec table slimmed; downmix through libswresample.
6. **DVD on the same line:** the DVD part's sectors into `mpeg.c` with the
   disc options, its titles through the title timeline, libdvdnav playback
   reading removed; checked against the DVD corpus checks.
7. **Matroska writer**, then **Blu-ray** (TS demuxer decided there).

Open points (decided on the way): the parsers' disc flag (a new
`PARSER_FLAG_*` bit set from the demuxer, or a format context flag that
`demux.c` hands to the parsers it creates); whether the title timeline's
files keep the `discrip_` prefix or are renamed once only the timeline is
left.

## How a disc is read today (before the order above)

The program tries the demuxers in turn (today only `hddvd`). The HD DVD
demuxer reads a title's blocks through the readers and decryption, feeds
them into `discrip_ps` (its own MPEG-PS demuxer on `discrip_pack`), and runs
the packets through the rip core (`discrip_*`: its cutter stage runs FFmpeg's
parsers for MPEG video, AC-3 / E-AC-3, DTS, MPEG audio and AAC, its own
cutters for VC-1, TrueHD / MLP, LPCM and sub-pictures). The DVD-Video
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
