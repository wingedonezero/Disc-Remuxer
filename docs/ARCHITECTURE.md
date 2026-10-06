# Architecture

Status: skeleton. This file grows with the code.

```
disc-remuxer (crates/disc-cli)     the caller: commands, options, settings, logging
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

## Crates

| Crate | Role |
|---|---|
| `disc-cli` | the `disc-remuxer` binary |
| `disc-core` | settings (registry, settings file, values in effect), the message catalog, finding discs and choosing job folders |
| `ffmpeg-sys` | builds `libs/ffmpeg`; C glue (`glue/glue.c`) + declarations |
| `libdvdnav-sys`, `libdvdread-sys`, `libdvdcss-sys` | build `libs/libdvd*` |
| `libexpat-sys` | builds `libs/expat` (XML parser for the HD DVD playlists) |
| `libaacs-sys`, `libgcrypt-sys`, `libgpg-error-sys` | build `libs/libaacs` (AACS) on `libs/libgcrypt` and `libs/libgpg-error` |
| `ccextractor-sys` | builds `libs/ccextractor` (C only, its own CMake) into the `ccextractor` helper program next to ours: CEA-608 closed captions in, SRT out, in its own process |

## Commands so far

- `disc-remuxer version` — program and library versions.
- `disc-remuxer scan <path> [--out DIR]` — the discs found and their job
  folders; creates nothing.
- `disc-remuxer probe <path> [--title N] [--out DIR]` — FFmpeg's DVD-Video
  demuxer as it is, one title of every disc found: stream list, chapters;
  with an output folder each disc is a job with its job folder and logs.
- `disc-remuxer settings [show|path]` — every setting with its value and
  source; the settings file's path.
- Global: `-v` / `-vv` (debug / trace; includes FFmpeg's, libdvdnav's and
  libdvdread's log lines), `-q`, `--settings FILE`, `--set group.key=value`.

## Commands, settings, messages, jobs

Commands and options: `docs/CLI.md`; settings: `docs/SETTINGS.md` (both
generated from the code, kept in step by tests).

Messages the program writes come from one catalog (`disc-core/src/msg.rs`):
number, level, text with named fields, written as `[id] text`. Number areas:
1xxx program / settings / jobs, 2xxx sources / discs, 3xxx titles, 4xxx
reading, 5xxx streams / timeline, 6xxx output. Lines from FFmpeg and the
libraries under it are written as they come, prefixed `ffmpeg:`.

Discs are found under a given path (`output.scan_depth` levels deep: folders
holding VIDEO_TS or BDMV, folders holding the disc files directly, `.iso` /
`.img` images). With an output folder (`--out` or `output.root`) every disc is
a job with its own job folder (named after the disc folder or image;
`output.keep_structure` recreates the searched tree; `_001`, `_002`, ... on a
clash) holding `<folder>_disc-remuxer.log` (info and up, timestamps, starting
with the command and every setting in effect) and, with `log.debug_file`,
`<folder>_disc-remuxer_debug.log` (everything). The console shows up to
`log.console` (`-q` / `-v` override).

### Settings file

One registry in `disc-core` lists every setting (group, key, type, default,
help). The settings file (`~/.config/disc-remuxer/settings.toml`, or
`--settings`) always lists all of them with their help text. On every start it
is brought in step with the registry: missing settings are added with their
default, unknown entries removed, kept values never changed, the old file saved
as a backup first. A wrong value stops the program and leaves the file as it
is. Value in effect: default, then settings file, then command line.
