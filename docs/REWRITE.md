# PLAN v2 — the rewrite: a real demuxer of our own, FFmpeg as an instrument

Written 2026-10-13, after the untangle (UNTANGLE.md) and the decisions so far:
demuxing lives inside the app; FFmpeg stays (full build, untrimmed is fine)
but stops being the processing layer; no check or feature is lost; the current
program's corpus output is the oracle for every step.

## 1. THE LINE (the rule that untangles everything)

> **Demuxing = every byte transformation from disc sector to output file.
> That path is ours, end to end. No byte that reaches an output file ever
> passes through FFmpeg.**
>
> **FFmpeg = an analysis instrument. We hand it COPIES of data and it answers
> questions ("what does this decode to?"). If FFmpeg vanished, every rip would
> still be byte-identical — only reports and checks would degrade.**

Consequence: FFmpeg can never corrupt a rip, time a frame, or decide a join.
It also can never blur the logs again: it is called in exactly four places,
all inside one module, with one log hook.

### Where the line cuts today's code

| Today's use | Side of the line | In the rewrite |
|---|---|---|
| mpeg.c / mpegts.c disc modes split packs/PES | data path | ours: `mux/` (the cores are already pure functions) |
| `av_parser_parse2` + disc-mode frame rules cut frames | data path | ours: `parse/` (the rules are already ours; the host gets simpler) |
| `eac3_core` / `dca_core` bitstream filters make the extracted-core TRACKS | **data path** (their output is written to files!) | ours: keep-only-core frame filters in `parse/` — trivial once frames are cut |
| LPCM header stripping / frame sizing | data path | ours (already is: `disctl_lpcm`) |
| Timeline, joins, junction, chapters, verify | data path | ours (already is: `timeline/`) |
| Writers (ES, WAVE, VobSub, SUP, SRT, chapters) | data path | ours (already is) |
| Details probe decodes one frame per track (HDR, DoVi, layout, profile...) | analysis | `analyze/` calls FFmpeg decoders on a sample copy |
| End check decodes written files back and compares | analysis | `analyze/` (same decoders both sides) |
| TrueHD seamless search decodes AU runs + mono mix to compare joins | analysis (it only *chooses* a join; bytes written are disc bytes) | `analyze/` (decoder + swresample mix, same numbers) |
| TrueHD stream-end sample count (`decode_count`) | analysis | `analyze/`; directory-math fallback stays first |
| LATM one-frame decode for rate/samples | analysis | `analyze/` (or later an own StreamMuxConfig parser) |
| Channel-layout discovery for file names | analysis | from the details probe |
| CCExtractor converts captions to text | analysis-style helper process | unchanged — it is the model: a tool beside the path |

## 2. The pipeline (the rip path, all ours)

```
source (image / folder)
  │
  ▼
io/        readers: block reads with retries, ISO 9660 / UDF x2 / folder,
           fs choice rule, disc format detection          [moved: discio_*]
  │
  ▼
format/    DVD | HD DVD | Blu-ray part: structure, checks, titles, tracks,
dvd/hddvd/ chapters, keys, "which block next" per segment; decryption via
bluray/    libdvdcss / libaacs                            [moved: dvd_*/hddvd_*/bluray_* + DVD policy
  │                                                        lifted from libdvdread; scan driver from libdvdnav]
  ▼
mux/       PS pack/sector splitter  |  TS per-PID PES reassembler
           → PES payloads with their PTS                  [extracted: mpeg.c / mpegts.c disc modes]
  │
  ▼
parse/     one frame cutter per codec behind one push API
           (bytes in → units out + why-skipped), picture headers, core-only
           filters (E-AC-3→AC-3, DTS-HD→DTS)              [extracted: the 14 parsers' disc rules;
  │                                                        + copied NAL/SPS/slice + header readers]
  ▼
timeline/  the title timeline: frame step & time pairing, video grid, audio
           durations, joiner, junction, PCM strategy, SPU/PGS/TextST, CC,
           chapters, verify (stage 5: re-parse every written frame, CRCs,
           order, durations)                              [moved: disctl_*]
  │
  ▼
writer/    ES files, WAVE, VobSub, SUP, SRT, chapter XML; later MKV
                                                          [moved: writer.c]
```

Beside the path, called with data copies:

```
analyze/   the ONE module that links FFmpeg (full build, untrimmed, vendored
           as-is and ~upstream-clean once our edits move out):
             a) details probe   (sample buffer → stream details incl. HDR,
                                 Dolby Vision, layouts, profile/level)
             b) end check       (disc-side sample + written file → compared
                                 details, same decoder both sides)
             c) seamless helper (TrueHD AU runs → PCM → mono mix → compare)
             d) small answers   (LATM config, MLP end-AU sample count)
           One av_log hook here, feeding our logger with a source tag.
           Absent/failed decode → "not checked", said plainly, never fatal.
```

And the info path (scan): format part reads the structure (with every IFO /
playlist check and the findings book) → title list (policy) → stream pass
(`mux/` + `parse/` over the largest segment, duplicates out, details via
`analyze/`a) → the disc model → GUI/CLI.

## 3. No feature lost — where each check lives

| Feature / check today | In the rewrite |
|---|---|
| Read retries, padding, failure accounting per attempt | `io/` (moves unchanged) |
| File-system choice rule, UDF compatibility switches, loop guards | `io/` (moves unchanged) |
| IFO/BUP redundant reads, raw table checks with byte-offset reports | `dvd/` (lifted `_rec` readers) |
| Damage detection + coalesced damage ranges, per-sector validity | `dvd/` (lifted vob reader) + `DiscTitleSource.damage` contract unchanged |
| CSS key crack + verification heuristics | `dvd/` + libdvdcss (unchanged) |
| Navigation scan (plays like a viewer, every button) | libdvdnav (vendored, VM patch set) + `dvd/` scan driver |
| Cell modes, decoy PGCs, fake/duplicate title rules | `dvd/` (policy, lifted) |
| AACS (BD/UHD/HD DVD), content hash check, key files | libaacs (unchanged) + format parts |
| Blu-ray decoy playlists, menu names without Java, stream-table repair | `bluray/` (moves unchanged) |
| Track details: codec, profile, HDR10/+/HLG, Dolby Vision MEL/FEL, layouts | `analyze/`a (same decoders → same answers) |
| Stream pass: duplicates, empty audio, unusable titles | `disc/` stream pass over `mux/`+`parse/` (moves) |
| Frame-level verify of output (CRCs, order, durations, placeholders) | `timeline/` verify — already decode-free |
| End check of written files (~25 fields vs disc side) | `analyze/`b (identical) |
| Seamless TrueHD joins (sample-exact) | `timeline/` junction + `analyze/`c (identical numbers) |
| Captions via CCExtractor | unchanged helper process |
| Findings book: dedup, caps, summary; one log, library lines attributed | `base/` log — now one system; libraries' callbacks and `analyze/`'s hook feed it |
| Numbered messages, JSON records, log files per disc | `base/` log (numbering scheme kept; JSON becomes a rendering of typed events) |
| Settings registry + TOML + GUI prefs from it | `base/` settings (moves) |
| GUI queue/details/progress/stop, selection kept per title | `gui/` on engine API v2 (typed events + disc model; loses its JSON re-parsing and text reverse-parsing) |

## 4. What gets copied from FFmpeg into `parse/` (bounded, one-time)

- H.264/HEVC NAL/RBSP unpack + SPS/PPS/slice-header readers (~1–2 k lines)
- AC-3/E-AC-3, DTS core, MPEG-audio header math (partly restated already)
- Language table, CRC tables
- Each copy noted with origin + license in the ledger (program is GPL-2.0-only
  already; nothing changes)

Decoders are NOT copied — they stay in `analyze/` behind the line.

## 5. Order of work (each step gated: tests green + corpus rips byte-identical
to the frozen baseline, logs diffed)

1. **Freeze the oracle**: full corpus ripped + info JSON + logs with the
   current program, stored read-only.
2. **Skeleton + base/**: build, pytest+cffi bridge, log/messages/settings/
   buffers/events. Fresh, small.
3. **io/ moves** (shims only). Proves the method.
4. **mux/ + parse/ extracted** from the disc modes; their unit tests ported;
   the splitter host replaces the AVCodecParser contract.
5. **timeline/ moves**; frame step re-hosted on `parse/`; core-only filters in.
6. **analyze/ built**: the four entry points over vendored FFmpeg (untrimmed),
   one log hook. `libs/ffmpeg` sheds our edits (they now live in mux/parse) —
   back to ~upstream.
7. **disc/ + hddvd/ + bluray/ move** (utility-coupled only — near-verbatim);
   stream pass on mux/parse/analyze.
8. **dvd/ lift**: title list, stream probe, vob-reader policy, scan driver up
   into the app; libdvdread slims toward upstream; libdvdnav keeps the VM
   patch set.
9. **engine API v2 + writer + end check**: typed events and models; CLI as a
   formatter; GUI consumes structs.
10. **MKV writer** (after every format's streams pass the gates), then retire
    the old tree as the archive.

## 6. Still open (user)

- D3: fresh repo vs the new tree beside the old in this repo until step 10.
- D4: MKV writer: libebml/libmatroska vs own EBML writer (decide at step 10).
- D5: `analyze/` behavior when decode disagrees with parse-level details —
  today's precedence kept (decoder wins for decoded truths); revisit per case.
