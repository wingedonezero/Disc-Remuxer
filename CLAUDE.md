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
commit messages.

## Step 1 — orient yourself

Before writing anything, run:

```bash
/home/chaoz/Desktop/Makemkv/Tests/.venv/bin/python \
    /home/chaoz/Desktop/Makemkv/Tests/Disc-Remuxer/atlas/atlas.py status
```

Read `Tests/Disc-Remuxer/atlas/status.md` and `Tests/Disc-Remuxer/TRACE_INDEX.md`.
The atlas tracks **every one of 9,628 functions** in `makemkvcon` with a
strict per-function progression. Don't take any action that bypasses it.

## Step 2 — pick work from the right tier

Tier order is **topological** and must not be jumped:

```
B1_foundation → B2_dvd / B3_bd / B4_uhd → B5_protection → B6_cli_tools
```

Within a tier:

```bash
atlas.py next --bucket=B1_foundation
```

picks the highest-priority unfinished item (lowest analysis_level, largest
size first within that level).

## Step 3 — examine before implementing

A function at L0 or L1 cannot be ported. It must first be **deep-examined**:

1. Read the full decomp: `Tests/Disc-Remuxer/decomp/functions/<shard>/FUN_<addr>.md`
2. Walk callers and callees one level out.
3. Check `Tests/Disc-Remuxer/analysis/oss_matches.tsv` for an OSS source anchor.
   If matched, read that source range. If not matched, search
   `makemkv-oss-1.18.3/{libmakemkv,mmgpl,libmmbd,libdriveio}/` for functions
   with similar string refs or call patterns.
4. Capture findings to `Tests/Disc-Remuxer/atlas/per_function/FUN_<addr>.md`
   with this structure:
   - **Address / size / signature**
   - **What it does** (one paragraph)
   - **Control-flow shape** (key branches, loops, dispatch tables, decoded magic numbers)
   - **Callers** (who triggers it, when in the pipeline)
   - **Callees** (what helpers it relies on — each must already be at L≥2)
   - **State touched** (struct offsets read/written, global addresses, decoded constants)
   - **OSS anchor** (path:lines or `none — internal to makemkvcon`)
   - **Edge cases handled** (every conditional branch's purpose)
   - **Trace evidence** (which captured traces fire it, with sample arg values if any)
5. Promote to L2:
   ```bash
   atlas.py examine <addr> --notes=atlas/per_function/FUN_<addr>.md
   ```

If the function calls into helpers that are still at L<2, **examine them
first**. Topological order applies inside a function's callee tree too.

## Step 3.5 — rebucket if the v1 categorizer was wrong

The v1 categorizer used heuristics on string content and call patterns,
so some functions ended up in the wrong bucket. **Once a function is
L2** (and only once it is L2), you may move it to the correct bucket
with explicit reasoning. **L0/L1 rows can never be rebucketed** — the
per-function note is the evidence that justifies any move.

### Inline at examine-time (preferred)

If the deep-exam note already concludes the row belongs in a different
bucket, add `--rebucket=BX` to the same `examine` call:

```bash
atlas.py examine 007e8260 --notes=atlas/per_function/FUN_007e8260.md \
    --rebucket=B2_dvd
```

This is one atomic operation: L0→L2 + bucket change + audit-log entry.

### Standalone for already-L2 rows

For rows that became L2 in earlier sessions, use the bare command:

```bash
atlas.py rebucket 007a4af0 --bucket=B5_protection \
    --reason="drive command-sequence sender (SDF/CSS class); per_function note"
```

`--reason` is required and goes into `atlas/rebucket_log.tsv` — one
short sentence ("DVD-only callers", "AACS key loader", etc.).

### When to rebucket

A note has earned a rebucket if it identifies any of:

- **All non-trivial callers are in one specific bucket** → move there
  (e.g. a "helper/alloc" with 8 callers all in `0x7exxx` DVD range → B2_dvd).
- **The string content nails a specific bucket** (e.g. `kvh.bin`,
  `OSSL_AES_*`, `MakeMKV v1.18.3 ...` → B5_protection, B5_protection,
  cli output).
- **The codec/byte-stream is bucket-specific** (e.g. Dolby Vision RPU
  → B4_uhd, BD+ SVQ → B5_protection).
- **The function turns out to be cross-bucket foundation** even though
  v1 tagged it specifically (e.g. central codec dispatchers) → move
  to B1_foundation.

The v1 tag is a starting point, not gospel. Use the L2 evidence.

### Don't rebucket on speculation

If the note says "possibly B5_protection (need to confirm)" or
"STAYS B1 or B6_cli_tools", **defer**. Hedged notes don't justify
moves. Either dig deeper or leave it.

## Step 4 — implementing

The Rust workspace lives at this repo's root. Crate layout:

```
crates/
├── libdvdcss-sys/    FFI bindings — vendored libdvdcss build via meson
├── libdvdread-sys/   FFI bindings — vendored libdvdread (linked to libdvdcss)
├── libdvdnav-sys/    FFI bindings — vendored libdvdnav
├── disc-core/        pure-Rust traits + DiscError + DiscType + check::*
├── disc-dvd/         safe wrappers over libdvdread + libdvdcss + libdvdnav
└── disc-cli/         the `disc-remuxer` binary (clap, flexi_logger, sha2)
vendor/
├── libdvdread/       git submodule, pinned tag
├── libdvdcss/        git submodule, pinned tag
└── libdvdnav/        git submodule, pinned tag
```

Add new code to the appropriate crate. Build with `cargo build`. The CLI
binary lives at `target/release/disc-remuxer`.

### Coding conventions

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
4. **Every invariant we know about gets a check.** If MakeMKV-via-the-atlas
   tells us "this VOB sector should start with 00 00 01 BA on a cleartext
   disc," that becomes a `check!` in our code, logged PASS/FAIL with the
   actual bytes shown. Transparency at every step — not after-the-fact
   debugging.
5. **Errors propagate via `disc_core::DiscError`** for library code,
   `anyhow::Result` for the CLI. Each error variant carries enough context
   (path, offset, count, etc.) to debug from the log alone.
6. **Job-log support** is built in via `flexi_logger`. CLI subcommands
   that produce output accept `--log-file <path>` (global flag) to mirror
   the structured log into a file with timestamps. Wire this in for any
   new "rip-like" subcommand so the operation is self-documenting.

### Recording an implementation in the atlas

When a Rust function implements behavior from a specific `FUN_<addr>`
that's been L2-deep-examined in the atlas, record the mapping there:

```bash
/home/chaoz/Desktop/Makemkv/Tests/.venv/bin/python \
    /home/chaoz/Desktop/Makemkv/Tests/Disc-Remuxer/atlas/atlas.py \
    implement <addr> \
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

The atlas mapping is research-internal. The code itself stays
implementation-name-only (no `FUN_<addr>` strings in source).

## Step 5 — verifying

Every L3 implementation needs evidence it works:

```bash
atlas.py test <addr> --status=unit
atlas.py test <addr> --status=byte_compare_pass
```

`byte_compare_pass` is the gold standard — `cargo build --release && ./target/release/disc-remuxer extract …`
output should match the captured reference rips in
`Tests/Disc-Remuxer/outputs/makemkv/` byte-for-byte, excluding the 24
per-rip random bytes (SegmentUID + DateUTC + the cascading CRC-32s — see
`atlas/seeds/per_rip_random.md` or the `project_random_ebml_fields` memory).

Smaller verification gates between L3 and full byte-compare:
- `cargo test` — unit + integration tests must pass clean
- `disc-remuxer dump-sectors … && sha256sum` — for the sector-read layer,
  hash must match `dd` of the same range on an unscrambled corpus disc

## Step 6 — keep the atlas honest

Before ending a session:

```bash
atlas.py verify       # must exit 0
atlas.py report       # regenerate status.md and tier checklists
```

`atlas verify` blocks if any row claims L≥3 without the cited `impl_path`
existing or without the decomp anchor appearing in that file. **Never bypass
it.** If verify fails, fix the row or the code — don't paper over.

## Hard rules — non-negotiable

1. **No skipping levels.** A row goes L0 → L1 → L2 → L3 → L4. Sessions that
   mark L3 without writing L2 notes are corrupting the atlas.
2. **Clean-room separation.** Code and commits in this repo cite *public
   specs and library APIs only*. The research source binary's name and
   `FUN_<addr>` identifiers do **not** appear in source files or commit
   messages. The atlas's `impl_path`/`impl_function` columns track the
   research-to-implementation mapping privately.
3. **Commits are tagged.** Every commit is `[infra]` / `[tooling]` /
   `[test]` / `[binding]` in the subject. (`[port]` and `FUN_<addr>` in
   subject lines are the old convention from the Python-port plan; do
   not use them.)
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
implementation (`5b/6.5`) of FAP-resync dropped 7 bytes per stream and
was wrong — reverted in commit `ca1256f`.

The relevant FFmpeg patch the user maintains
(`/home/chaoz/Desktop/Programs/FFmpeg` HEAD,
`avformat/dvdvideo: fix AC3 frame loss at PTM discontinuity boundaries`)
fixes a DIFFERENT bug — wrongly DISCARDING valid frames whose PTS
equalled `prev_pts` after a PTM reset. The fix resets the duplicate-
tracking state at the discontinuity. Same principle: byte stream
stays intact, time base resets.

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

Current `disc-dvd::video_es::UserDataFilter` strips them (passes unit
tests) but has a **known PES-boundary bug** producing ~106-byte zero
gaps at certain video PES seams. Resulting `.mpg` is still ~727 KB
larger than MakeMKV's and not yet byte-identical.

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
mkvtoolnix auto-reads it. Delay is measured per audio track as
`first_audio_pts - first_video_pts` (90 kHz ticks / 90 = ms). Our
`disc-dvd::chapters` writes the XML; `disc-cli::rip_title` is the
user-facing command that produces all of the above.

### `mkvmerge "invalid data"` warnings on raw audio are NOT authoritative

When you `mkvmerge` a raw `.ac3` from our output, it can flag chunks
(typically ~767 bytes = one AC-3 frame at 192 kbps) at stc_discontinuity
boundaries as "invalid data … skipped." Those bytes match MakeMKV's
elementary stream EXACTLY. The MKV container layer expects PTS
metadata to handle the discontinuity; raw `.ac3` lacks that. Treat the
warnings as expected; verify against `mkvextract` of MakeMKV's MKV
output if in doubt.

### Library defaults

* `libdvdread`: no logger callback registered. Its `CHECK_VALUE`
  warnings go to stdout/stderr via `fprintf` (e.g. the
  `libdvdread: Couldn't find device name.` messages on directory
  rips). To route them through `log::warn!`, register a
  `dvd_logger_cb.pf_log` with `DVDOpen2` — outstanding follow-up.
* `libdvdnav`: silently filters NV_PCK / system_header sectors out of
  the block stream — they don't reach the caller as `BLOCK_OK`. For
  ANGEL title 7 this is 31 sectors; on title 1, 5081. We don't need
  them either, but the math affects "bytes emitted by libdvdnav" vs
  "sectors in the cells we ripped manually."
* `libdvdread`'s `ifo_print.c` has a known cosmetic divisor bug
  (`sizeof(c_adt_t)` instead of `CELL_ADDR_SIZE`) in its debug
  printer. We don't link the printer — see `wrapper.h` and the
  comment in `disc-dvd::ifo::cell_adr_table`.

### Current CLI surface

| command | what it does |
|---|---|
| `info <path>` | dump everything libdvdread tells us about a DVD |
| `dump-sectors` | read raw sectors from a VOB stream |
| `dump-title` | walk PGC cells manually, write a single `.vob` |
| `dump-title-nav` | same output via libdvdnav (no NV_PCK / sys_header sectors) |
| `demux-vob` | per-stream split of a `.vob` file (no IFO context) |
| `demux-title` | per-stream split driven by the manual cell walk |
| `demux-title-nav` | per-stream split driven by libdvdnav |
| `scan-streams` | parse a sector stream and report per-stream byte counts |
| `rip-title` | **MakeMKV-style per-track output**: language tags, delay value, VobSub subs, chapters XML, CC sidecar |

`rip-title` is the user-facing command. Everything else is plumbing /
testing.

### Open follow-ups at end of 2026-05-22 session

1. `video_es::UserDataFilter` PES-boundary bug — produces ~106-byte
   zero gaps at certain seams. Filter passes its unit tests on the
   same byte patterns in single-call feeds; bug is in the multi-call
   trailing-3-byte holdback interacting with state.
2. VobSub `.sub` size is ~30% smaller than MakeMKV's per stream —
   likely missing multi-PES SPU collection (we treat every PES with
   PTS as a complete SPU).
3. EIA-608 → SRT decoder not yet written. CC bytes go to
   `*_cc.bin` (raw user_data with start codes intact).
4. libdvdread logger bridge — defer to follow-up.
5. Multi-angle handling untested (no multi-angle disc in the corpus).
6. Reachability-traced cellwalk mode (the "third mode" of MakeMKV
   speculatively — atlas hasn't deep-examined FUN_00708050 so we
   don't have ground truth).
7. End-to-end verification still incomplete on Merlin (3hr) and
   SPACE_SYMPHONY (LPCM) — only ANGEL_S1D1 has been compared.
   Audit at `Tests/Disc-Remuxer/outputs/ours/AUDIT_2026-05-22.md`.

## Where things live

### Research workspace (read-only from this repo's perspective)

| | Path |
|---|---|
| Atlas source-of-truth | `Tests/Disc-Remuxer/atlas/atlas.tsv` |
| Atlas tool | `Tests/Disc-Remuxer/atlas/atlas.py` |
| Atlas schema | `Tests/Disc-Remuxer/atlas/schema.md` |
| Per-function deep-exam notes | `Tests/Disc-Remuxer/atlas/per_function/FUN_<addr>.md` |
| Per-tier checklist | `Tests/Disc-Remuxer/atlas/tiers/B*.md` |
| Rebucket audit log | `Tests/Disc-Remuxer/atlas/rebucket_log.tsv` |
| Master trace + ambiguities doc | `Tests/Disc-Remuxer/TRACE_INDEX.md` |
| Decomp dumps | `Tests/Disc-Remuxer/decomp/functions/<shard>/FUN_<addr>.md` |
| ptrace tracer + scripts | `Tests/Disc-Remuxer/traces/tools/` |
| Reference rips (MKV form, for byte-compare) | `Tests/Disc-Remuxer/outputs/makemkv/` |
| Reference rips (mkvextract'd streams) | `/home/chaoz/Desktop/Makemkv/<DISC>/` — ANGEL_S1D1, SPACE_SYMPHONY_MAETEL_1.iso_001, Merlin 1998 R1 SE |
| Our rip outputs (for inspection / SHA-compare) | `Tests/Disc-Remuxer/outputs/ours/<disc>/<command>/` |

### Implementation repo (this directory)

| | Path |
|---|---|
| Workspace root | `Cargo.toml` |
| FFI crates (cargo-built C libs + bindgen) | `crates/libdvd{css,read,nav}-sys/` |
| Pure-Rust core (traits, errors, check::*) | `crates/disc-core/` |
| DVD safe wrappers | `crates/disc-dvd/` |
| CLI binary (`disc-remuxer`) | `crates/disc-cli/` |
| Vendored upstream libraries | `vendor/libdvd{read,css,nav}/` (git submodules) |
| Build output | `target/{debug,release}/disc-remuxer` |
| rpath / linker config | `.cargo/config.toml` |

### Test corpus (host filesystem)

| | Path |
|---|---|
| DVD test discs (directories + ISOs) | `/home/chaoz/Desktop/Makemkv/Dvds for testing/` |
| BD + UHD test discs | `/home/chaoz/Desktop/Makemkv/Discs for testing/` |
| libdvdcss key cache | `~/.dvdcss/<disc-id>/` |

## Things to **never** do in a session

- Edit `atlas.tsv` by hand. Use `atlas.py` commands.
- Mention the research source binary by name in source files or commit
  messages. The atlas tracks research-to-impl mappings privately via
  `impl_path`/`impl_function`; nothing leaks into this repo.
- Use the old `Port of <SourceBinary> FUN_<addr>` docstring pattern. That
  was the Python-port plan. Current code uses spec-citation comments only.
- "Stub out" a function temporarily without recording the gap as `blockers`.
- Add code that solves a problem differently from the proven correct
  behavior. Faithful first, then improve later if needed.
- Rebucket a function before it's at L≥2. `atlas.py rebucket` blocks
  this — don't try to work around it. The per-function note is the
  evidence that justifies the move.
- Rebucket based on the v1 tag alone. Read the decomp / strings /
  callers and let the note's findings drive the decision.
- Run `atlas.py` with `--by=user` or any other identity unless explicitly
  asked. Default is `claude_chat`.
- Skip `atlas verify` and `atlas report` at session end when atlas work
  was done this session. (Pure implementation-side sessions that don't
  touch the atlas don't need this.)

When a session is unsure whether something is allowed, ask the user before
proceeding. The cost of pausing is small; the cost of corrupting the atlas
or diverging the port is huge.
