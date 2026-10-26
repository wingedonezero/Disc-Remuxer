# Disc-Remuxer

@~/.claude/disc-remuxer.local.md

> The line above imports the maintainer's private instructions, kept outside
> this repository. When that file is present it holds all project rules and
> is authoritative — read it first. **This file stays minimal: never add
> private details to it, to code, or to commit messages.**

A C program (`disc-remuxer`, `src/disc-remuxer/`, with a Qt 6 GUI in `src/gui/`) that rips DVD,
HD DVD and Blu-ray / UHD sources into elementary streams (Matroska later). The whole demux path —
readers, demuxers, frame cutters, the title timeline, the writer — is our own code in `src/`;
FFmpeg (the maintainer's fork) is only an analysis instrument, linked from `src/analyze/`.
libdvdread, libdvdnav and libdvdcss read DVDs (table parsing, the navigation VM, CSS); expat parses the HD DVD
playlists; libaacs (on libgcrypt / libgpg-error) handles AACS; CCExtractor (a helper program) turns
closed captions into text. All live in `libs/` (see `libs/README.md`: what is changed in each).

## Build and test

```bash
./build.sh            # debug build -> dist/debug/ (./build.sh release -> dist/release/)
./build.sh test       # debug build, then every test (pytest; arguments go to pytest)
```

`./build.sh` (the top `Makefile` and `mk/*.mk`) compiles the DVD libraries
and expat directly, FFmpeg, libaacs, libgcrypt and libgpg-error with their own
configure + make, CCExtractor with its own CMake, and the GUI with CMake against the host's Qt 6
(needs a C and C++ compiler, make, nasm, pkg-config, cmake, rsync, the Qt 6 Widgets
development files), all into `build/<mode>/`, and copies only the finished
programs (`disc-remuxer`, `disc-remuxer-gui`, ...) into `dist/<mode>/`. The tests (`tests/`, Python) call our
C code through a cffi bridge built against the same libraries.

## Layout

See `docs/ARCHITECTURE.md` and `libs/README.md`.

- `src/disc-remuxer/` — the engine (`libdisc-remuxer.a`, `engine.h`) and the command line (`main.c`)
- `src/gui/` — the GUI (C++, Qt 6 Widgets), linked with the engine
- `src/disc/` (with `dvd/`, `hddvd/`, `bluray/`), `src/timeline/`, `src/mux/`, `src/parse/`, `src/io/`,
  `src/base/` — the demux path, top to bottom; `src/analyze/` — the one module that links FFmpeg
- `libs/` — FFmpeg and every library (see `libs/README.md`: what is changed in each)
- `mk/` — the build rules (`Makefile`, `build.sh`)
- `tests/` — unit tests (`unit/`), test data builders (`synth/`), helpers, the cffi bridge (`bridge/`)

## Commits

Tag every commit subject: `[impl]` (disc-handling code), `[infra]` (workspace, build, docs,
logging), `[binding]` (C libraries and their wrappers), `[tooling]` (helper tools) or `[test]`.
