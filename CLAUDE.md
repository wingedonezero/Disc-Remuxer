# Disc-Remuxer — session rules (READ BEFORE STARTING WORK)

This is a long-running multi-session reverse-engineering project. The
**atlas** (research, decomp notes, function map) lives in
**`/home/chaoz/Desktop/Makemkv/Tests/Disc-Remuxer/`**. The **implementation
repo** is this directory (`/home/chaoz/Desktop/Programs/Disc-Remuxer/`).
Two directories. Never mix.

Goal: a Rust CLI tool (`disc-remuxer`) that reads DVD / Blu-ray / UHD discs
and extracts byte-identical elementary streams. We use the atlas to
*understand* what proven correct disc-handling looks like, then write our
own clean Rust on top of upstream OSS libraries (libdvdread, libdvdcss,
libdvdnav for DVD; libaacs/libbluray/libbdplus later for BD/UHD). The
implementation is original; the atlas is private research.

**Reference version is MakeMKV 1.18.4** (moved 2026-06-30; 1.18.3 archived
under `Tests/Disc-Remuxer/OLD/`). Sources live in `Tests/Disc-Remuxer/Sources/`.

## Clean-room policy (read this twice)

The atlas notes and decomp dumps under `Tests/Disc-Remuxer/` are research
artifacts. **Nothing from them — no specific function addresses, no source
binary names, no internal identifier conventions — appears in this repo's
code or commit messages.** Use:

- **Public specs** for justifications: DVD-Video Book, MPEG-PS, SCSI MMC,
  EBML/Matroska, AACS-LA, BDA BDMV — all publicly documented.
- **Public library APIs** for naming: libdvdread / libdvdnav / libdvdcss
  field names exactly (`tt_srpt_t::nr_of_srpts`, `pgc_t::nr_of_cells`,
  `dvdnav_get_next_block`, etc.).
- **DVD/BD spec terminology** for fields not exposed by the libraries.

The atlas's `impl_path` / `impl_function` columns record which `FUN_<addr>`
maps to which Rust function — that mapping is research-internal and stays
in `atlas.tsv`. It does **not** appear in code docstrings, comments, or
commit messages. The atlas integrity check greps the source for the
**Rust symbol** (`impl_function`), never for `FUN_<addr>`.

---

# Part 1 — The atlas (v2)

## The v2 model: progress and trust are separate axes

The atlas is **one row per function** in `atlas.tsv` (9,634 rows, 24 columns)
plus a **prose encyclopedia article** per examined function in
`per_function/<FUN>.md`. Everything else — status dashboards, cone maps,
worklists, the corpus-gap ledger — is a **regenerated view**, never a
separate source of truth.

v1 used a single `analysis_level` L0–L4 axis that conflated *how far we got*
with *how sure we are*, so a heuristic guess looked exactly as authoritative
as a verified fact. Some of those guesses were wrong. **v2 splits the axes,
and that split is the whole point:**

| axis | values | means |
|---|---|---|
| **`status`** | `unknown → traced → classified → examined → verified` | how far analysis has progressed |
| **`confidence`** | `none → hypothesis → evidenced → verified` | trust in the format+role claim |

- `traced` = observed firing in a capture. A **mechanical fact.**
- `classified` = format+role assigned with cited evidence, no article yet.
- `examined` = full encyclopedia article, control-flow + callees understood.
- `verified` = confirmed against a trace AND, where it produces output, byte-compare.
- `hypothesis` = a guess (pattern, LLM label, firing pattern). **NOT validated.**
- `evidenced` = backed by decomp/trace evidence read under v2.

`fired_on`, `oss_match`, `callers`/`callees` are mechanical and trustworthy
regardless of `confidence` — that axis is specifically about the *semantic*
classification.

**Buckets (B1–B8) and levels (L0–L4) are gone.** Do not use them, do not
look for `atlas.py next --bucket=`, do not try to `rebucket`. The
replacement is two orthogonal vocabularies:

```
format:  unknown / foundation / dvd / bd / uhd / hddvd / protection / cli / oos
role:    unknown / byte_deciding / glue / io / vm / codec / demux / mux /
         subpic / nav / container / crypto / obfuscation
```

## Step 1 — orient yourself

```bash
/home/chaoz/Desktop/Makemkv/Tests/.venv/bin/python \
    /home/chaoz/Desktop/Makemkv/Tests/Disc-Remuxer/atlas/atlas.py status
```

Then read `Tests/Disc-Remuxer/atlas/schema_v2.md` and, if `atlas.py report`
has been run, the regenerated views in `atlas/views/`.

> The v1 companions (`status.md`, `TRACE_INDEX.md`, `ISSUE_TRACKER.md`,
> `FULL_CONE_MAP_*.md`) now live under `OLD/` and describe the **1.18.3**
> binary. They are historical. See "Mechanical facts" below for why.

## Mechanical facts you must know before touching anything

**1. Addresses are not stable across MakeMKV versions.** The 1.18.3 → 1.18.4
rebuild kept only **489 of 9,628** addresses (~5%). Every address-keyed v1
artifact — the 1,643-row LLM summary pass, the v1 per-function notes, the
cone map, the BSim OSS labels — is stranded. **Do not remap them in.** Some
v1 semantic labels were wrong, which is why v2 was rebuilt clean; importing
them re-imports the contamination. Mechanical artifacts (BSim `oss_match`,
cluster membership) are **re-derived directly against 1.18.4**, never
migrated. Any new bulk pass must store a **content fingerprint** alongside
the address so the next version bump is a remap, not a redo.

**2. Half the binary is string-deobfuscation and can be excluded mechanically.**
A function whose only non-libc callees live in the `0x0048xxxx`/`0x0049xxxx`
family is a deobfuscated-string builder. That is **5,143 of 9,634 (53%)**.
It produces no output bytes → `format=oos`, `role=obfuscation`. Independently
re-derived on 1.18.4; matches the v1 count (5,125) to within 0.4%. That leaves
**4,491 functions** that actually need analysis, not 9,634. Implemented in
`analysis/sweep/mechanical_tier.py` — run it, don't re-derive it by hand.

This is the *only* rule that earns a mechanical exclusion. A companion rule
excluding small leaf functions by size was tried and **rejected**: size does
not imply role, and it mislabelled a 35-byte `mux` function and a 39-byte
`io` function as `glue`, and asserted `glue` on a function we had honestly
marked `unknown`. See `analysis/sweep/README.md`. **If a mechanical rule
cannot cite evidence about behaviour, it does not get to assign a role.**

**3. `format` is already populated as a hypothesis for the traced rows.**
`outputs/set_format_hypothesis.py` derived it from the firing pattern
(`uhd`-only→uhd, `dvd`-only→dvd, `bd`/`bd,uhd`→bd, all-three→foundation) at
`confidence=hypothesis`, `provenance=trace_fired`. Don't redo it. **Do**
correct it when content contradicts firing — a function that fires only on
UHD but whose strings are all BD-J is `bd`, not `uhd`.

**4. Never-fired ≠ dead.** 6,399 rows have never fired, but that means *our
corpus doesn't cover them*, not that they're unreachable. A never-fired
function whose content is clearly HD-DVD or BD+ is a `corpus-gap`, recorded
with `atlas.py corpus-gap <addr> --needs=<disc-or-feature>`. Never bulk-retire
a function for not having fired.

## Step 2 — pick work

```bash
atlas.py next --format=dvd --limit=20
```

There is no tier order any more — `format` and `role` are independent, so
work is prioritised by **byte-impact**, leaf-first within a callee tree. A
function whose callees are still `unknown` is harder to trust; prefer leaves.

## Step 3 — classify (cheap) or examine (expensive)

Two commands, deliberately different in cost. **Use the cheap one by default.**

### `classify` — format + role + a cited reason, no article

```bash
atlas.py classify 00720b80 \
    --format=dvd --role=demux --name=ps1_substream_header_parse \
    --confidence=evidenced \
    --evidence="switch on (id&0xf800): 0x8000=AC3, 0x8800=DTS, 0xA000=LPCM; \
reads BE16 first_access_unit_pointer then advances 3"
```

`--evidence` is **required**. It is the auditable link between the label and
what justified it. A classification with no citable evidence is a guess —
record it at `--confidence=hypothesis` or not at all.

**Abstention is a correct answer.** If a function has no strings, no
informative callees, and an opaque body, leave it `unknown`. Recording
`unknown` honestly is strictly better than a plausible label that later
poisons downstream work. That is the exact failure v2 exists to prevent.

### `examine` — the full encyclopedia article

Reserve this for functions that **decide output bytes**. Write the article
to `atlas/per_function/FUN_<addr>.md` using the template in `schema_v2.md`:

```
# FUN_<addr> — <semantic_name>
| address | size | status | confidence | format | role |
## What it does            (one paragraph — the lookup summary)
## Control-flow shape      (branches, loops, dispatch tables, magic numbers)
## Callers                 (who triggers it, when — [[FUN_x]] links)
## Callees                 (what it relies on — [[FUN_y]] links)
## State touched           (struct offsets, globals, constants)
## Trace evidence          (which runs fire it, sample args)
## OSS                     (oss_match / delegates_to)
## Byte-impact             (byte_deciding? why — verdict + evidence)
## Confidence & provenance (why we believe the above, and how sure)
```

then:

```bash
atlas.py examine 00720b80 --note=atlas/per_function/FUN_00720b80.md \
    --format=dvd --role=demux --name=ps1_substream_header_parse \
    --confidence=evidenced
```

### Other row commands

```bash
atlas.py show <addr>                              # everything known about one row
atlas.py set-delegation <addr> --to=libdvdnav:dvdnav_get_next_block
atlas.py corpus-gap <addr> --needs=hddvd          # known-but-never-fired
atlas.py ingest-trace --tag=dvd --set=analysis/dvd_01_ASBY_2582_ISO.set
atlas.py implement <addr> --path=… --function=… --kind=…
```

## Step 4 — keep the atlas honest

```bash
atlas.py verify       # must exit 0
atlas.py report       # regenerate atlas/views/
```

`verify` blocks if a row claims an implementation without the cited
`impl_path` existing or without the `impl_function` symbol appearing in that
file. **Never bypass it.** If verify fails, fix the row or the code.

Pure implementation-side sessions that don't touch the atlas don't need this.

---

# Part 2 — The implementation

## Crate layout

```
crates/
├── libdvdcss-sys/    FFI bindings — vendored libdvdcss build via meson
├── libdvdread-sys/   FFI bindings — vendored libdvdread (linked to libdvdcss)
├── libdvdnav-sys/    FFI bindings — vendored libdvdnav
├── disc-core/        format-agnostic foundation: DiscType + detect, DiscError
│                     (generic variants + a Backend{source} wrapper), check::*,
│                     and the uniform selection model — model (TitleCollection/
│                     Title/Track, each with an `enabled` flag), selection
│                     (Selection + mark_min_length), backend (DiscBackend trait
│                     + Session). NO format concepts.
├── disc-dvd/         all DVD: libdvdread/css/nav wrappers, DvdError, DvdBackend
│                     (IFO→TitleCollection enumerator), the demuxers
│                     (demux/video_es/vobsub/mpegps/nav/...), and ops/ — the
│                     orchestration. ops::rip_title is THE rip pipeline;
│                     dump_*/demux_*/scan_streams/info are isolated stages.
└── disc-cli/         thin command triggers (parse args → call a disc-dvd op →
                      print). src/ top = info/list/rip (user pipeline);
                      src/dvd/ = `dvd <tool>` diagnostics. (clap, flexi_logger, sha2)
vendor/
├── libdvdread/       git submodule, pinned tag
├── libdvdcss/        git submodule, pinned tag
└── libdvdnav/        git submodule, pinned tag
```

Add new code to the appropriate crate. Build with `cargo build`. The CLI
binary lives at `target/release/disc-remuxer`.

> Fresh worktrees need `git submodule update --init vendor/libdvd*` before
> `cargo build`, or the `-sys` crates fail.

## Coding conventions

1. **Field names mirror libdvdread / libdvdnav public C headers verbatim**
   (`tt_srpt_t::nr_of_srpts`, `pgc_t::nr_of_cells`, `cell_playback_t::first_sector`,
   etc.). Rust structs and variables follow the same names. Anyone reading
   the code can grep them in `<dvdread/ifo_types.h>` / `<dvdnav/dvdnav.h>`.
2. **No mention of the research source binary in code or commits.** See
   the clean-room policy at the top of this file. Use public-spec
   citations instead (DVD-Video, SCSI MMC, MPEG-PS, etc.).
3. **Every observable step logs.** `log::info!` for lifecycle events,
   `log::debug!` for inner steps, `log::trace!` for byte-level detail.
   `disc_core::check::{check_eq, require_eq, check_in_range, check}` for
   PASS/FAIL verifications, all under the `disc_check` log target.
4. **Every invariant we know about gets a check.** If the atlas tells us
   "this VOB sector should start with 00 00 01 BA on a cleartext disc,"
   that becomes a `check!` in our code, logged PASS/FAIL with the actual
   bytes shown. Transparency at every step — not after-the-fact debugging.
5. **Errors:** `disc-core` exposes a format-agnostic `DiscError` (generic
   variants + a `Backend { source }` wrapper). Each backend keeps its own
   error type — `disc_dvd::DvdError` for the libdvdread/css/nav primitives —
   and converts to `DiscError::Backend` at the public boundary
   (`From<DvdError> for DiscError`). The `disc-dvd::ops` orchestration layer
   and the CLI use `anyhow::Result` (rich `.context`). Every variant / context
   carries enough detail (path, offset, count, …) to debug from the log alone.
6. **Job-log support** is built in via `flexi_logger`. CLI subcommands
   that produce output accept `--log-file <path>` (global flag) to mirror
   the structured log into a file with timestamps. Wire this in for any
   new "rip-like" subcommand so the operation is self-documenting.

## Recording an implementation in the atlas

```bash
atlas.py implement <addr> \
    --path=crates/disc-dvd/src/file.rs \
    --function=DvdFile::read_blocks \
    --kind=semantic_port
```

`--kind` choices:
- `exact_port` — behavior matches the decomp exactly (used where divergence
  breaks byte-identity, e.g. cell-walk ordering, structural-protection skip)
- `semantic_port` — matches the equivalent published spec / OSS library
  behavior; we use the upstream library where one exists
- `mechanical_port` — boilerplate that came along with an owning class
- `hardcoded` — baked-in constants (deobfuscation tables, magic strings)
- `skipped` — out-of-scope, replaced by upstream library, or dead code

## Verifying

`byte_compare_pass` is the gold standard — `cargo build --release &&
./target/release/disc-remuxer rip …` output should match the captured
reference rips in `Tests/Disc-Remuxer/outputs/makemkv/` byte-for-byte,
excluding the 24 per-rip random bytes (SegmentUID + DateUTC + the cascading
CRC-32s — see the `project_random_ebml_fields` memory).

Smaller verification gates:
- `cargo test` — unit + integration tests must pass clean
- `disc-remuxer dvd dump-sectors … && sha256sum` — for the sector-read layer,
  hash must match `dd` of the same range on an unscrambled corpus disc

---

# Part 3 — Hard-won ground truth

## Hard rules — non-negotiable

1. **Progress and trust are separate.** Never record a semantic claim above
   the confidence its evidence supports. `unknown` and `hypothesis` are
   legitimate, useful answers. A confident wrong label is the most expensive
   thing you can put in the atlas.
2. **Clean-room separation.** Code and commits in this repo cite *public
   specs and library APIs only*. The research source binary's name and
   `FUN_<addr>` identifiers do **not** appear in source files or commit
   messages.
3. **Commits are tagged.** Every commit is `[infra]` / `[tooling]` /
   `[test]` / `[binding]` in the subject. (`[port]` and `FUN_<addr>` in
   subject lines are the old convention; do not use them.)
4. **No invented functions.** If you can't find an atlas note and no OSS
   analogue exists, *stop and trace* (`Tests/Disc-Remuxer/traces/tools/ptrace_tracer`)
   or *read more decomp*. Never write a function that "feels right."
5. **No defensive code we can't justify from spec or atlas.** Faithful
   first. If a check looks defensive but appears in the atlas notes for the
   equivalent function, keep it. If it's a "what if" hypothetical, drop it.
6. **Byte-compare is the only success metric.** Test suites that count MSGs
   or check metadata fields can pass while bytes diverge.
7. **One rip pipeline.** No ffmpeg-pipe fallback, no "we'll retire this
   later" parallel path. If you can't make it work, mark the row blocked
   and surface the issue.
8. **If a behavior is ambiguous** → `traces/tools/ptrace_tracer` on the
   relevant disc, OR read more decomp from callers/callees. Never guess.

## DVD rip ground truth (verified 2026-05-22 vs MakeMKV mkvextract)

These are the lessons that took the longest to learn the hard way.
Re-read before touching the demuxer, the file naming, or anything
about audio.

### Audio bytes: naive PES-payload concatenation IS the answer

**MakeMKV preserves every audio byte the encoder emitted.** Across cell
boundaries with `stc_discontinuity == true`, do NOT drop bytes:

- No first_access_unit_pointer (FAP) resync.
- No trailing partial-frame truncation.
- No PTS-aware byte drops.

Verified on ANGEL_S1D1 title 1 (44 min, 23 cells, 9 stc_discontinuity
boundaries, 4 AC-3 streams): naive concat produces SHA-identical bytes
to `mkvextract` of MakeMKV's MKV for ALL 4 audio tracks. An earlier
implementation of FAP-resync dropped 7 bytes per stream and was wrong —
reverted in commit `ca1256f`.

The relevant FFmpeg patch the user maintains
(`/home/chaoz/Desktop/Programs/FFmpeg` HEAD,
`avformat/dvdvideo: fix AC3 frame loss at PTM discontinuity boundaries`)
fixes a DIFFERENT bug — wrongly DISCARDING valid frames whose PTS
equalled `prev_pts` after a PTM reset. Same principle: byte stream
stays intact, time base resets.

> **This rule is DVD-specific — do not generalise it across formats.**
> On BD/UHD seamless-branching discs MakeMKV *does* drop whole audio
> frames at PlayItem junctions. See the BD section below.

Per-PES byte stripping for DVD private_stream_1 (`0xBD`) substreams:

| substream | strip beyond PES header | output ext |
|---|---:|---|
| AC-3 (`0x80..=0x87`) | 3 bytes (BD common: num + 16-bit FAP) | `.ac3` |
| DTS (`0x88..=0x8F`) | 3 bytes (same BD common) | `.dts` |
| LPCM (`0xA0..=0xA7`) | 6 bytes (BD common 3 + LPCM 3) | `.wav` |
| Subpicture (`0x20..=0x3F`) | 0 (SPU starts immediately) | (.idx/.sub) |
| Video (`0xE0`) | 0 | `.mpg` |

### Video bytes: strip `user_data_start_code (0x000001B2)`

DVD MPEG-2 video carries NTSC Line-21 closed captions inside MPEG-2
`user_data` blocks. MakeMKV strips those bytes from the video ES (so
the result is "pure" MPEG-2) and emits them separately as an SRT
track (after EIA-608 decoding). To match their `.mpg` bytewise we
MUST do the same strip.

On ANGEL_S1D1 title 1, there are 5805 user_data blocks totaling
~1.26 MB — without stripping, our `.mpg` is +1.26 MB vs MakeMKV's.

`disc-dvd::video_es::UserDataFilter` strips them (passes unit tests)
but has a **known PES-boundary bug** producing ~106-byte zero gaps at
certain video PES seams.

### Subpicture format: VobSub (.idx + .sub), NOT .sup

`.sup` is Blu-ray PGS. DVD subtitles are **VobSub**: a `.sub` file
containing MPEG-PS sectors (each subtitle pack-aligned to 2048 bytes,
private_stream_1 PES wrapping the raw SPU bytes) plus a `.idx` text
file with the YCrCb→RGB palette + per-subtitle `timestamp:`/`filepos:`
index. `disc-dvd::vobsub` emits both.

### Filename convention (MakeMKV's exact mkvextract format)

```
{prefix}_t{NN}_track{N}_[{lang}].mpg                 video
{prefix}_t{NN}_track{N}_[{lang}]_DELAY {ms}ms.ac3    audio (ac3/dts/wav)
{prefix}_t{NN}_track{N}_[{lang}].idx                 VobSub index
{prefix}_t{NN}_track{N}_[{lang}].sub                 VobSub data
{prefix}_t{NN}_track{N}_[{lang}].srt                 closed-caption text
{prefix}_t{NN}_chapters.xml                          MKV chapter XML
```

The `_DELAY {ms}ms` literal is **part of the filename, not a sidecar.**
mkvtoolnix auto-reads it.

### `mkvmerge "invalid data"` warnings on raw audio are NOT authoritative

When you `mkvmerge` a raw `.ac3` from our output, it can flag chunks
(typically ~767 bytes = one AC-3 frame at 192 kbps) at stc_discontinuity
boundaries as "invalid data … skipped." Those bytes match MakeMKV's
elementary stream EXACTLY. Verify against `mkvextract` of MakeMKV's MKV
output if in doubt.

## BD / UHD ground truth

### Seamless-branch AV sync: whole audio frames ARE dropped

Verified 2026-07-31 on a 2-in-1 UHD (theatrical + extended sharing clips).
**Every `AV sync issue … encountered overlapping frame` log line is a
PlayItem junction in the MPLS** — 26 junctions matched 26 log events exact
to the millisecond.

Each clip's audio is independently frame-aligned but the video cut is not on
an audio frame boundary, so the outgoing clip's last audio frame overhangs.
MakeMKV accumulates a running skew; when it reaches one full audio frame it
drops a frame from *every* audio track:

- `10.666 ms` = 512 samples @ 48 kHz = one DTS core frame — the drop quantum
- 26 overlaps summed to 156.657 ms; 14 drops × 10.666 = 149.324 ms
- residual 7.333 ms = exactly the final reported skew

Still unknown from outside and needing decomp: whether the dropped frame is
the last of the outgoing clip or the first of the incoming one, and whether
the accumulator runs in 90 kHz ticks or samples. Anchor via the message
catalog (`msg_catalog_lookup`, `0065a320` on 1.18.4).

MPLS PlayItem layout, for reference: name `b[0:5]`, codec `b[5:9]`,
flags `b[9:11]`, stc_id `b[11]`, IN/OUT `b[12:20]`, times in 45 kHz.

## Library defaults

* `libdvdread`: no logger callback registered. Its `CHECK_VALUE`
  warnings go to stdout/stderr via `fprintf`. To route them through
  `log::warn!`, register a `dvd_logger_cb.pf_log` with `DVDOpen2` —
  outstanding follow-up.
* `libdvdnav`: silently filters NV_PCK / system_header sectors out of
  the block stream — they don't reach the caller as `BLOCK_OK`. For
  ANGEL title 7 this is 31 sectors; on title 1, 5081.
* `libdvdread`'s `ifo_print.c` has a known cosmetic divisor bug
  (`sizeof(c_adt_t)` instead of `CELL_ADDR_SIZE`) in its debug
  printer. We don't link the printer — see `wrapper.h` and the
  comment in `disc-dvd::ifo::cell_adr_table`.

## Current CLI surface

**User commands** (format-agnostic — detect disc type, delegate to the backend):

| command | what it does |
|---|---|
| `info <path>` | dump everything libdvdread tells us about a DVD |
| `list <path>` | the uniform title/track tree the selectors operate on — per-title index, per-track audio/subtitle index, codec, language (`—` = untagged), `[skipped: MinLength]` marks; `--min-length` (default 120) |
| `rip --disc <path> --out-dir <dir>` | **the rip pipeline** — MakeMKV-style per-track output (libdvdnav + faithful handlers: user_data strip, VobSub, delay, chapters). Default = all titles ≥ `--min-length` (120s; `0`=all), all tracks in IFO order; narrow with `--title 0,2` / `--audio eng/0/none` / `--subtitle …` |

`rip` is THE pipeline. Selection is **index-anchored** (`rip --title N` is the
0-based collection index from `list`; language is a convenience layer that
falls back to all-of-kind on no match). Min-length **unselects** (marks
`SkipReason::MinLength`), never removes.

**DVD diagnostics** — `dvd <tool>` (source under `disc-cli/src/dvd/`). NOT the
rip pipeline: isolated single stages + the older manual cell-walk, for
debugging / byte-verification.

| command | what it does |
|---|---|
| `dvd dump-sectors` | read raw sectors from a VOB stream (+ CSS) |
| `dvd scan-streams` | parse a sector stream, report per-stream byte counts |
| `dvd demux-vob` | per-stream split of a `.vob` file (generic `Demuxer`) |
| `dvd dump-title` | manual PGC cell-walk → single `.vob` |
| `dvd demux-title` | per-stream split via the manual cell walk |
| `dvd dump-title-nav` | dump a title via libdvdnav |
| `dvd demux-title-nav` | per-stream split via libdvdnav |

The traversal axis (manual cell-walk vs libdvdnav) and MakeMKV's protection
modes (CellWalk/CellTrim/CellFull) belong as a future `rip` option resolved
**inside `disc-dvd`**, not as separate commands.

## Open follow-ups

1. `video_es::UserDataFilter` PES-boundary bug — ~106-byte zero gaps at
   certain seams. Filter passes its unit tests on the same byte patterns
   in single-call feeds; bug is in the multi-call trailing-3-byte
   holdback interacting with state.
2. VobSub `.sub` size vs MakeMKV's per stream — partly addressed by the
   multi-PES SPU continuation work; re-measure.
3. EIA-608 → SRT decoder not yet written. CC bytes go to `*_cc.bin`
   (raw user_data with start codes intact).
4. libdvdread logger bridge — deferred.
5. Multi-angle handling untested (no multi-angle disc in the corpus).
6. Reachability-traced cellwalk mode (MakeMKV's "third mode") — no ground
   truth yet.
7. End-to-end verification incomplete on Merlin (3hr) and SPACE_SYMPHONY
   (LPCM) — only ANGEL_S1D1 has been compared.
8. The classification sweep over the 4,491-function real universe has not
   been run. `role` is `unknown` for 9,583 of 9,634 rows. Design, tooling
   and a validated prototype are in `analysis/sweep/`.

---

# Part 4 — Where things live

## Research workspace (read-only from this repo's perspective)

| | Path |
|---|---|
| Atlas source-of-truth | `Tests/Disc-Remuxer/atlas/atlas.tsv` |
| Atlas tool (v2) | `Tests/Disc-Remuxer/atlas/atlas.py` |
| Atlas schema (v2) | `Tests/Disc-Remuxer/atlas/schema_v2.md` |
| Encyclopedia articles | `Tests/Disc-Remuxer/atlas/per_function/FUN_<addr>.md` |
| Regenerated views | `Tests/Disc-Remuxer/atlas/views/` (`atlas.py report`) |
| Decomp dumps | `Tests/Disc-Remuxer/decomp/functions/<shard>/FUN_<addr>.md` |
| Call graph | `Tests/Disc-Remuxer/outputs/callgraph_1.18.4.tsv` |
| Trace fired-sets (per disc) | `Tests/Disc-Remuxer/analysis/*.set` |
| Classification sweep tooling | `Tests/Disc-Remuxer/analysis/sweep/` |
| ptrace tracer + scripts | `Tests/Disc-Remuxer/traces/tools/` |
| MakeMKV sources (bin + oss, 1.18.4) | `Tests/Disc-Remuxer/Sources/` |
| Reference tools' sources | `Tests/Disc-Remuxer/Sources/{PgcDemux_1205_src,dgmpgdec2005src}` |
| Reference rips (MKV, for byte-compare) | `Tests/Disc-Remuxer/outputs/makemkv/` |
| Reference rips (mkvextract'd streams) | `/home/chaoz/Desktop/Makemkv/<DISC>/` |
| Our rip outputs | `Tests/Disc-Remuxer/outputs/ours/<disc>/<command>/` |
| **1.18.3-era archive (historical)** | `Tests/Disc-Remuxer/OLD/` |

## Implementation repo (this directory)

| | Path |
|---|---|
| Workspace root | `Cargo.toml` |
| FFI crates | `crates/libdvd{css,read,nav}-sys/` |
| Pure-Rust core | `crates/disc-core/` |
| DVD safe wrappers + ops | `crates/disc-dvd/` |
| CLI binary (`disc-remuxer`) | `crates/disc-cli/` |
| Vendored upstream libraries | `vendor/libdvd{read,css,nav}/` (git submodules) |
| Build output | `target/{debug,release}/disc-remuxer` |
| rpath / linker config | `.cargo/config.toml` |

## Test corpus (host filesystem)

| | Path |
|---|---|
| DVD test discs | `/home/chaoz/Desktop/Makemkv/Dvds for testing/` |
| BD + UHD test discs | `/home/chaoz/Desktop/Makemkv/Discs for testing/` |
| libdvdcss key cache | `~/.dvdcss/<disc-id>/` |

Scratch (rips, traces, mux temp, `TMPDIR`) goes to on-disk `scratch/` under
the research workspace — **never `/tmp`**, which is tmpfs/RAM here.

## Things to never do in a session

- Edit `atlas.tsv` by hand. Use `atlas.py` commands.
- Use v1 vocabulary — buckets `B1`–`B8`, levels `L0`–`L4`, `rebucket`. Gone.
- Import v1 semantic labels (summaries, notes, cone map) into v2. They are
  quarantined evidence, not truth, and some are wrong. Re-derive instead.
- Record a semantic claim at a confidence its evidence doesn't support.
  `unknown` is a valid, useful answer.
- Bulk-retire never-fired functions as dead. Use `corpus-gap`.
- Mention the research source binary by name in source files or commit
  messages.
- "Stub out" a function without recording the gap.
- Add code that solves a problem differently from the proven correct
  behavior. Faithful first.
- Run `atlas.py` with `--by=user` unless explicitly asked. Default is
  `claude_chat`.
- Skip `atlas verify` + `atlas report` at session end when atlas work was
  done.

When a session is unsure whether something is allowed, ask the user before
proceeding. The cost of pausing is small; the cost of corrupting the atlas
or diverging the port is huge.
