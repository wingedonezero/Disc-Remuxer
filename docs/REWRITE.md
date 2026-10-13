# The rewrite: architecture and progress

Updated 2026-10-13, on branch `claude/rewrite-untangle-1c24ca` (the rewrite
branch; main still holds the pre-rewrite program and is the byte-compare
reference). Inventory of what the old shape was made of: `docs/UNTANGLE.md`.

Status: **steps 1–6 done** (part 3; one piece of 6 deferred: the log flip
waits for step 7's own log contexts). The next step is 7 (the disc layer
leaves FFmpeg).
Tests at every commit: 735 passed, 15 skipped (the disc-marked ones without a
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
  (`dr_vlog(logctx, level, fmt, ...)`). During migration the program and the
  test bridge point it at FFmpeg's `av_vlog`, so every line keeps its exact
  text AND its source attribution (the AVClass of the caller). When the disc
  layer leaves FFmpeg (step 7), the hook points at `base/log.h` directly and
  `log_ffmpeg.c` moves into `analyze/`.
- The DVD libraries already log through their callback structs into the same
  funnel; libaacs through its handler. Nothing prints on its own.

### 2.4 Errors, events, settings

- Errors: `base/err.h` — 0 ok, `-errno` for system errors (numerically
  identical to FFmpeg's `AVERROR(e)`, so values cross the migration seam
  unchanged), `DR_ERR_*` for our conditions (EOF, invalid data, no memory,
  limit, cancelled, external). Where our values reach FFmpeg-side consumers
  during migration, the wrapper translates at the edge (`DR_ERR_INVALID` ->
  `AVERROR_INVALIDDATA`), so tests and messages see no change.
- Events: `base/events.h` — the engine-v2 contract. One `DrEvent` union
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
  scan driver (`scan.c`) moves UP into `dvd/` at step 8.
- **libdvdread**: slims toward upstream at step 8. The `_rec` IFO readers,
  report instrumentation and fs hooks keep their value; the policy half
  (title list, stream probe, cell heuristics, the `vob_css[100]` state
  block) comes up into `dvd/` as an app-side disc context.
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
What: `discdec.c`/`disc.h`/`discinfo.h` → our own disc model (no
AVFormatContext/AVStream/AVOption/metadata dicts: typed structs end to
end), `disctitle.c` driving `mux/` directly (the AVIO + sub-demuxer seam
goes), `discpass.c` on mux/parse/analyze, `disclang.c` with its own ISO-639
table, the HD DVD and Blu-ray parts near-verbatim (utility-coupled).
This deletes: the "disc" AVInputFormat, the mpeg.c/mpegts.c disc wrappers
(mux/ is called directly), the last libavformat edits except nothing —
**libs/ffmpeg returns to ~upstream** (the one historical fix, parser.c
timestamp lookup #1, died at 4d).
The writer opens the disc layer natively; options come typed from settings.
Gate: corpus byte-compare + info compare; the GUI must run (it still speaks
engine.h v1 until step 9).

### Step 8 — `src/dvd` (the DVD lift)
What: up from libdvdread into `dvd/`: `title_list.c`, `stream_probe.c`, the
policy half of `vob_read.c` (cell walk/trim, decoys, segments, chapters),
keyed by an app-side disc context that replaces `dvd_reader_s.vob_css[100]`
and the `ifo_handle_private_s` extensions; up from libdvdnav: `scan.c` (the
driver; the VM patch set stays). The findings book is base's already; the
report structs become app types both libraries' callbacks feed.
libdvdread's remaining diff: `_rec` readers + instrumentation + fs hooks
(or further slimming if the `_rec` readers come up too — decide there).
Gate: the 30-DVD title list/track comparisons re-run + corpus byte-compare.

### Step 9 — engine API v2 (GUI-first)
What: `engine.h` v2 on `base/events.h`: the disc model exposed as typed
structs with enum property keys (nothing matched by English names), rip
results as events (damage as arrays, not text blobs), selection written
into the model, display strings built from fields (the GUI's reverse-
parsing of `engine_track_parts()` and its QJsonDocument re-parsing go),
`TitleOutcome` crosses the boundary typed, JSON rendered only by the CLI.
The warts list to burn down is UNTANGLE.md §4, item by item.
Gate: GUI feature parity by hand + the suite; no rip-path change.

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
- D-8: how far libdvdread slims (policy-only lift vs `_rec` readers too).
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
