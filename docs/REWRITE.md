# The rewrite: architecture and progress

Updated 2026-10-15, on branch `claude/disc-layer-ffmpeg-handoff-7ba7a7` (the
rewrite branch since step 7c; `claude/rewrite-untangle-1c24ca` stopped at 7b;
main still holds the pre-rewrite program and is the byte-compare reference). Inventory of what the old shape was made of: `docs/UNTANGLE.md`.

Status: **steps 1–8 DONE**: 7a (the clip demuxer), 7b (the format parts,
rows 258–263), 7d (the disc model, row 264; 7c folded into it) and 7e (the
log flip, row 265) are in — **libs/ffmpeg is byte-for-byte the import
again**, the "disc" demuxer is gone, the program and GUI run on the typed
disc API (disc/disc.h), the modules log straight into base/log.h with
their own source names, and the processing library's lines come in through
analyze's hook. Step 8, the DVD lift, is in (rows 266–272): the DVD
libraries keep only table parsing and the navigation VM; everything else
of the DVD part is `src/disc/dvd/`. Next: step 9 (engine API v2, GUI-first),
then step 10 (the MKV writer). The corpus byte-compare + info compare
against a main build is the user's, run once the whole rewrite is done.
Tests at every commit: 743 passed, 15 skipped (the disc-marked ones without a
disc). Every move so far is behavior-preserving by construction and by test;
the real-disc byte-compare against a main build is still owed (user runs it
when the demux side is complete).

---

## Part 1 — The goal and the one rule

The program (a disc demuxer: DVD, HD DVD, Blu-ray, UHD in; correct elementary
streams, later MKV, out) was built INSIDE FFmpeg: ~38k lines of our own code
lived in `libavformat/`, our frame-cutting rules inside FFmpeg's parsers, our
DVD logic inside libdvdread. It worked (verified byte-for-byte on real discs)
but had no boundary between "our engine" and "FFmpeg", two logging systems,
string-typed data between layers, and no way to say what of FFmpeg we used.

The rewrite re-hosts the same verified code into a tree of our own, module by
module, each move gated by the test suite and byte-compares. Nothing is
redesigned unless its design was the problem (the glue, the transport, the
API shapes). One rule decides every placement question:

> **Demuxing = every byte transformation from disc sector to output file.
> That path is ours, end to end: no byte that reaches an output file ever
> passes through FFmpeg.**
>
> **FFmpeg = an analysis instrument. It is handed COPIES of data and answers
> questions ("what does this decode to?"). If it vanished, every rip would be
> byte-identical; only reports and checks would degrade.**

Consequences: FFmpeg can never corrupt a rip, time a frame or decide a join;
it is called from exactly one module (`analyze/`, step 6) with one log hook;
everything else of it may one day be dropped or upgraded freely.

## Part 2 — The target architecture

### 2.1 The module map and the dependency rule

```
gui/  cli/                    Qt 6 Widgets | the command line (a formatter)
   │
engine/                       settings use, selection, runs, writer, checks,
   │                          the disc model and typed events for the GUI
disc/                         the disc layer: open once, disc kind, title
   │                          list, stream pass, title reader
   ├── dvd/  hddvd/  bluray/  format parts: structure, titles, tracks, keys,
   │                          chapters, "which block next"; decryption under
   │                          them (libdvdcss, libaacs)
   ├── timeline/              the title timeline: frame step, video grid,
   │                          audio timing, joiner, junction, PCM strategy,
   │                          SPU/PGS/TextST, captions, chapters, verify
   ├── mux/                   container splitters: PS packs, TS PES
   ├── parse/                 frame cutters: one rule set per codec, one
   │                          splitter engine
   └── io/                    readers: block reads with retries, ISO 9660,
                              UDF x2, disc folder, fs choice, disc format
base/                         the one part everything may use: log, events,
                              settings, messages, errors, memory, buffers,
                              byte readers, CRC, the library-line hook

beside the path:
analyze/                      the ONE module that links FFmpeg (untrimmed):
                              details probe, end check, seamless helper,
                              small answers; one av_log hook into our log
helpers:                      ccextractor (a process), ffmpeg/ffprobe
                              binaries in dist/ for testing
```

Dependencies point down only. `base` depends on libc alone. `io`, `mux`,
`parse`, `timeline` depend on `base` only — **enforced by the build**: their
compile flags carry no FFmpeg include path, so an FFmpeg dependency is a
compile error. Only `analyze/` includes FFmpeg headers. The GUI and CLI see
the engine's headers and base's, nothing deeper.

### 2.2 The rip path (all ours when done)

```
source (image / folder / later a drive)
  io/      opens it, picks the file system, reads 2048-byte blocks with the
           retry rules, says every attempt and damage
  format   places the title's segments, decrypts (CSS per sector key /
  part     AACS per aligned unit), answers "the next block of this segment"
  mux/     PS: one sector = one pack, its packets by the disc reader's
           rules | TS: 192-byte source packets, per-PID PES reassembly;
           payloads out with their PES times, stretches left out reported
  parse/   the codec's rule cuts payloads into units/frames; picture
           headers read; core-only filters (E-AC-3->AC-3, DTS-HD->DTS)
  timeline/ the frame step pairs times with frames; video grid; audio
           durations; the joiner puts segments on one timeline; the
           junction fixes gaps/overlaps; chapters; stage-5 verify re-parses
           every outgoing frame (CRCs, order, durations)
  engine/writer  elementary-stream files, WAVE, VobSub, SUP, SRT, chapter
           XML (all hand-written stdio today already); later the MKV writer
```

The info path: format part reads the structure (every check reported with
file/table/byte offset through the findings book) → title list (policy) →
stream pass (mux+parse over the largest segment; duplicates out; details via
analyze) → the disc model (`DrDisc`/`DrTitle`/`DrTrack`) → GUI/CLI.

### 2.3 Logging: one system

- `base/log.h` is the log: levels error/warning/ok/info/detail, numbered
  program messages (`base/msg.h`, 1000s program … 6000s output), step
  headings, run log file + debug file, terminal colours or JSON records, the
  caller's sink (today `EngineSink`, v2 `DrSink`), dedup of repeated library
  lines, error/warning counts.
- `base/vlog.h` is the ONE hook the extracted modules log through
  (`dr_vlog(logctx, level, fmt, ...)`); logctx is the module's source-name
  string (`dr_disc_open()`'s log name, "disc" by default). `log_init()`
  installs base's own sink for it (`base/log.c`): the line is formatted
  and handed to `log_library_line()` with that source — no AVClass
  anywhere on the disc path, no program mediation. The
  `dr_vlog_wants(level)` check lets modules skip building expensive debug
  dumps that would be dropped. The processing library's own lines
  (analyze's decoders) come in through `analyze/log.c`
  (`dr_analyze_log_install()`), with the end check's detail demotion
  toggled by `details_of_file()` itself.
- The DVD libraries already log through their callback structs into the same
  funnel; libaacs through its handler. Nothing prints on its own.

### 2.4 Errors, events, settings

- Errors: `base/err.h` — 0 ok, `-errno` for system errors (numerically
  identical to FFmpeg's `AVERROR(e)`, so values cross the migration seam
  unchanged), `DR_ERR_*` for our conditions (EOF, invalid data, no memory,
  limit, cancelled, external). Where our values reach FFmpeg-side consumers
  during migration, the wrapper translates at the edge (`DR_ERR_INVALID` ->
  `AVERROR_INVALIDDATA`), so tests and messages see no change.
- Events: `src/disc-remuxer/events.h` (was `base/events.h`; step 9a) — the engine-v2 contract. One `DrEvent` union
  (line, progress, title start, track result, title result with a damage
  range ARRAY, run result) on one `DrSink`. The GUI consumes structs; JSON
  becomes a rendering by the CLI, never the transport. Adopted at step 9;
  the fields mirror today's JSON records one to one.
- Settings: the registry + `disc-remuxer.toml` (tomlc17) moved into base
  unchanged; one settings system for CLI and GUI stays the law. Options
  reach modules as typed fields (step 7+), not string dictionaries.

### 2.5 analyze/ — FFmpeg as an instrument (step 6)

Four entry points, all fed copies, none on the data path:

| Call | In: | Out: | Replaces |
|---|---|---|---|
| details probe | a sample of a track's elementary stream | width/height/aspect, frame rate, profile/level, HDR10/HDR10+/HLG mastering + CLL, Dolby Vision MEL/FEL, channels/layout/rate/bits | the decoders in `discpass.c` and the writer's layout probe |
| end check | the disc-side sample + the written file | ~25 stream details, compared by the same decoder build | `check.c`'s linked decode |
| seamless helper | candidate TrueHD AU runs | PCM, mono mix, the comparison numbers | `disctl_mlp` decode + swresample |
| small answers | one LATM frame; MLP end AUs | rate/samples; the end AU's sample count | the two narrow decode sites |

FFmpeg stays full-build and vendored as-is; once the in-place edits are gone
(end of step 4/5), it is ~upstream-clean and updatable. `analyze/` failing
or absent degrades to "not checked", said plainly — never fatal, never a
different rip.

### 2.6 The libraries that remain libraries

- **libdvdnav**: vendored with the VM patch set (~1,100 lines: asserts →
  recorded failures, deterministic random/shuffle, playback clock, GPRM
  counter semantics, nav-only skipping, `open_files`, working `dup`). The
  scan driver moved UP into `dvd/dvd_scan.c` at step 8a.
- **libdvdread**: slims toward upstream at step 8. The `_rec` IFO readers,
  report instrumentation and fs hooks keep their value; the policy half
  (title list, stream probe, cell heuristics, the `vob_css[100]` state
  block) comes up into `dvd/` as an app-side disc context (the title list
  is up since 8b, the stream probe since 8c, the cell rules since 8d).
- **libdvdcss** (+59) and **libaacs** (+941, cleanly additive incl. HD DVD)
  stay as they are. expat, tomlc17, CCExtractor (helper process) unchanged.

### 2.7 Build shape

`./build.sh [release|test]` as before. Each module is a static library:
`mk/base.mk → libdisc-base.a`, `io.mk`, `mux.mk`, `parse.mk` (later
`timeline.mk`, `disc.mk`, formats, `analyze.mk`). During migration FFmpeg
compiles with `--extra-cflags=-I<src>` and links the module libraries via
`--extra-libs` (its disc code still calls them); the program, the GUI and
the pytest/cffi bridge link them directly. Link order: engine → FFmpeg libs
→ io → mux → parse → base.

---

## Part 3 — Progress: what is done

Every step: moved with history (`git mv` where whole files), gated by
`./build.sh test` (735 passed at each commit), ledgered in `libs/README.md`
(rows 248–251).

### Step 1 — docs (c78f9627)
`docs/UNTANGLE.md` (the measured inventory of the old shape) and this plan.

### Step 2 — `src/base` (f4f331d4)
- MOVED: `log.c/h` (minus the FFmpeg hook), `msg.h`, `settings.c/h` (+
  tomlc17 compiled here). The sink (`EngineSink`, `engine_set_sink`) moved
  from `engine.h` into `base/log.h` — the GUI contract starts leaving the
  FFmpeg-including header.
- NEW: `err.h/c`, `mem.h/c` (checked wrappers, strlcpy/strlcat, asprintf),
  `buf.h/c` (refcounted, 64-byte zeroed padding, atomic count — ready for
  the timeline), `events.h` (the v2 contract), later `bytes.h` (rl/rb/wl
  16/24/32/64), `macros.h`, `vlog.h/c`, `crc16.h/c`.
- The FFmpeg log hook split into the program's `log_ffmpeg.c` (line
  reassembly, AVClass source naming, the end check's detail demotion),
  feeding `log_library_line()` — base is FFmpeg-free by build construction.
- Tests: +12 (`test_base.py`); bridge and both programs link base.

### Step 3 — `src/io` (5c501e4c)
- MOVED whole: `discio_read/fs/iso9660/udf_netbsd/udf_linux/folder.c`,
  `discio.h` (~8.6k lines). Shims only: `av_log`→`dr_vlog` (hook kept at
  `av_vlog` → every logged line byte-identical, 100+ log assertions green),
  allocs→`base/mem`, `AVERROR(e)`→same values, `AVERROR_INVALIDDATA`→
  `DR_ERR_INVALID` (4 test files take the value from the bridge now),
  `avpriv_open`→`open(O_CLOEXEC)`.
- FFmpeg's disc layer keeps calling the readers (`io/discio.h`), compiled
  with `src/` on its include path, linked via `--extra-libs`.

### Step 4 — `src/mux` + `src/parse` (three commits so far)

**cce5270c — the container splitters.**
- `mux/ps_disc.c/h`: mpeg.c's disc-sector reading moved as it was (it was
  already pure functions over `const uint8_t sec[2048]`).
- `mux/ts_disc.c/h`: mpegts.c's disc mode re-hosted on an event seam —
  `pes` (a PES begins; keeps stream-creation TIMING identical), `emit`
  (payload piece with its PTS), `gap` (stretches left out), `drop`, `note`,
  `error`. `TS_DISC_NOPTS` == `AV_NOPTS_VALUE` numerically.
- The demuxers keep wrapper duty: streams/packets, storing stretches for
  `ff_mpegts_disc_gap()` or logging them with the same words, `disc_err()`
  translating our error values back to FFmpeg's.

**ce0ed118 — the pure frame rules + one log hook.**
- `parse/rules.h`: `DrCutRule` (unit_size/resync/state) and `DrFrameRule`
  (sync/size/check/resync/limit) — the same member shapes the FFmpeg
  engines run, so `FFDiscCutRule` is now a typedef of ours.
- MOVED rules: TrueHD/MLP, VC-1 (start-code walk, its own state), DVD/HD
  DVD sub-pictures, ADTS, LOAS, Blu-ray LPCM, PGS display sets, TextST
  segments + the TextST segment reader (`parse/hdmv_textst.h`).
- `base/vlog.h` becomes the one hook; io's own hook removed.
- `parser.c`'s engine translates rule errors at its edge.

**9e074440 — the audio rules, header math ours.**
- `parse/ac3_rule.c`: A/52 frame-size table + syncinfo checks of our own;
  crc1/crc2 through `base/crc16.c` (bit-identical construction);
  `dr_ac3_stream_header()` (E-AC-3 bitstream info through the channel map).
- `parse/dca_rule.c`: DTS core + extension-substream header walks,
  `dr_dca_first_frame()`; the DTS-HD "does an extension follow" state in
  `DrDcaRuleCtx`.
- `parse/mpa_rule.c`: MPEG-audio header decode + Layer II allocation
  tables (spec values), free-format search, the side-information CRC.
- `parse/bits.h`: our bit reader; reads past the end return zero bits and
  keep advancing, so every overrun check fires where it did.
- The parsers keep hookups only; the HD DVD probe calls parse's checks.

**Step 4c — the H.264 / HEVC cutters and picture values (ledger row 252).**
- `parse/h264.h/_rule.c`, `parse/hevc.h/_rule.c`: the AU cutters moved as
  they were, AND the whole picture pass (type, key, display order, fields,
  rate, notes) — libavcodec's machinery replaced by our own spec-derived
  readers: SPS / PPS (+ VPS) walks, SEI (pic timing / recovery point), the
  slice header through the reference picture marking (MMCO 5), the POC math
  (8.2.1 / 8.3.1), exp-Golomb in `parse/bits.h` (with the table reader's
  over-long-code behaviour) and `parse/rational.h` (the rate reduction),
  all written to read bit for bit what the FFmpeg parse read (the 60 /
  1000-byte slice cap included).
- The two parsers keep only the hookup; their edits inside
  `parse_nal_units` reverted to upstream. `DR_ERR_UNSUPPORTED` added
  (engine edge: `AVERROR_PATCHWELCOME`).
- `pcm_dvd`'s 15-line rule deferred to 4d (its native host passes the LPCM
  frame size explicitly).

**Step 4d — the native splitter; no FFmpeg parser knows discs (row 253).**
- `parse/split.h/.c`: ONE splitter runs every rule — dr_split_write() /
  dr_split_next() / dr_split_end(); chunks carry their bytes, stream offset,
  status (unit / skip / refused / no-start / end), note and (H.264 / HEVC)
  the picture values. The engine halves keep the parser-host engines'
  decisions step for step. Per-codec hookup inside: the AC-3 kind
  (dr_split_take_any), the DTS extension-substream rule, the MPEG-audio DVD
  rule (dr_split_dvd), the LPCM frame format (dr_split_pcm_format,
  `parse/pcm_dvd_rule.c` — the 15-line rule moved), the H.264 / HEVC
  contexts and picture passes.
- `disctl_frames.c` re-hosted: the splitter path has no av_parser offset
  gymnastics and no shadow window (the splitter's window is authoritative);
  the H.264 frame rate persists in the frame step now (the last rate a
  unit's stream said). MPEG-1/2 video and E-AC-3 keep FFmpeg's stock
  parsers (they never had disc rules); only they still use av_parser.
  The HD DVD stream probe re-hosted the same way.
- THE DELETION: `parser.c`'s two engines, `PARSER_FLAG_DISC*`,
  `disc_status/note/fields` on AVCodecParserContext, every parser's disc
  branch and fields, the two pcm parsers and the PGS parser shell
  (deleted whole — they were ours); `parser.c`'s timestamp-lookup fix
  (historical row 1) reverted — nothing on the rip path reads av_parser
  timestamps — and its test removed with it. libavcodec's whole diff vs
  upstream is now −736/+4 lines.

**Moved so far: ~17k lines across 4 libraries; libavcodec is ~upstream
again.**

---

**Step 5 — the title timeline moved (rows 254, 255).**
- 5a: the audio header reads and core filters became ours in `parse/`
  (`dr_ac3_header` / `dr_dca_core_header` / `dr_mpa_header`;
  `dr_eac3_core` / `dr_dca_core` replace the eac3_core / dca_core
  bitstream filters, applied by the audio stage directly).
- 5b: the 11k-line timeline moved whole into `src/timeline`
  (`libdisc-timeline.a`), FFmpeg-free by build construction — on base and
  parse only. Its own codec ids (`DRCodecID`, value-pinned to the
  library's, asserted at build time), `DrBuf` buffers, base errors (the
  title reader translates EOF once at the seam and wraps a frame's bytes
  for the packet), the MLP major-sync CRC and the CCExtractor text
  buffers our own. What it still borrows goes through ONE host interface
  (`DRTimelineHost` in `timeline/disctl.h`, implemented by
  `libavformat/disctl_ffmpeg.c`, registered by the program and the test
  bridge): the stock frame parser (MPEG video, E-AC-3 — codecs that never
  had disc rules), the LATM stream-header decode and the TrueHD / MLP
  end-of-stream decode. Step 6 moves the host's duties into `analyze/`.
- The dependency note of §2.1 reads "base only" for the four leaf
  modules; since the splitter lives in parse, the timeline depends on
  base AND parse.

---

## Part 4 — What is left, in detail

**Step 6 — src/analyze stands; FFmpeg answers questions only (rows 256,
257).**
- `src/analyze` (`libdisc-analyze.a`) is the one module of ours that links
  the processing library; it is handed copies of data and answers
  questions. In it: the timeline's host (the stock frame parser for MPEG
  video and E-AC-3, the LATM stream header, the TrueHD / MLP end-of-stream
  decode; `dr_analyze_use()` registers it, with the codec-id build-time
  checks), the details probe and end check (the former `check.c`), the
  writer's channel-layout probe (`DrLayoutProbe`), and the stream pass's
  details probe (`pass_probe.c`, registered as `DiscProbeOps` through
  `libavformat/discprobe.h`; the pass keeps the probe list, cache, 3-D
  byte scan, limits, drops and logs). Absent a registration, details stay
  unknown and the pass says so — never a different rip.
- DEFERRED to step 7: the log flip (`dr_vlog` straight to `base/log.h`,
  the av_log funnel into analyze). Reason: today every module line's
  source still comes from the FFmpeg-side caller's AVClass context; the
  replacement — modules carrying their own source — only exists once the
  disc layer's contexts are ours. Flipping earlier would change
  attribution twice.

### Step 7 — `src/disc` + `hddvd/` + `bluray/` (the disc layer leaves FFmpeg)
In five slices, one commit and one full test run each:

- **7a — DONE (ledger row 258): the clip demuxer.** `src/disc/clip_demux.h`
  (`libdisc-disc.a`, the start of the disc module: base + mux + the
  timeline's headers, no FFmpeg under it) reads a clip's blocks from the
  caller and gives raw PES payloads; the title reader, the stream pass, the
  filler probe, the side clips and the HD DVD stream probe drive it
  directly. The AVIO + sub-demuxer seam is gone; **mpeg.c / mpegts.c /
  mpeg.h / mpegts.h are byte-for-byte upstream again** (their disc wrapper
  duties — the payloads' streams, the first-access-unit split, the PS
  counts, the TS stretches — moved into the clip demuxer with the same log
  texts). Errors cross the seam through `clip_err()`; the block readers
  return `DR_ERR_EOF`. Dropped as dead: the sub-demuxer's per-stream
  discard check (nothing ever set it; the `discarded` counts remain, at 0).
- **7b — the format parts move** near-verbatim into
  `src/disc/{dvd,hddvd,bluray}/` (the `*dec.c` adapters stay in libavformat
  until 7d and keep the `DiscFormat` vtable). Landed so far (rows 259–261):
  the language codes (`disc/lang.c`, own ISO 639-2 table), the disc-info
  model (`disc/info.h`, values pinned and asserted), and THE BLU-RAY PART
  (`disc/bluray/`, FFmpeg-free: base utilities, own SHA / CRC-32 / UTF-8
  read, libaacs + expat remain its libraries; errors as base values,
  translated once at the seams — `part_err()` in discdec). THE HD DVD PART followed
  (row 262) and THE DVD PART closed it (row 263) — all three format parts
  live under `src/disc/`, and libavformat keeps only the disc layer
  (discdec / discpass / disctitle) and the three thin adapters. 7b DONE.
- **7d — DONE (row 264; 7c folded in): the disc model.** The whole disc
  layer left libavformat: `discdec.c` → `src/disc/disc.c` with the typed
  public API `disc/disc.h` (`DrDisc` / `DrDiscOptions` / `DrDiscTrack` /
  `DrDiscFrame` / `DrDiscChapter`: no AVFormatContext / AVStream /
  AVOption / metadata dicts — the former stream metadata and dictionary
  results are fields), `disc.h` → `disc/model.h`, `disctitle.c` →
  `disc/title_reader.c` (frames out as `DrDiscFrame`, `DrBuf` references),
  `discpass.c` → `disc/pass.c`, `discprobe.h` → `disc/probe.h` (analyze's
  registration), the three `*dec.c` adapters → `src/disc/*/*_part.c`;
  `disclang.c`'s `avlanguage.c` link died with `disc/lang.c`. The program
  switched whole: the writer reads `dr_disc_read()` frames and takes the
  typed track results, options come typed from the settings
  (`DrDiscOptions`, key files as a list), `disc_mkv_codec_id` is the
  engine's own table (codecs.c), analyze grew track-based probe entry
  points (`details_probe_open_track` / `dr_layout_probe_open_track` and
  byte feeds), `engine.h` carries `disc/info.h`. The "disc" AVInputFormat
  died — **libs/ffmpeg is byte-for-byte the import commit again** (the
  upstream bluray protocol restored; the one historical fix, parser.c
  timestamp lookup #1, died at 4d); FFmpeg builds without `src/` on its
  include path and links none of our libraries.
- **7e — DONE (row 265): the log flip.** The modules log with their own
  source names (logctx = a source-name string through `base/vlog.h`;
  flipped at 7d, since no AVClass context is left on the disc path), and
  the hook is base's own: `log_init()` installs it (`vlog_line()` in
  `base/log.c` — no program mediation). The FFmpeg-line hook became
  `analyze/log.c` (`dr_analyze_log_install()`); the end check's detail
  demotion is `details_of_file()`'s own (`dr_analyze_log_detail()`), so
  the writer stopped toggling it; `log_ffmpeg.c/.h` died.

Gate: corpus byte-compare + info compare; the GUI must run (it still speaks
engine.h v1 until step 9).

### Step 8 — the DVD lift (D-8 DECIDED 2026-10-15: the full shape, in slices)
The end state: the DVD libraries keep only what is a library's job — **table
parsing and the navigation VM** — and everything else lives in
`src/disc/dvd/`: the title-VOB placement, the sector reads on our io with
the sector check, CSS through libdvdcss directly (the same pattern as the
Blu-ray part's AACS: decryption under the format part), the damaged ranges,
the VOBU records and map, the cell policy (walk/trim, checks, seamless
angles), segments, chapters, the title list, the stream probe and the
navigation-scan driver. No output byte passes through libdvdread then; the
VM's own nav-only reads (menus, NAV packs — policy inputs) still do, by
design. What stays in the libraries: libdvdread's `_rec` IFO readers with
their checks and report channel (the VM shares the IFO parse; table parsing
is a proper library job), the fs hooks, the NAV-packet checks; libdvdnav's
VM patch set, untouched.

In slices, one commit and one full test run each, behavior-preserving:
- **8a — DONE (row 266)** — the scan driver: libdvdnav's `scan.c` up into
  `dvd_scan.c` (app logic already; everything is handed in). Its lines go
  through `ff_dvd_lib_vlog()`, the "DVD: " voice the libraries' callbacks
  use (texts and levels unchanged); libdvdnav's diff is the VM patch set.
- **8b — DONE (row 267)** — the title list: `title_list.c` up into
  `dvd_title_list.c` (policy over the disc structure and the scan's
  recordings), with the title's stream step; on the DVD part's disc, its
  lines through `ff_dvd_lib_vlog()`.
- **8c — DONE (row 268)** — the stream probe up (`dvd_stream_probe.c`,
  with the sector's packet reader); its sectors still come through
  libdvdread's title-VOB reading until 8e.
- **8d — DONE (row 269)** — the cell rules + the app-side context:
  `vob_read.c`'s rules half up into `dvd_cells.c` (cell checks, seamless
  angles, the program chain check, cell list / CellWalk / CellTrim,
  segments, chapter places), on `DVDCells`, a per-title-set context the DVD
  part's disc owns (with the cell check's mode and random sample); the
  program chain table belongs to libdvdread's disc structure. The
  `ifo_handle_private_s` extensions stay: they are the IFO table reader's.
  Until 8e the rules ask libdvdread's reading for VOBUs (two small
  accessors, `DVDTitleVOBBlocks_rec()` / `DVDVobuMap_rec()`).
- **8e** — the mechanical half up, in two (decided 2026-10-15; placement
  leaves `DVDOpenDisc_rec()`, libdvdread's findings book stays the one
  report channel):
  - **8e-1 — DONE (row 270)**: the VOBUs (`dvd_vobu.c`: records, the VOBU
    map's trust, where VOBUs start and follow) on `DVDCells`; the map comes
    from the disc structure as read; placement and raw reads still asked of
    libdvdread.
  - **8e-2 — DONE (row 271)**: `vob_read.c` whole into `dvd_vob.c`
    (placement, sector read + check, CSS via libdvdcss, damage ranges, the
    file-view fallback) — the format part reads like the Blu-ray part
    does. Placement is the DVD part's code, asked by `DVDOpenDisc_rec()`
    through a callback at the moments it ran before (decided 2026-10-15:
    log order and broken-title-set behaviour unchanged). The reads go
    through the same file callbacks and lookup as libdvdread's reader
    (dvd_nav.c, our io).
- **8f — DONE (row 272)** — the libraries settled: stale declarations
  and comments out; libdvdnav keeps upstream's `searching.c` /
  `read_cache.c` (unchanged upstream files; decided 2026-10-15). Measured
  against the imports: libdvdread +2,483 / −156 (was +7,599: the `_rec`
  IFO readers and checks, `disc_read.c`, the findings book, the NAV-packet
  checks, the file hooks and lookup), libdvdnav +981 / −208 (was +2,449:
  the VM changes only); `src/disc/dvd/` holds 8,967 lines.

The detailed plan (inventory, seams, open sub-questions per slice) is in
the maintainer's workspace, implementation/PLAN_dvd_lift_step8_2026-10-15.md.
Gate: the 30-DVD title list/track comparisons re-run + corpus byte-compare
(with the rest of the rewrite's, run by the user at the end).

### Step 9 — engine API v2 (GUI-first)
What: `engine.h` v2 on the engine's events (`src/disc-remuxer/events.h`;
it began as `base/events.h`, but its disc and title events carry the disc
model, which base may not see): the disc model exposed as typed structs
with enum property keys (nothing matched by English names), rip results
as events (damage as arrays, not text blobs), selection written into the
model, display strings built from fields (the GUI's reverse-parsing of
`engine_track_parts()` and its QJsonDocument re-parsing go), `TitleOutcome`
crosses the boundary typed, JSON rendered only by the CLI. The warts list
to burn down is UNTANGLE.md §4, item by item.

The rule (user, 2026-10-15): ONE engine, two equal front ends in the same
program — the GUI (what the user works with) and the command line (what
tests and checks drive). Both speak the engine's typed API; no JSON or
text feeding, no external-UI protocol. PARITY: everything the GUI can
choose or see, the command line can choose or print, so a command-line
test covers what the GUI does; no rule or wording lives only in the GUI.

In slices, one commit each, the suite green, the command line's output
(text and `--json`) byte-identical unless a slice says otherwise:
- **9a — the events.** One emit point (`engine_event()`, `emit.h`): the
  six places that wrote JSON records build typed events (disc, title,
  title start, track result, title result, run result); the log's lines
  and progress become events too. A front end's sink (`engine_set_events()`)
  gets the structs; without one, JSON mode renders each event as the
  record it was (the same code, moved beside the event). The base log's
  sink (`LogSink`, `log_set_sink()`) keeps lines and progress only. The
  GUI's `LogRelay` copies each event into Qt values and the window takes
  typed signals (`onRecord`'s JSON parsing gone).
- **9b — DONE — what the front ends show is the engine's.** 9b-1: every
  disc / title property has a key (`DiscPropKey`, `src/disc/props.c`: its
  name, group, label, language kind), so nothing is matched by English
  words. 9b-2: the engine builds the details (rows: heading, sub-heading,
  "label: value" at a depth; `views.h` / `views.c`: `engine_disc_rows()`,
  `engine_title_rows()`, `engine_track_rows()`, `engine_chapters_rows()`)
  and the queue's one-line texts (`engine_disc_line()`,
  `engine_title_line()`, `engine_title_state()`, `engine_track_line()`,
  `engine_chapter_line()`, times and sizes); the GUI's details panel and
  queue only lay them out (its own wording, rules and FFmpeg headers gone),
  and `info --details` prints the same rows and lines. Gate besides the
  suite and the command-line comparison: every disc the suite builds
  snapshotted by the old and the new GUI, compared byte for byte.
- **9c — selection in the engine**: titles, splits, tracks, second view
  through engine calls both front ends use.
- **9d — the parity pass**: every GUI action and view gets its engine call
  and its command-line form (per-track results, damage ranges, chapters).
- **9e — the GUI shows the new data** (the user's wishes, asked first).

Gate: the suite + a before/after comparison of the command line on every
test that drives it (the same arguments to the old and the new program:
stdout, stderr, exit code and every written file); GUI feature parity by
hand; no rip-path change.

### Step 10 — the MKV writer, then retirement
The writer once every format's streams pass (decision D4: libebml/
libmatroska vs an own EBML writer — the streams and timeline are done by
then, so it is a pure output module with the end check behind it). Then the
old in-FFmpeg scaffolding is fully gone, `legacy/rust`'s remaining test
ports continue as before, and main gets the merge.

### Open decisions (ask the user at the step)
- D-4c: pcm_dvd now (rule signature grows a format argument) or at 4d.
- D-6: analyze/'s decode-vs-parse precedence when they disagree (today the
  decoder wins for decoded truths; keep, revisit per case).
- D-8: DECIDED (2026-10-15) — the full slim, in slices (step 8 above): the
  libraries keep table parsing (`_rec` readers) and the VM; reading, CSS
  and policy come up. The `_rec` readers stay library-side.
- D-10: libebml/libmatroska vs own EBML writer.
- Merge points: the user merges to main when he has byte-compared real
  discs (earliest sensible: after 4d; safest: after 7).

## Part 5 — How to verify at any point

- `./build.sh test` — the suite (735 + growing; every moved module keeps
  its tests, the bridge links the module libraries).
- Corpus byte-compare: build main and the branch, `demux` the same titles,
  `cmp` every output file, diff the logs (texts must match; the version
  line and, after step 6, library source tags may differ).
- `info` compare: field-for-field on every corpus disc (JSON records).
- The ledger: every FFmpeg-touching change is one row in `libs/README.md`
  with its test; the rewrite's rows are 248+.
