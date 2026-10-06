# Disc-Remuxer

@~/.claude/disc-remuxer.local.md

> The line above imports the maintainer's private instructions, kept outside
> this repository. When that file is present it holds all project rules and
> is authoritative — read it first. **This file stays minimal: never add
> private details to it, to code, or to commit messages.**

A Rust CLI (`disc-remuxer`) that rips DVD, Blu-ray and HD DVD sources into
Matroska files or elementary streams. Our own copy of FFmpeg is the processing
layer; it is built with our copies of libdvdread, libdvdnav and libdvdcss;
expat parses the HD DVD playlists; libaacs (on libgcrypt / libgpg-error)
handles AACS; CCExtractor (a helper program) turns closed captions into
text. All live in `libs/` and are edited directly (see `libs/README.md`).

## Build and test

```bash
./build.sh            # debug build -> dist/debug/ (./build.sh release -> dist/release/)
cargo test --workspace
cargo clippy --workspace --all-targets
```

`./build.sh` (the top `Makefile` and `mk/*.mk`) compiles the DVD libraries
and expat directly, FFmpeg, libaacs, libgcrypt and libgpg-error with their own
configure + make, CCExtractor with its own CMake (needs a C compiler, make,
nasm, pkg-config, cmake, rsync), all into `build/<mode>/`, and copies only the
finished programs into `dist/<mode>/`. The Rust crates (moving to C) still
build the current CLI and run the tests.

## Layout

See `docs/ARCHITECTURE.md` and `libs/README.md`.

- `crates/disc-cli` — the `disc-remuxer` binary
- `crates/ffmpeg-sys` — builds `libs/ffmpeg`, C glue and declarations
- `crates/libdvd*-sys` — build `libs/libdvd*`
- `crates/libexpat-sys` — builds `libs/expat`
- `crates/libaacs-sys`, `libgcrypt-sys`, `libgpg-error-sys` — build `libs/libaacs` and its crypto
- `crates/ccextractor-sys` — builds `libs/ccextractor` into the `ccextractor` helper program

## Commits

Tag every commit subject: `[impl]` (disc-handling code), `[infra]` (workspace, build, docs,
logging), `[binding]` (C libraries and their wrappers), `[tooling]` (helper tools) or `[test]`.
