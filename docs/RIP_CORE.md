# The shared rip core

Status: design, before code. One module for every disc format (HD DVD first,
then DVD, then Blu-ray / UHD). It turns a title's segments into timed, joined,
checked frames per track. The outputs (elementary streams, later Matroska)
only write what it gives them.

## 1. Where it sits

```
format demuxer (hddvddec.c, dvdvideodec.c, later Blu-ray)
  title plan, segments (EVOB clip / cell group / playlist clip), tracks,
  packet source per segment (MPEG-PS / MPEG-TS sub-demuxer, raw PES payloads)
        |  payload records: track, segment, bytes, PES PTS (or none), byte position
        v
rip core (libavformat/discrip_*.c)
  1 units      cut each track into units (FFmpeg parsers), timestamp records
  2 timing     per track: video grid / audio durations / sub-picture snap
  3 joiner     segments -> one timeline per track
  4 junction   per output track: title start, audio skew (absorb / drop),
               seamless overlap search, PCM gaps
  5 checks     measure + verify, events
        |  frames: data, time, duration, flags (key, discardable, chapter mark)
        v
format demuxer read_packet -> outputs (stream files; Matroska later)
```

The format demuxer keeps everything format-specific: opening the disc, the
title plan, the track list, decryption, and the packet source. It hands the
core raw PES payloads: its sub-demuxer runs with `AVFMT_FLAG_NOPARSE |
AVFMT_FLAG_NOFILLIN`, so FFmpeg's generic layer neither cuts frames nor fills
in timestamps. Where a sub-demuxer must strip a header differently on one
format (HD DVD LPCM, for example), it is copied under the format prefix
(`hddvd_ps.c` from `mpeg.c`).

The demuxer's packets out of `read_packet` are the core's frames: final
time, duration and flags, already joined. Outer streams have no parser
(`AVSTREAM_PARSE_NONE`), so nothing re-times them.

## 2. Time

One internal unit: 1/1,080,000,000 s (27 MHz x 40). It is exact for 90 kHz
PES times (x 12,000), for video frame and field durations (1001/30000 s =
36,036,000; 1001/24000 s = 45,045,000) and for 48 / 96 / 192 kHz samples.
Durations are `samples * 1,080,000,000 / rate` in unsigned 64-bit integers
(truncated), always computed the same way so results are reproducible.

PES PTS wrap: the reference time of a source is the first PTS of the master
track within its first 2000 sectors minus 300 s; a PTS below it gets +2^33.

## 3. Stage 1: units and timestamp records

- Each payload is one record {byte position, length, PTS}. The payload goes
  to the track's unit cutter (`av_parser_parse2`; the parser's own timestamp
  logic is not used).
- A unit gets a record's PTS when it is the FIRST unit that starts inside
  that record's bytes. Every later unit of the same payload has no
  timestamp. A record that ends before the next unit starts is dropped.
- The cutter is FFmpeg's parser where it cuts the same; a prefixed copy where
  it must differ (e.g. MPEG-2 GOP user data as a separate unit, VC-1 field
  pictures paired).

## 4. The codec table

Every track's codec has one entry. A missing entry = the track is refused,
logged with the reason; nothing is guessed.

| entry | meaning |
|---|---|
| family | `timed` (duration and sync rule below) or `stream` (duration from the cutter, never a sync unit) |
| cutter | parser id, or our copy |
| duration | rule for each unit (e.g. AC-3: samples / rate from the stream's first header; TrueHD: per major-sync access unit) |
| sync unit | whether a unit may start a drop / join (AC-3, E-AC-3, DTS: every frame; TrueHD: major-sync units only) |
| decoder | FFmpeg decoder for the overlap search, or none |
| core cut | unit cut to its core frame (when the track is a derived core) |
| video only | units per frame (2 = fields), frame-rate source, display-order number and its wrap period |
| sub-picture only | own cutter (unit size from its header); duration from the display-stop command; snapped to the video grid |

## 5. The format profile

| entry | HD DVD | DVD | Blu-ray / UHD |
|---|---|---|---|
| packet source | MPEG-PS (`hddvd_ps.c`) | MPEG-PS | MPEG-TS |
| LPCM extra header | 5 bytes | 3 bytes | own format |
| audio lead-in tolerance | 102 ms | 1 ms | 1 ms |
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
  the block). The text comes from CCExtractor (`libs/ccextractor`, a helper
  program: the frames in its raw caption format, SRT out).
- **Sub-pictures** (DVD-Video, and HD DVD with 32-bit sizes and 8-bit
  commands): units are cut from the byte stream by their own size (a header
  stating less than 9 bytes, or a first control sequence that does not fit,
  gives up the bytes kept so far; a unit whose control sequences do not parse
  is left out, with a warning). A unit lasts until its stop command
  (STP_DSP delay x 1024/90000 s); without one it lasts one delay step and is
  marked when it shows a picture (`DR_F_NO_STOP`). Its time is the video grid
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
- Chapter marks (`discrip_chapters.c`; time marks: HD DVD, Blu-ray): the
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

- **Title start (audio):** a first unit before 0 by more than the format's
  tolerance has its earlier units dropped (whole units, keep at most
  tolerance + one unit of lead); the remaining lead becomes a permanent
  shift of the whole track. A first unit after 0 keeps its delay.
- **Audio skew** (one value per track, starts at the shift):
  - overlap (unit earlier than expected): skew grows;
  - short gap: absorbed while skew - gap > -max(12 ms, unit duration);
  - real gap: reported with the missing frame count, skew back to 0,
    nothing inserted;
  - when skew >= shift + one unit: whole units dropped, only up to the next
    sync unit, with 1/16-unit tolerance;
  - every change > 1 ms (or |skew| > 1 ms) is an event.
- **Seamless overlap search** (`discrip_seamless.c`; entered by codecs with
  non-sync units: TrueHD / MLP, compared only for those): at the tail of a
  segment (frames from its last 64 MiB) a non-sync frame whose contiguous run
  (at most 0.5 s, 99 frames) reaches past the start of the next segment's
  first sync unit U. Candidate counts of duplicated frames: overlap / frame
  duration +-5 (at least 1, at most the frames before U). Both sides are
  decoded (side A after the frames handed on since the last sync unit;
  side B = U and the frames after it, as many as side A + 2), turned into
  32-bit samples, mixed to one channel (libavresample's matrix: centre 1,
  surround sqrt(2)/8, LFE 0, normalised; Q30 integer mix), and for each
  candidate the end of A from where its frames start is compared with the
  start of B: the Pearson correlation in Q32, all integer (silence = 0); the
  last candidate above 0.95 is the match. k = the smallest candidate whose
  frames fit the overlap (within frame / 16). A match below k + 2 drops its
  frames at the end of A (U's major sync is kept) and the skew becomes where
  the frames before them ended against U; else k = 0: A goes on unchanged,
  or the k frames are re-timed to end at U and the skew rule follows. Each
  search is an event with its best correlation (for comparing it with other
  ways of joining, Blu-ray).
- **PCM tracks** (`discrip_pcm.c`, in place of the junction): the samples
  leave in fixed frames (1/30 s at 48 / 44.1 kHz, else about 32 ms rounded up
  to a divisor of the rate) timed by a sample counter from 0. Start: frames
  more than 1 ms early are dropped, a lead-in within 1 ms is the origin; a
  later start below 5 s is filled with silence, from 5 s on skipped on the
  output clock. A frame more than 10800 ticks and 2 samples off the sample
  count: when the frames after it (up to 32) go on from where it would end
  without the gap, its time was broken and it is appended; else silence
  fills the gap (two output frames per step, the rest pending). A frame
  running more than 10800 ticks and 2 samples into the next is cut at the
  next one's start; a next frame starting before this one: its rest past
  this one is kept for the next step, or it is dropped when it lies inside.
  The video ending before a frame leaves the rest out. LPCM units
  (`discrip_lpcm.c`): one frame each, from the packets' audio frame headers,
  converted to little-endian.
- Video: passes unchanged (sets the highest video time the audio side uses).
- Sub-pictures: no junction stage.

## 9. Stage 5: measure, decide, verify

The rules above are the default policy. Around them:

- **Measure** (every frame, every track): source time (PES-derived, on the
  joined timeline), output time, and the stream-file position (sum of the
  output durations before it, plus the start shift). Per join: the master's
  end, each track's overlap / gap, and for decodable codecs whether the
  overlapping units are duplicates.
- **Decide**: every action (drop, shift, absorb, repair, placeholder) is an
  event with its reason and values.
- **Verify** (after the policy; `discrip_verify.c`, one checker per output
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
  - TrueHD / MLP: each AU's input timing against the one before it (+ the
    samples per AU), every break and its size;
  - not yet: joins where a different action would have given a smaller
    error.

Events go to the log (messages 5xxx) and, at debug level, as one structured
line each, so a run can be compared event by event with an expected list.

## 10. Outputs

- **Stream files:** each track's output frames written back to back (the
  same frame set as the Matroska path), plus the chapters. Gaps and the
  start shift are logged per track; a stream file cannot hold them.
- **Matroska** (later, libebml + libmatroska): frame times as given; cluster
  cuts placed on every audio / video track at the same time (first master
  sync unit 0.4-32.4 s after the previous cut).

## 11. Build order and tests

1. units + timestamp records; 2. audio durations and markers; 3. junction
(title start, skew, drops, real gaps); 4. joiner; 5. video grid (vote,
check, repair); 6. sub-picture snap; 7. checks; 8. seamless overlap search;
9. PCM tracks.

Each rule gets a test on built payload streams (`crates/ffmpeg-sys/tests/`).
With the corpus variables set, whole titles are run and their event lists
compared with expected lists (HD DVD first, then DVD).
