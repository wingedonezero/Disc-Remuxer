# Architecture

Status: skeleton. This file grows with the code.

```
disc-remuxer (src/disc-remuxer)   the caller: commands, options, settings, logging
  └─ FFmpeg (libs/ffmpeg)          the processing layer, our edited copy
       ├─ disc detection           (to add) opens a source and picks the format's demuxer
       ├─ per-format demuxers      titles, tracks, segments, timing for one format:
       │    ├─ DVD-Video           FFmpeg's dvdvideo demuxer, to be extended
       │    ├─ Blu-ray             (to add) BD, UHD, 3D
       │    └─ HD DVD              our hddvd demuxer (titles and tracks; EVOB reading to add)
       └─ sub-demuxers, parsers, codecs (MPEG-PS, MPEG-TS, ...), with disc-type
          specific handling where a codec behaves differently on a format
  └─ outputs                       (to add) elementary streams; Matroska
```

The DVD-Video demuxer (`libs/ffmpeg/libavformat/`) is split into files:

| File | Part |
|---|---|
| `dvdvideodec.c` | the orchestrator: FFmpeg's demuxer entry points (open, read packet, seek, close), sub-demuxer handling, options |
| `dvdvideo_internal.h` | structures and functions shared by the files |
| `dvdvideo_ifo.c` | opening the volume and the IFO structures |
| `dvdvideo_play.c` | reading MPEG-PS blocks by playback through libdvdnav (titles) and from the menu VOBs; to be replaced by our own navigation scan and reading layer |
| `dvdvideo_chapters.c` | chapter markers |
| `dvdvideo_streams.c` | video, audio and subpicture streams from the IFO attributes |
| `dvdclut.c` | subpicture palette conversion |

DVD reading underneath FFmpeg: libdvdnav (navigation), libdvdread (file
system, IFO, NAV packets), libdvdcss (CSS) — all from `libs/`.

The HD DVD demuxer (`libs/ffmpeg/libavformat/`, Advanced Content images
read through the disc readers `discio_*`):

| File | Part |
|---|---|
| `hddvddec.c` | the orchestrator: open (VTI, playlists, AACS, title plan, tracks), options, the chosen title's streams |
| `hddvd_internal.h` | structures and functions shared by the files |
| `hddvd_vti.c` | the Advanced VTS information file: attribute and EVOB tables |
| `hddvd_xpl.c` | the playlists (XML, through expat) |
| `hddvd_titles.c` | the title plan: titles from the playlists' clip runs, time maps, chapters |
| `hddvd_aacs.c` | AACS keys (through libaacs) and making EVOB sectors usable |
| `hddvd_tracks.c` | the tracks of each title: attribute records, playlist languages, the Dolby Digital Plus probe |
| `disclang.c` | language codes written on discs -> ISO 639-2 |

## The program (`src/disc-remuxer/`, C)

| File | Part |
|---|---|
| `main.c` | commands (`info`, `demux`, `settings`, `version`), options (`--config`, `--set`, `--json`, `-v`), the run |
| `settings.c` | the settings registry and `disc-remuxer.toml` next to the program (written with every setting and its help; new settings added, unknown ones removed, values kept) |
| `writer.c` | opening a title and writing its stream files, chapters and VobSub pairs |
| `names.c` | file names from `output.file_name_template` |
| `log.c` | the log: terminal (colours), `--json` records, a log file per disc in the output folder |
| `msg.h` | the numbers of the program's own messages |

The demuxer contract the program relies on: `docs/FORMATS.md`. Finding
discs, job folders and batches are the GUI's job; the program handles one
disc per run.

## Tests (`tests/`, Python)

`./build.sh test` builds the program and a bridge (cffi, compiled against
our headers and linked with the static libraries of the mode), then runs
pytest: `tests/unit/` calls our C code directly on data built byte by byte
(`tests/synth/`, helpers in `tests/helpers/`); tests marked `disc` need a
real disc named by an environment variable and show as skipped without one.
`make coverage` and `tests/tools/coverage.py` show which lines a test run
reaches.

The Rust crates that came before the C program, and the tests still to port
from them, are in `legacy/rust/` (see its `TODO.md`).
