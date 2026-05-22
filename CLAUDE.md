# Disc-Remuxer — session rules (READ BEFORE STARTING WORK)

This is a long-running multi-session reverse-engineering port of MakeMKV's
`makemkvcon` 1.18.3 to Python. Source of truth, working artifacts, and
test data live in **`/home/chaoz/Desktop/Makemkv/Tests/Disc-Remuxer/`**.
Port code commits here (`/home/chaoz/Desktop/Programs/Disc-Remuxer/`).
Two directories. Never mix.

The goal is **byte-identical MKV output** vs MakeMKV on supported disc types.
That requires the same control-flow decisions and edge-case coverage as the
original binary. We don't get there by guessing.

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

Write port code in `/home/chaoz/Desktop/Programs/Disc-Remuxer/disc_remuxer/...`.

The port code **must** cite the decomp anchor in a comment at the function
top:

```python
def parse_vts_ifo(buf: bytes) -> VtsIfo:
    """Port of MakeMKV FUN_007abc12 (mmgpl/dvdread/ifo_read.c:1029–1158).

    Walks the VTS_PTT_SRPT table, validates the magic at offset 0,
    extracts per-title sector pointers ...
    """
```

The string `FUN_<addr>` must appear in the file. `atlas.py implement` won't
let you promote to L3 without it. Then:

```bash
atlas.py implement <addr> \
    --path=disc_remuxer/dvd/ifo.py \
    --function=parse_vts_ifo \
    --kind=semantic_port \
    --name=parse_vts_ifo \
    --oss=mmgpl/dvdread/ifo_read.c:1029-1158
```

`--kind` choices:
- `exact_port` — control flow matches the decomp line-by-line (used for
  protection / cell-walk / where divergence breaks byte-identity)
- `semantic_port` — matches OSS source (mmgpl / libmakemkv / libmmbd) with
  MakeMKV's customizations layered on
- `mechanical_port` — boilerplate that came along with an owning class port
- `hardcoded` — used for deobfuscation stubs (we bake in the cleartext output)
- `skipped` — out-of-scope or genuinely dead code

## Step 5 — verifying

Every L3 implementation needs evidence it works:

```bash
atlas.py test <addr> --status=unit
atlas.py test <addr> --status=byte_compare_pass
```

`byte_compare_pass` is the gold standard — extract ES + EBML-tree-diff
against the captured MakeMKV reference rips in
`Tests/Disc-Remuxer/outputs/makemkv/`, excluding the 24 per-rip random bytes
(SegmentUID + DateUTC + the cascading CRC-32s — see
`atlas/seeds/per_rip_random.md` or the `project_random_ebml_fields` memory).

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
2. **Every port commit cites a `FUN_<addr>` anchor** OR is tagged
   `[infra]` / `[tooling]` / `[test]` / `[binding]` in the message body.
3. **No invented functions.** If you can't find a decomp anchor and no OSS
   analogue exists, *stop and trace* (`Tests/Disc-Remuxer/traces/tools/ptrace_tracer`)
   or *read more decomp*. Never write a function that "feels right."
4. **No defensive code that isn't in MakeMKV.** Faithful first.
5. **Byte-compare is the only success metric.** Test suites that count MSGs
   or check metadata fields can pass while bytes diverge.
6. **One rip pipeline.** No ffmpeg-pipe fallback, no "we'll retire this
   later" parallel path. If you can't make it work, mark the row blocked
   and surface the issue.
7. **If decomp is ambiguous** → `traces/tools/ptrace_tracer` on the relevant
   disc, OR read more decomp from callers/callees. Never guess.

## Where things live

| | Path |
|---|---|
| Atlas source-of-truth | `Tests/Disc-Remuxer/atlas/atlas.tsv` |
| Atlas tool | `Tests/Disc-Remuxer/atlas/atlas.py` |
| Atlas schema | `Tests/Disc-Remuxer/atlas/schema.md` |
| Per-function deep-exam notes | `Tests/Disc-Remuxer/atlas/per_function/FUN_<addr>.md` |
| Per-tier checklist | `Tests/Disc-Remuxer/atlas/tiers/B*.md` |
| Rebucket audit log | `Tests/Disc-Remuxer/atlas/rebucket_log.tsv` |
| Master trace + ambiguities doc | `Tests/Disc-Remuxer/TRACE_INDEX.md` |
| Decomp dumps (read-only) | `Tests/Disc-Remuxer/decomp/functions/<shard>/FUN_<addr>.md` |
| ptrace tracer + scripts | `Tests/Disc-Remuxer/traces/tools/` |
| MakeMKV reference rips (for byte-compare) | `Tests/Disc-Remuxer/outputs/makemkv/` |
| Port code (this repo) | `disc_remuxer/...` (to be created) |

## Things to **never** do in a session

- Edit `atlas.tsv` by hand. Use `atlas.py` commands.
- Mark a function `implemented` without the impl file containing the decomp anchor.
- "Stub out" a function temporarily without recording the gap as `blockers`.
- Write a Python helper that solves a problem MakeMKV does differently.
- Rebucket a function before it's at L≥2. `atlas.py rebucket` blocks
  this — don't try to work around it. The per-function note is the
  evidence that justifies the move.
- Rebucket based on the v1 tag alone. Read the decomp / strings /
  callers and let the note's findings drive the decision.
- Run `atlas.py` with `--by=user` or any other identity unless explicitly
  asked. Default is `claude_chat`.
- Skip `atlas verify` and `atlas report` at session end.

When a session is unsure whether something is allowed, ask the user before
proceeding. The cost of pausing is small; the cost of corrupting the atlas
or diverging the port is huge.
