# Architecture

`disc-remuxer` reads DVD-Video, HD DVD, Blu-ray (with UHD and 3-D) sources —
disc images and disc folders — and writes each title's streams as
elementary-stream files with their chapters (Matroska output comes later).
One engine, two front ends: the GUI (Qt 6) and the command line.

How the code got this shape (it was first built inside FFmpeg, then moved
out module by module): `docs/history/REWRITE.md`. The libraries and every
change made to them: `libs/README.md`. The title timeline in detail:
`docs/TITLE_TIMELINE.md`.

## The one rule

> **Demuxing — every byte transformation from disc sector to output file —
> is our own code, end to end. No byte that reaches an output file passes
> through FFmpeg.**
>
> **FFmpeg is an analysis instrument.** It is handed copies of data and
> answers questions ("what does this stream decode to?"). If it were
> missing, every rip would be byte-identical; only reports and checks would
> degrade.

FFmpeg is linked from one module only (`src/analyze/`), unmodified (see
`libs/README.md`). Every codec is cut by `src/parse/` (MPEG-1/2 video and
E-AC-3, which never had a disc rule, by `parse/stock.c`: FFmpeg's own cut,
ported). The timeline still asks analyze one question (`DRTimelineHost` in
`src/timeline/disctl.h`), on copies, no stream byte passing: at a TrueHD
join, the samples of a stream's last access unit (decoded: they depend on
its coded blocks, which only a decoder walks).

## The modules

```
src/gui/         the GUI (C++, Qt 6 Widgets)          ─┐ front ends: the engine's
src/disc-remuxer/main.c   the command line              ─┘ typed calls and events
   │
src/disc-remuxer/ the engine (libdisc-remuxer.a): settings in use, the open
   │              disc, the selection, runs, the stream writer, the views
   │              (what the front ends show), finding discs, typed events
src/disc/         the disc layer: open once, disc kind, title list, the
   │              stream pass (tracks and their details), the title reader
   ├── dvd/ hddvd/ bluray/   format parts: structure, titles, tracks,
   │              chapters, keys (libdvdcss, libaacs), "the next block of
   │              this segment"
src/timeline/     the title timeline: frames timed, video grid, audio
   │              timing, joins, junction, subtitles, captions, chapters,
   │              the per-frame verify
src/mux/          container splitters: MPEG-PS packs, MPEG-TS PES
src/parse/        frame cutters: one rule set per codec, one splitter engine;
                  the stock cutters (MPEG video, E-AC-3: FFmpeg's cut, ported)
src/io/           readers: block reads with retries, ISO 9660, two UDF
                  readers (NetBSD- and Linux-based), disc folders, the
                  file-system choice, the disc format
src/base/         what everything may use: the log, messages, settings,
                  errors, memory, buffers, byte readers, CRCs, SHA

beside the path:
src/mkv/          the Matroska writer (C++ on libebml + libmatroska + zlib, a
                  C interface): a title into one MKV file with mkvmerge's rules
src/analyze/      the one module that links FFmpeg: stream details, the end
                  check of written files, the TrueHD end-of-stream decode
                  (the timeline's one question); FFmpeg's log lines into ours
helpers           ccextractor (a separate process: captions to text)
```

Dependencies point down only. `base` depends on libc alone; `io`, `mux` and
`parse` on `base` only; `timeline` on `base` and `parse`. The build enforces it: those modules,
the engine and the GUI compile without FFmpeg's include path, so an FFmpeg
include there is a compile error. Each module is a static library
(`mk/<module>.mk`).

## The rip path

```
source (image / folder)
  io/       opens it, picks the file system, reads 2048-byte blocks with
            the retry rules, says every attempt and damaged block
  format    places the title's segments, decrypts (CSS per sector key,
  part      AACS per aligned unit), answers "the next block of this segment"
  mux/      PS: one sector = one pack | TS: 192-byte source packets, PES per
            PID; payloads out with their PES times, stretches left out said,
            continuity counters checked (missing packets said, duplicates
            left out)
  parse/    the codec's rule cuts payloads into frames; picture headers
            read; core-only filters (E-AC-3 -> AC-3, DTS-HD -> DTS)
  timeline/ times paired with frames; the video grid; audio durations; the
            joiner puts segments on one timeline; the junction handles gaps
            and overlaps at joins; chapters; the verify re-reads every frame
            that leaves (CRCs, order, durations, the audio format it states)
  writer    output.format = streams: elementary-stream files, WAVE, VobSub (.sub + .idx), SUP, SRT,
            chapter XML (src/disc-remuxer/writer.c); then the end check
            reads each written file back (analyze/)
            output.format = mkv: one Matroska file (mkvout.c -> src/mkv),
            written as mkvmerge writes one from the same streams with the
            disc's own times: equal byte for byte on real discs (H.264,
            LPCM, AC-3 / E-AC-3, DTS, PGS so far; other tracks are left out
            and said); settings mkv.compression (PGS zlib, default none)
            and mkv.remove_dialog_normalization (default off)
```

The info path: the format part reads the structure (every check reported
with its file, table and byte offset) → the title list → the stream pass
(mux + parse over the title's largest segment; duplicates left out; details
through analyze) → the disc model (`src/disc/info.h`, `disc.h`) → the
engine → the front ends.

## The disc interface (`src/disc/disc.h`, `info.h`)

`dr_disc_open()` opens a source once and gives the disc model: the disc
(kind, name, file system, protection, properties with keys), its title list
(name, length, size, the files and parts it plays, chapters, angles, marks
such as a likely fake playlist) and each title's streams (kind, codec,
language, details: picture size, frame rate, HDR and Dolby Vision, channels,
layout, rate, bits; which stream is taken out of which: a DTS core, forced
subtitles, a 3-D second view). Options are typed fields (`DrDiscOptions`:
title, part, second view, Dolby Vision merged or separate, minimum length,
the DVD cell mode, read attempts, the UDF reader, key files, the caption
helper). `dr_disc_title_open()` opens one title (or one part of it) on the
open disc; `dr_disc_read()` gives its frames in order, track by track, with
their times; the title's chapters, damaged ranges and per-track results
come at its end.

## Logging

One log (`src/base/log.h`): levels error / warning / ok / info / detail,
numbered program messages (`src/base/msg.h`: 1000s program and settings,
2000s sources, 3000s titles, 5000s streams and checks, 6000s output), step
headings, the run's log file and debug file, terminal colours or JSON
records, and the front end's sink. The modules log through one hook
(`src/base/vlog.h`, `dr_vlog(logctx, level, ...)`, the source named by
`logctx`); the DVD libraries and libaacs through their callbacks into the
same place; FFmpeg's lines through `analyze/log.c`. Nothing prints on its
own. A library line already shown in the run is a detail line while a disc
or a title is opened; while a title is read every line shows.

The standard log says every decision, finding, drop and damage; the debug
log adds the mechanics.

## The engine and its two front ends

Every file of `src/disc-remuxer/` but `main.c` is the engine
(`libdisc-remuxer.a`, `engine.h`). The GUI and the command line both use its
typed API; neither has rules or wording of its own (parity: what the GUI
can choose or show, the command line can choose or print, so a
command-line test covers the GUI).

| File | Part |
|---|---|
| `engine.c`, `engine.h` | settings and the log set up; a disc opened once (`engine_disc_open()`); its overview (`info`); a run (its folder: given, or `<output.folder>/<disc name>`; its log files; its titles; the summary); a stop (`engine_cancel()`, Ctrl+C) |
| `events.h`, `events.c`, `emit.h` | typed events on one sink (`engine_set_events()`): log lines, progress, the disc and its titles, title start, track result, title result, run result; without a sink, JSON records on the terminal (`--json`) |
| `views.h`, `views.c` | what the front ends show, in the engine's words: details rows of the disc, a title, a track and the chapters; the queue's one-line texts; the queue's states while scanning and ripping (`info --details`, `demux --details` print them) |
| `select.c`, `select.h` | the selection: titles ripped, written as parts, tracks kept (`--tracks`) |
| `discs.c`, `discs.h` | finding discs in a folder and each disc's output folder (`find`, the GUI's queue) |
| `writer.c`, `writer.h` | one title's stream files, chapters, VobSub pairs, WAVE files, and the end check |
| `mkvout.c`, `mkvout.h` | one title into one MKV file (output.format = mkv): the tracks the writer takes, held until each has started, then the Matroska writer (`src/mkv/`) |
| `codecs.c`, `codecs.h` | codec, profile, layout and Matroska names as players show them |
| `names.c`, `names.h` | file names from `output.file_name_template` |
| `main.c` | the command line: `info`, `demux`, `find`, `settings`, `version`; options `--config`, `--set`, `--json`, `--details`, `--split`, `--second-view`, `--tracks`, `-v` |

Settings: one registry (`src/base/settings.c`) and `disc-remuxer.toml` next
to the program, shared by both front ends; every setting says when it is
read (as a disc is opened, as a rip runs, by the GUI).

### The GUI (`src/gui/`, C++, Qt 6 Widgets)

`disc-remuxer-gui` links the engine like the command line. `MainWindow`: the
queue (columns Type, Description, Size, Status, Progress; each disc's titles
with check boxes, each title's chapters and tracks under it), the details
of the selected row (`DetailsPanel`), the log (`Console`), the run (discs
one after another, progress, Stop), a menu per row (parts, output folder,
log, scan again, remove). `QueueTree` (drops, reordering), `DiscFinder`
(the engine's finder), `EngineWorker` (scans and rips on a thread of its
own; `LogRelay` turns the engine's events into Qt signals), `PrefsDialog`
(every setting from the registry, one tab per group). Built by `mk/gui.mk`
with CMake against the host's Qt.

## Tests (`tests/`, Python)

`./build.sh test` builds the program and a bridge (cffi, compiled against
our headers and linked with the static libraries), then runs pytest:
`tests/unit/` calls our C code directly and runs the program on discs built
byte by byte (`tests/synth/`, helpers in `tests/helpers/`); tests marked
`disc` need a real disc named by an environment variable and show as skipped
without one. `make coverage` and `tests/tools/coverage.py` show which lines
a test run reaches.

## Open items

- The MKV writer: H.264, LPCM, AC-3 / E-AC-3, DTS and PGS so far (on libebml + libmatroska,
  mkvmerge's rules); the other codecs (TrueHD, HEVC + Dolby Vision, the DVD and HD DVD
  ones) one by one, each checked against mkvmerge on a real disc; a forced-only PGS copy
  that turns out empty is still an empty track (to decide); the end check does not read the
  MKV back yet.
- Holes in a title (a damaged stretch left out): the stream files close the
  hole, the subtitles and chapters after it keep the disc's times — to
  decide: shift them by what the video lost.
- A caption cue's end can round 1 ms differently from the reference rips'.
