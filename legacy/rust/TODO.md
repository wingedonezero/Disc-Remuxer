# The Rust crates: what is left to carry over

The program is C now (`src/disc-remuxer/`, built by `./build.sh`) and the tests
are Python (`tests/`, run by `./build.sh test`). These crates are the Rust
program and tests that came before. 353 of their 370 tests are ported, under
the same names (`tests/unit/test_<file>.py`, `test_<name>`). The crates are
kept here only until the list below is done. Then this folder goes.

They no longer build from here: their build scripts find `libs/` by relative
path from the old place.

## Tests still to port

They wait for the parts of the program they belong to.

### The stream writer: 4 tests, when the whole-tool tests come

`crates/ffmpeg-sys/tests/demux_writer.rs` tested the old glue writer. The C
program's `writer.c` does the same job, so these become whole-tool tests
(`disc-remuxer demux` on a built HD DVD):

- `an_audio_stream_is_named_by_codec_layout_and_delay`: the file
  `<name>_00_und_DD_stereo DELAY 0ms.ac3`, the frames back to back.
- `pcm_goes_into_a_wave_file`: 16-bit stereo, the 44-byte RIFF header with the
  sizes filled in at the end.
- `multichannel_pcm_takes_the_extensible_header`: 24-bit 5.1,
  WAVE_FORMAT_EXTENSIBLE with channel mask 0x3F, the 68-byte header.
- `sub_pictures_go_into_a_vobsub_pair`: units into 2048-byte packs (a large
  unit over two packs), the `.idx` with the language and the times.

Already ported: `the_index_header_converts_the_palette_as_the_reference_does`.

### Closed captions: 2 tests, when captions are wired into the C program

`crates/ccextractor-sys/tests/helper.rs`: the two tests of the real helper
program are ported (`tests/unit/test_helper.py`). These two test the Rust SRT
reader. They come back with the C code that reads the helper's SRT:

- `the_srt_reader_takes_cues_as_the_helper_writes_them`: a BOM, CRLF lines,
  numbered cues, times as `HH:MM:SS,mmm`.
- `the_srt_reader_refuses_what_the_helper_does_not_write`: numbers not from 1,
  a wrong arrow, a short timing line, non-ASCII times.

### Settings and file names: whole-tool tests of the C program

The Rust settings and names code is replaced by `settings.c` and `names.c`.
Their behaviours become whole-tool tests:

- `crates/disc-core/tests/settings_file.rs` (4):
  - a missing file is created with every setting at its default, and loading
    again changes nothing;
  - the file is brought in step (new settings added, unknown ones removed)
    and the user's values are kept;
  - wrong values stop the run and leave the file untouched;
  - `--set` overrides the file and refuses unknown keys and bad values.
- `crates/disc-core/src/names.rs` (5): the default template (name and title
  number), the title word standing in for a missing name, the cleaning
  rules, the comment / date / number forms, invalid templates.

### Rust-only: not ported (decide when the folder goes)

- `crates/disc-core/tests/catalog_docs.rs` (3): message numbers unique and in
  their area, message fields filled in, and the generated `SETTINGS.md`
  matching the registry. Worth carrying over for the C program: a test that
  the numbers in `msg.h` are unique and in their area, and generated docs for
  its commands and settings (the Rust ones, `CLI.md` / `SETTINGS.md`, are kept
  in this folder).
- `crates/disc-core/tests/discs_find.rs` (4): finding discs under a folder and
  choosing job folders. That is the GUI's job now.
- `crates/disc-cli/src` (2): the generated `CLI.md` matching the Rust CLI, and
  the read settings becoming demuxer options (the C program's `open_title`
  does this; a whole-tool test can cover it).

## Real-disc tests: built-data versions

The `disc`-marked tests need real discs and reference files. The aim is a
built-data test for everything they check, so that every run checks it.
`make coverage` and `tests/tools/coverage.py` show which lines only the real
discs reach. Comparing the built-data run with the real-disc run that can
still be made here (2026-10-06):

- **DVD navigation scan:** `dvdvideo_scan.c` (about 215 lines) and most of
  libdvdnav's VM. The built DVDs have no first-play program chain, so the scan
  stops at once. Needed: a built DVD whose first-play chain jumps into its
  titles (and menus). Do this when DVD is wired into the core.
- **DVD CSS:** the key search in `dvdvideo_source.c` (about 70 lines) and
  libdvdcss's key cracking. Needed: sectors scrambled in the tests with a
  made-up key that libdvdcss can find. Do this with DVD.
- **HD DVD:** 19 lines, PES and pack header variants in `hddvd_tracks.c`
  (stuffing, extensions). Small.
- **UDF (NetBSD-based):** 20 lines of small cases. Small.

Real-disc tests whose reference files no longer exist: `DISCIO_FS_REFERENCE`,
`DISCIO_UDF_REFERENCE` (dumps), `DVDVIDEO_SCAN_REFERENCE`,
`DVDVIDEO_CSS_FOLDER`, and the block list of `DVDCSS_SCRAMBLED_IMAGE`. Their
tests show as skipped. The built-data versions above replace what they
checked.

## Commands of the Rust CLI

- `debug dvd-scan`, `debug dvd-titles`: move to the C program as
  `disc-remuxer debug dvd-scan` / `dvd-titles` (same output lines).
- `scan` (finding discs, job folders) and `probe` (stock FFmpeg DVD
  demuxer): not carried over; the GUI finds discs, `info` replaces `probe`.

## Already moved out of here

- `ffmpeg-sys/glue/glue.c`: the log routing and the parser runner are in
  `tests/bridge/bridge.c`; its writer (`dr_demux`) became `writer.c`.
- `libdvdnav-sys/glue/selftest.c`: now `tests/bridge/dvdnav_selftest.c`.
- `ffmpeg-sys/tests/data/`: now `tests/data/`.
