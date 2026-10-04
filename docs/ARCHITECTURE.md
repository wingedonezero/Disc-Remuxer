# Architecture

Status: skeleton. This file grows with the code.

```
disc-remuxer (crates/disc-cli)     the caller: commands, options, settings, logging
  └─ FFmpeg (libs/ffmpeg)          the processing layer, our edited copy
       ├─ disc detection           (to add) opens a source and picks the format's demuxer
       ├─ per-format demuxers      titles, tracks, segments, timing for one format:
       │    ├─ DVD-Video           FFmpeg's dvdvideo demuxer, to be extended
       │    ├─ Blu-ray             (to add) BD, UHD, 3D
       │    └─ HD DVD              (to add)
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

## Crates

| Crate | Role |
|---|---|
| `disc-cli` | the `disc-remuxer` binary |
| `disc-core` | settings (registry, settings file, values in effect); later the message catalog, job folders and job logs |
| `ffmpeg-sys` | builds `libs/ffmpeg`; C glue (`glue/glue.c`) + declarations |
| `libdvdnav-sys`, `libdvdread-sys`, `libdvdcss-sys` | build `libs/libdvd*` |

## Commands so far

- `disc-remuxer version` — program and library versions.
- `disc-remuxer probe <source> [--title N]` — FFmpeg's DVD-Video demuxer as
  it is, one title: stream list, chapters. For checking its behaviour on real
  discs before changing it.
- `disc-remuxer settings [show|path]` — every setting with its value and
  source; the settings file's path.
- Global: `-v` / `-vv` (debug / trace; includes FFmpeg's, libdvdnav's and
  libdvdread's log lines), `-q`, `--settings FILE`, `--set group.key=value`.

## Settings

One registry in `disc-core` lists every setting (group, key, type, default,
help). The settings file (`~/.config/disc-remuxer/settings.toml`, or
`--settings`) always lists all of them with their help text. On every start it
is brought in step with the registry: missing settings are added with their
default, unknown entries removed, kept values never changed, the old file saved
as a backup first. A wrong value stops the program and leaves the file as it
is. Value in effect: default, then settings file, then command line.
