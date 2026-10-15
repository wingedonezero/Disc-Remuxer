# The title timeline

`src/timeline/` (`disctl.h`, `disctl_*.c`): one module for every disc
format (DVD, HD DVD, Blu-ray / UHD). It turns a title's segments into
timed, joined, checked frames per track. The outputs (elementary streams,
later Matroska) only write what it gives them. Where it sits in the
program: `docs/ARCHITECTURE.md`.

## 1. Where it sits

```
title reader (src/disc/title_reader.c) with the format part (src/disc/dvd,
hddvd, bluray): segments (cell group / EVOB clip / playlist clip), tracks,
the clip demuxer per segment (src/mux: MPEG-PS packs / MPEG-TS PES)
        |  payload records: track, segment, bytes, PES PTS (or none), byte position
        v
title timeline (src/timeline/disctl_*.c)
  1 units      cut each track into units (src/parse rules), timestamp records
  2 timing     per track: video grid / audio durations / sub-picture snap
  3 joiner     segments -> one timeline per track
  4 junction   per output track: title start, copies at joins, audio skew
               (absorb / drop), PCM gaps
  5 checks     measure + verify, events
        |  frames: data, time, duration, flags (key, discardable, chapter mark)
        v
dr_disc_read() -> the writer (stream files; Matroska later)
```

The format part keeps everything format-specific: the disc's structure,
the title's segments, the track list, decryption and "the next block of
this segment". The clip demuxer (`src/mux`) hands the timeline raw PES
payloads with their PES times; nothing on the way cuts frames or fills in
or corrects timestamps (LPCM keeps its audio frame header, which the
timeline reads).

`disctl_title.c` puts the stages together for one title: the title reader calls
`ff_disctl_title_segment()` at each segment, `ff_disctl_title_payload()`
for each payload (track, bytes, PES time) and `ff_disctl_title_finish()` at the end, and
takes the output frames with `ff_disctl_title_frame()`. Per segment each
track has a frame step (its codec's rule) and its timing stage; the joiner's frames go to the
junction (or the PCM strategy), every output frame to the checks. Audio
frames wait until the video has been handed on past their time (the
junction compares them with the video). Events of the video and
sub-picture stages wait until the joiner has placed their segment and then
move onto the title timeline; every event is one debug line (the first 50
of each kind and track; the rest are counted).

The frames `dr_disc_read()` gives are the timeline's: final time,
duration and flags, already joined; nothing after it re-times them.

## 2. Time

One internal unit: 1/1,080,000,000 s (27 MHz x 40). It is exact for 90 kHz
PES times (x 12,000), for video frame and field durations (1001/30000 s =
36,036,000; 1001/24000 s = 45,045,000) and for 48 / 96 / 192 kHz samples.
Durations are `samples * 1,080,000,000 / rate` in unsigned 64-bit integers
(truncated), always computed the same way so results are reproducible.

PES PTS wrap: the reference time of a source is the first PTS of the master
track within its first 2000 sectors (a packet that starts a sector) minus
300 s; a PTS below it gets +2^33. Packets before the reference is found
wait for it; none in those sectors fails the title.

## 3. Stage 1: frames and timestamp records (`disctl_frames.c`)

- Each payload is one record {byte position, length, PTS}. The payload goes
  to the track's frame cutter: its codec's rule in `src/parse/` run by the
  one splitter engine (`parse/split.h`); MPEG-1/2 video and E-AC-3, which
  have no disc rule, go to the stock cutters (`parse/stock.c`: FFmpeg's
  parsers' cut, ported). The cutter finds the frames
  ("units").
- A unit gets a record's PTS when it is the FIRST unit that starts inside
  that record's bytes. Every later unit of the same payload has no
  timestamp. A record that ends before the next unit starts is dropped.
- A rule says for each chunk whether it is a frame or bytes that are no
  frame (left out); the disc reader's rules (TrueHD / MLP, sub-pictures,
  VC-1, LPCM, H.264 / HEVC pictures, ...) live in the rules. VC-1:
  a unit is one frame with the headers before it and the user data after it
  (frame / field user data stays with its frame); an end of sequence stays only after a second field; an
  end of sequence without one, and headers not followed by a frame, are units
  without a frame, left out; a slice, a field or user data where none can be
  stops the track.

## 4. The codec table

Every track's codec has one entry. A missing entry = the track is refused,
logged with the reason; nothing is guessed.

| entry | meaning |
|---|---|
| family | `timed` (duration and sync rule below) or `stream` (duration from the parser, never a sync unit) |
| split | the codec's rule in `src/parse/` (none: the stock cutters, `parse/stock.c` — MPEG-1/2 video, E-AC-3) |
| duration | rule for each unit (e.g. AC-3: samples / rate from the stream's first header; TrueHD: per major-sync access unit) |
| sync unit | whether a unit may start a drop / join (AC-3, E-AC-3, DTS: every frame; TrueHD: major-sync units only) |
| filter | the core filter each unit goes through (`dr_eac3_core`: an AC-3 track taken out of E-AC-3; `dr_dca_core`: a DTS track taken out of a DTS-HD stream) |
| video only | units per frame (2 = fields), frame-rate source, display-order number and its wrap period |
| sub-picture only | units found by the DVD sub-picture rule (unit size from its header); duration from the display-stop command; snapped to the video grid |

## 5. The format profile

| entry | HD DVD | DVD | Blu-ray / UHD |
|---|---|---|---|
| packet source | MPEG-PS (`mux/ps_disc.c`) | MPEG-PS | MPEG-TS (`mux/ts_disc.c`) |
| LPCM extra header | 5 bytes | 3 bytes | own format |
| chapters | time marks | byte positions -> first picture | time marks |
| leading / trailing segment probe | yes (large clips kept) | yes | yes |
| DVD LPCM dynamic-range check | - | yes | - |
| two-layer video pairing | - | - | yes |

## 6. Stage 2: timing per track (inside one segment)

- **Video:** every frame time is a grid point: `base + position * frame
  duration`. Positions are sums of field durations in display order (from
  the order numbers). The base is voted from the first 1000 pictures (PES
  time - position). PES times then only check the grid: a picture off by
  >= 0.1 ms is reported; when most pictures in a 25 s window are off, the
  grid is repaired (a forward jump of up to 600 s is followed with empty
  placeholder frames; a small backward drift is corrected one unit at a
  time). Non-picture units are not part of the video track.
- **Audio (timed family):** the unit with a timestamp keeps it; others follow
  by duration. A zero-size unit becomes an empty marker that keeps its time.
- **Audio (stream family):** as above.
- **Side units:** a codec rule may take bytes out of a unit before the
  video sees it. MPEG-1 / 2: user data right after a GOP header (where
  DVD-Video carries its line-21 captions) leaves the video; user data after
  a sequence or picture header stays. Each piece is handed on with the time
  of the next picture after its unit in the same batch (in an open GOP the
  first B picture, the GOP's first picture on screen), else of the batch's
  last picture (a closed GOP: the I picture), lasting one field.
- **Closed captions** (DVD-Video): a side unit that is a caption block
  (00 00 01 B2 'C' 'C' 01 F8, a count byte with bit 6 clear, 9 + 3 x count
  bytes; the rest of the piece is not part of it) is a caption frame; its
  CEA-608 entries get the field markers the caption decoder takes (from
  their marker bytes and the odd-field-first bit; an unknown pattern ends
  the block). It goes to the title's caption track (a track of its own, not
  in the video): placed on the title timeline like a subtitle, then sent to
  CCExtractor (`libs/ccextractor`, a helper program next to ours,
  `disctl_ccx.c`) in its raw caption format at its time in ms (truncated).
  At the end of the title an erase-displayed-memory pair at the video's end
  ends a caption still shown; the helper's SRT is read back strictly and its
  cues leave as text frames (codec `subrip`). The helper keeps 1 ms off both
  ends of each cue. Caption data without caption text (null pairs only, as on
  the HD DVD corpus) gives no cues. A title without a caption track counts
  the caption blocks in its video and warns (left out).
- **Sub-pictures** (DVD-Video, and HD DVD with 32-bit sizes and 8-bit
  commands): units are cut from the byte stream by their own size (a header
  stating less than 9 bytes, or a first control sequence that does not fit,
  gives up the bytes kept so far; a unit whose control sequences do not parse
  is left out, with a warning). A unit lasts until its stop command
  (STP_DSP delay x 1024/90000 s); a unit that shows a picture without one
  (`DR_F_NO_STOP`) lasts until the next unit of its track starts (across
  segments; the title's last one until the video's end, untested on real
  discs). A unit drawn outside the track's canvas (SET_DAREA) is counted and
  warned about. Its time is the video grid
  point nearest to its PES time (the video must know its grid first, so the
  units wait for it). Left out, each with a warning and an event: a unit
  without a time of its own (a second unit starting in the same PES packet),
  a unit more than 0.1 ms before the video's first field. The bytes are never
  changed.

## 7. Stage 3: joiner

- Segment 0 starts the timeline at its first master (video) time. Segment k
  is placed where the master video of segment k-1 actually ended (its last
  frame's time + duration), not where the disc's tables say it ends.
- Every unit: `out = time + offset(k) - start(k)`.
- Audio units earlier than their track's expected time by more than their
  own duration are moved to it; smaller overlaps pass to stage 4.
- Sub-pictures keep their times: a sub-picture replaces the one shown
  before it. One that starts before the previous one ends, by more than its
  own duration, is reported (event); the reference moves it to the end of
  the previous one instead.
- Chapter marks (`disctl_chapters.c`; time marks: HD DVD, Blu-ray): the
  disc's records are planned first: a broken tail (a last record at 0 after
  one that is not, or earlier than the one before it) is cut off, a record
  earlier than the one before it otherwise refuses the title; records in
  leading segments left out are dropped and the rest move back by their
  length; the first kept record within 0.1 s of the start is at 0; a record
  at the time of the one before it is dropped (the reference keeps it, so
  its later chapter names shift by one); when the first kept record does
  not start the title, a chapter is added at 0 (record 0 when it was
  dropped, else "Chapter 00" when the setting asks). The master's key frame
  at, or up to 0.4 s before, each mark carries the chapter flag (one per
  frame; empty markers take none); the k-th flagged frame starts chapter k,
  which ends where the next starts, the last at the title's end; fewer than
  two -> no chapters. DVD marks by byte position (the first picture past a
  cell's position) come with the DVD wiring.

## 8. Stage 4: junction per output track

- **Title start (audio):** every unit keeps its own time. A track whose
  first unit is after 0 keeps that delay; one whose first unit is before 0
  keeps its units before the video at their times (a stream file's negative
  delay, `DR_EV_START_EARLY`). Earlier versions followed the reference
  program: units before the format's tolerance (1 ms DVD / Blu-ray, 102 ms
  HD DVD) plus one unit dropped and the rest of the lead made a shift of the
  whole track, which put the track out of sync by that rest (SAINTBEAST #3:
  24.133 ms late for the whole episode). A container that cannot hold
  negative times (Matroska) decides at its writer how to keep sync (start
  the video later, or drop whole units before 0 and keep the rest at their
  times).
- **Audio skew** (one value per track, starts at 0):
  - overlap (unit earlier than expected): skew grows;
  - short gap: absorbed while skew - gap > -max(12 ms, unit duration);
  - real gap: reported with the missing frame count, skew back to 0,
    nothing inserted;
  - when skew >= one unit (within one 90 kHz tick): whole units dropped,
    only up to the next sync unit, with 1/16-unit tolerance;
  - every change > 1 ms (or |skew| > 1 ms) is an event.
- **Copies at a join** (every audio codec but TrueHD / MLP): the next
  segment's first frame, when it is byte for byte the frame the segment
  before ends with and the stamps overlap by more than one 90 kHz tick, is
  left out (at most one per join); a longer copy is only reported.
- **TrueHD / MLP joins**: by the streams' own sample numbers. Every restart
  header carries the number of its first sample (output timing, modulo
  65536); the access units after it count on by one unit's samples. At a
  join: the next segment's first unit starts where the earlier one ends:
  everything kept; its second unit starts there: the first holds the
  earlier segment's last samples again and is left out (only when the
  second is a major sync). Either way the skew this leaves counts as right
  (the skew rule only acts on skew beyond it: the stamps put every
  segment's first unit on its first picture, so their overlap is no drift).
  Numbers that do not connect (a piece on another timeline) or a side
  without them: the skew rule. One event per join, a summary per track.
- **PCM tracks** (`disctl_pcm.c`, in place of the junction): the samples
  leave in fixed frames (1/30 s at 48 / 44.1 kHz, else about 32 ms rounded up
  to a divisor of the rate) timed by a sample counter from the track's
  start. Start: a track before the video keeps every frame, its clock (and
  its output times) starting at its first frame (a negative delay); a later
  start below 5 s is filled with silence, from 5 s on skipped on the output
  clock. A frame more than 10800 ticks and 2 samples off the sample
  count: when the frames after it (up to 32) go on from where it would end
  without the gap, its time was broken and it is appended; else silence
  fills the gap (two output frames per step, the rest pending). A frame
  running more than 10800 ticks and 2 samples into the next is cut at the
  next one's start; a next frame starting before this one: its rest past
  this one is kept for the next step, or it is dropped when it lies inside.
  The video ending before a frame leaves the rest out. LPCM units
  (`disctl_lpcm.c`): one frame each, from the packets' audio frame headers,
  converted to little-endian.
- Video: passes unchanged (sets the highest video time the audio side uses).
- Sub-pictures: no junction stage.

## 9. Stage 5: measure, decide, verify

The rules above are the default policy. Around them:

- **Measure** (every frame, every track): source time (PES-derived, on the
  joined timeline), output time, and the stream-file position (sum of the
  output durations before it, from the first frame's time). Per join: the master's
  end, each track's overlap / gap, and for decodable codecs whether the
  overlapping units are duplicates.
- **Decide**: every action (drop, shift, absorb, repair, placeholder) is an
  event with its reason and values.
- **Verify** (after the policy; `disctl_verify.c`, one checker per output
  track, fed every frame as it leaves the core; nothing is changed, every
  finding is an event and counted):
  - every output unit re-parsed by its codec's check: MPEG video / VC-1 a
    start code and a picture / frame start code; AC-3 / E-AC-3 whole
    syncframes, each one's CRC (as FFmpeg's decoder checks it); DTS a core
    frame that fits (the rest an extension substream) or an extension
    substream; MPEG audio, ADTS, LOAS one frame of their header's size;
    TrueHD / MLP the AU length field and the major-sync checksum;
    sub-pictures their size and control sequences; a codec without a check
    is counted as unchecked (LPCM so far);
  - per track: durations > 0; audio / subtitles: each frame after the one
    before it (overlaps counted with the largest); video in display order:
    each picture starts where the one before it ends (within the 1-tick
    truncation of grid times), holes with the placeholders in them,
    overlaps;
  - audio sync error per frame against its own time (`DRFrame.src`, set by
    the joiner: the PES-derived time on the title timeline before any move):
    as a stream file plays it (start delay + the durations before it) and by
    the output times; the largest of each and where, the stream file's at
    the end;
    per segment (`DRFrame.seg`, set by the joiner): a segment where the
    stream-file error goes beyond 1 ms (`DR_SYNC_LIMIT`) is a warning
    (`DR_EV_SYNC`);
  - not yet: joins where a different action would have given a smaller
    error.
- **Title checks** (`disctl_title.c`, the master video's segments and
  joins; nothing is changed, every finding is an event, a warning where it
  may be a fault, and a summary ends the title; a summary that passes is a
  detail line (`-v`, the debug log), so the standard log of a clean title is
  its track list and one line: "done, all checks passed (chapters, size)"):
  - how decoding starts at each segment's first key frame, by the codec's
    own marks (`DRPicture.start`): MPEG-2 closed_gop / broken_link of its GOP
    header, VC-1 CLOSED_ENTRY / BROKEN_LINK of its entry point, H.264 IDR or
    a recovery point (its broken_link_flag), HEVC IDR / BLA / CRA; and the
    pictures after it in decoding order shown before it. An open start or a
    broken link with such pictures is a warning (`DR_EV_JOIN_START`): they
    may need the end of the segment before (or nothing exists before the
    title's start), or cannot be decoded;
  - a segment whose first picture is no key frame (`DR_EV_JOIN_NOT_KEY`);
  - a segment that starts with the pictures the segment before ends with:
    the same bytes in the same order, a key frame among them, up to 32
    (`DR_EV_JOIN_REPEAT`; equal single pictures are no finding, a still
    scene codes to equal skipped pictures);
  - an end of sequence code with pictures after it in its segment
    (`DR_EV_SEQ_END_INSIDE`);
  - per segment the pictures the video stage read and gave out
    (`DR_EV_PICTURES` when they differ);
  - what was taken out of the video (zero stuffing after GOP headers, GOP
    user data: captions to their track, units that are no picture) and the
    scan the pictures state (progressive / interlaced, top field first,
    repeat first field), all kept as they are;
  - per track the output checks above as one line (PASS when nothing was
    found);
  - each segment's first picture against where the disc's own structure
    starts it (`DiscTitleSource.clip_start`: DVD-Video its first VOBU's
    VOBU_S_PTM from the navigation pack, HD DVD its EVOB's start time from
    the VTI, Blu-ray its first PlayItem's IN time): a warning when they
    differ (the title would leave out, or add, what the disc plays there),
    a summary line per title;
  - the title reader: the blocks read, those not usable, packets of streams
    without a track; for program streams (`ff_mpegps_disc_stats`) the
    sectors read and empty, navigation packets, system headers, padding and
    other packets of no stream left out, sectors left out or cut.

Events go to the log (messages 5xxx) and, at debug level, as one structured
line each, so a run can be compared event by event with an expected list.

## 10. Outputs

- **Stream files:** each track's output frames written back to back (the
  same frame set as the Matroska path), plus the chapters. Gaps are logged
  per track; a stream file cannot hold them. Names:
  the title's name from the file-name template (`<title name>_t<NN>`), then
  `_<track>_[<language>]_<codec>[_<channel layout>]`, then ` (forced)`,
  ` (commentary)`, ` (visually impaired)` as the disc marks the track, audio
  with ` DELAY <ms>ms` (its first frame's time against the title start, which
  is the video's, rounded to ms, negative when the audio starts first). The
  shape is the one the MKVToolNix GUI reads a file name in: it splits the
  name at `[ ] ( ) { } . + - = #` (not at `_` or space) and takes a piece
  that is a language code as the track's language, the words forced and
  commentary as those flags, and `delay <number>` as the delay. A language
  the disc does not name is `und`; the video, which no disc format names a
  language for, gets setting `output.video_language`: by default `market`,
  the disc's market language (a Blu-ray's disc library files, a DVD's menus,
  an HD DVD's title set default language; several in no order: English when
  among them), English when the disc names none; or a fixed code. Codec words and layout names are the
  ones the reference's info output uses (`DD`, `DDplus`, `TrueHD`,
  `5.1(side)`, ...; the layout from FFmpeg's decoder on the first frames, through analyze).
  LPCM leaves as little-endian PCM in a RIFF WAVE file (EXTENSIBLE with the
  channel mask above two channels or 16 bits). Sub-pictures as a VobSub
  pair: the units unchanged (DVD-Video and HD DVD forms) in MPEG-2 program
  stream packs of 2048 bytes (`.sub`, private stream 1 sub-stream 0x20, the
  PTS in each unit's first packet), and the `.idx`: the track's index header
  (frame size; the 16 palette entries converted to RGB as the reference
  converts them) and one `timestamp: ..., filepos: ...` line per unit.
  FFmpeg reads both forms; MKVToolNix 2026's VobSub reader cuts HD DVD units
  (32-bit size) to their first packet. HD DVD units of coding 4 are drawn on
  1920x1080 whatever the video's size: that is the frame size in the index.
  Closed captions as SRT (`_CC.srt`; numbered cues, `HH:MM:SS,mmm`, LF). Every title of a disc: one folder per
  title, named like its files.
- **Matroska** (later, libebml + libmatroska): frame times as given; cluster
  cuts placed on every audio / video track at the same time (first master
  sync unit 0.4-32.4 s after the previous cut).

## 11. Build order and tests

1. units + timestamp records; 2. audio durations and markers; 3. junction
(title start, skew, drops, real gaps); 4. joiner; 5. video grid (vote,
check, repair); 6. sub-picture snap; 7. checks; 8. copies at joins;
9. PCM tracks.

Each rule gets a test on built payload streams (`tests/unit/test_disctl_*.py`).
With the corpus variables set, whole titles are run and their event lists
compared with expected lists (HD DVD first, then DVD).
