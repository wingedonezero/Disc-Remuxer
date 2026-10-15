# UNTANGLE — what the current program is made of, and what it really depends on

Written 2026-10-13 from the code as it stands (main at 2f94869b). Every number is
measured against each library's import commit (`git diff 7e3ad3ba... --stat`).

## 1. The parts, by size

| Part | Where it lives today | ~Lines | What it is |
|---|---|---|---|
| Readers | `libs/ffmpeg/libavformat/discio_*` | 8,570 | Block reads with retries, ISO 9660/Joliet, two UDF readers (NetBSD- and Linux-derived), disc folder on the host, file-system choice rule, disc-format detection |
| Disc layer | `discdec.c`, `disc.h`, `discinfo.h`, `discpass.c`, `disctitle.c`, `disclang.c` | 4,530 | The one "disc" demuxer: open once, disc kind, title list, stream pass, title reader, language table |
| DVD format part | `dvddec.c`, `dvd_*.c` | 2,200 | Thin adapters over our libdvdread/libdvdnav additions |
| HD DVD format part | `hddvddec.c`, `hddvd_*.c` | 3,400 | VTI, XPL playlists (expat), time maps, AACS, title plan, tracks |
| Blu-ray format part | `bluraydec.c`, `bluray_*.c` | 8,760 | BDMV/BDAV/AVCHD structure, MPLS/CLPI/index, AACS + content hash, decoy playlists, menu names, titles, tracks, its own raw-TS probe |
| Title timeline | `disctl_*.c`, `disctl.h` | 11,130 | The format-independent core: frame step, video grid, audio durations, joiner, junction, PCM strategy, SPU/PGS/TextST, captions, chapters, verify |
| In-place FFmpeg edits | `mpeg.c`, `mpegts.c`, `parser.c` + 14 codec parsers, `avlanguage.c` | ~4,100 added | PS sector rules, TS per-PID PES reassembly, the generic disc cut/frame engines, each codec's frame rules in "disc mode" |
| libdvdread additions | `disc_read.c`, `vob_read.c`, `title_list.c`, `stream_probe.c`, `_rec` IFO readers, findings book | +7,599 | IFO/BUP redundant reads, raw table readers, VOB sector reading + CSS + damage ranges, cell heuristics, title list, stream probe |
| libdvdnav additions | `scan.c`, VM edits | +2,449 | The navigation scan (app logic), and invasive VM changes it needs (crash-proofing, deterministic random, playback clock, nav-only skipping) |
| libdvdcss additions | 2 exports | +59 | Title key out, descramble-with-key |
| libaacs additions | `hddvd.c`, `bd.c`, `keyfile.c` | +941 | Cleanly additive: HD DVD and key-file AACS paths, log hook |
| Engine | `src/disc-remuxer/` | 4,598 | Settings, log, messages, selection, writer (all output containers hand-written, **no FFmpeg muxers**), end check, names |
| GUI | `src/gui/` (Qt 6 Widgets) | 2,214 | Queue, details, prefs from the settings registry, console, engine on a worker thread |
| Tests | `tests/` (pytest + cffi) | 72 unit files | Call the C directly on byte-built data |

## 2. The coupling map — what actually needs FFmpeg

Roughly **80 % of our ~43 k lines in `libs/ffmpeg` is FFmpeg-free logic or
utility-only** (`av_log`, mallocs, `AVERROR`, byte readers — all shimmable).
The real anchors are exactly four:

### Anchor 1 — the parser host (self-inflicted, simpler standalone)
`disctl_frames.c` runs every codec's frame cutting through
`av_parser_parse2()`; our rules live in the parsers behind `PARSER_FLAG_DISC`
and two generic engines in `parser.c` (`ff_parser_disc_cut`,
`ff_parser_disc_frames`). Those engines are contorted to honour the
AVCodecParser return-value contract. Standalone, this becomes a plain push
splitter (feed bytes → get units + why-skipped), and the offset gymnastics
disappear. The per-codec rules themselves are pure header math (AC-3, DTS,
MPEG audio, AAC, LATM, MLP, VC-1, dvdsub, PGS, LPCM — trivial to lift;
H.264/HEVC medium: they need a small NAL/RBSP + SPS/PPS/slice-header reader
of our own).

### Anchor 2 — the container splitters (cores already pure)
- `mpeg.c` disc mode: the PS pack/sector logic is already pure functions
  (`ff_mpegps_disc_packet()` over a `const uint8_t sec[2048]`); only the
  AVStream/AVPacket wrapper is FFmpeg glue.
- `mpegts.c` disc mode: its own per-PID PES reassembly state machine
  (`DiscTsPid`), barely touching upstream TS code. Extracts naturally.

### Anchor 3 — the AVFormat plumbing (mechanical)
`discdec.c` is registered as an input format; `disctitle.c` drives mpeg/mpegts
as sub-demuxers over a custom AVIO callback; `writer.c` opens "disc" with an
options dict and reads packets; results ride as **stringly-typed AVDictionary
metadata** (`disc_track`, `language`, `frames`, `warnings`, `damage`, ...)
parsed back out of decimal strings. All of it is a data-carrier choice, not
logic — and it is one of the things that made the lines feel blurred.

### Anchor 4 — decoders (the only genuine design decision)
Every decoder use site:

| Site | What for | Standalone answer |
|---|---|---|
| `discpass.c` details probe | One decoded frame per track for the info list: width/height/aspect, frame rate, profile/level, HDR10/HDR10+/HLG mastering + CLL, Dolby Vision MEL/FEL, channels/layout/rate/bits | The big one. Options: own header/SEI readers, or hand a dumped sample to an **external** ffprobe/ffmpeg process |
| `check.c` end check | Decode the written file back, compare ~25 stream details against the disc side | Both sides can go through the same external ffprobe (same build = same field words) |
| `writer.c` channel-layout probe | Decode until a frame reveals the layout for the file name | Falls out of the details probe |
| `disctl_mlp.c` | TrueHD stream-end AU sample count | Substream-directory math fallback already exists in the file |
| `disctl_codecs.c` LATM | Decode one LOAS frame for rate/samples | Replaceable with a StreamMuxConfig parser |
| `disctl_audio.c` BSFs | `eac3_core` / `dca_core` extraction | Trivial once frames are already cut (drop dependent frames / keep core) |

**No FFmpeg muxers are used anywhere.** Every output container (WAVE, VobSub,
SUP, SRT, chapter XML, elementary streams) is already hand-written stdio.

## 3. The DVD libraries — what belongs where

- **libdvdread**: ~60 % of the +7.6 k is application policy (the whole
  `title_list.c` and `stream_probe.c`, the heuristic half of `vob_read.c`:
  cell rejection/trim/walk, decoy PGCs, segments, chapters). It lives in the
  library only to reach `dvd_reader_s` internals (the `vob_css[100]` state
  block in `dvdread_internal.h`) and the shared log channel. Externalizing
  that one state block unlocks the lift. The other ~40 % (IFO/BUP redundant
  reads, raw table readers, sector reads + damage + CSS) reads raw bytes
  itself — it needs a file layer, not libdvdread's parsing: it could live in
  the app with **no DVD library at all** underneath.
- **libdvdnav**: `scan.c` (1,285 lines) is app logic on the public API — lifts
  out almost verbatim. The VM edits (~1,100 lines: asserts→recorded failures,
  deterministic random, playback clock, counter semantics, nav-only skipping,
  `open_files`, working `dup`) **cannot** lift out; the scan requires them
  from underneath. Keep a vendored libdvdnav with this small, delimited patch
  set — or own a DVD VM (~4 kLoC; not worth it now).
- **libdvdcss**: +59 lines, trivially additive. Keep (or absorb: the
  descramble is ~40 lines of public algorithm).
- **libaacs**: nearly clean additions. Keep; or copy the AES-G/MKB/keydb
  pieces (~1–2 kLoC) into the app and drop the library (no drive/MMC code is
  used).

## 4. The engine/GUI seams that blurred the lines

- `engine.h` includes `libavformat/discinfo.h` → FFmpeg types leak into every
  GUI file.
- Rip results (title start/track/result) exist **only as JSON text records**;
  the GUI re-parses them with QJsonDocument; `TitleOutcome` never crosses the
  boundary; `TitleOutcome.damage` is a 4 KiB newline text blob.
- The GUI reverse-parses English suffixes out of `engine_track_parts()` text
  (`lastIndexOf("  (forced subtitles of track"...)`) for data that exists
  structurally in `DiscStreamInfo`.
- Disc properties are matched by English name ("Fake playlist check").
- The log dedup keys on English line prefixes ("Title check: ", ...).
- Settings → demuxer options go through an AVDictionary of strings mapped 1:1
  from the registry.
- Messages: ours are numbered by area (1000s program … 6000s output);
  library lines carry code 0 + a source string derived from the AVClass item
  name via the one `av_log_set_callback` funnel (with line reassembly and
  dedup). The design is sound; only the ownership (FFmpeg's callback as the
  transport) changes in a rewrite.

Status (2026-10-15, rewrite step 9): every item above is gone. The engine
header carries our own disc model (step 7d); rip results are typed events
(9a); details, queue texts and property keys are the engine's (9b); the
damaged ranges are a list (9e-2); the repeat filter decides by when a line
comes, not by its words (9e-3); settings reach our own modules directly
(steps 4–7); the processing library's lines come through analyze's one hook
(step 7e).

## 5. What is already clean (keep as-is)

- The writer's hand-rolled output containers; the names template engine
  (zero deps); the settings registry + TOML file; the one-settings-system
  rule; the GUI structure (worker thread, sink relay, prefs from the
  registry); the findings-book idea (dedup + caps + summary) in the DVD libs;
  the per-change ledger discipline of `libs/README.md`; the pytest+cffi test
  method; the byte-compare verification discipline.
