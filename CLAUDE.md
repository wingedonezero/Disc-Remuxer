# Disc-Remuxer

@~/.claude/disc-remuxer.local.md

> The line above imports the maintainer's private instructions, kept outside
> this repository. When that file is present it holds all project rules and
> is authoritative — read it first. **This file stays minimal: never add
> private details to it, to code, or to commit messages.**

A Rust CLI (`disc-remuxer`) that rips DVD, Blu-ray and HD DVD sources into
Matroska files or elementary streams. Our own copy of FFmpeg is the processing
layer; it is built with our copies of libdvdread, libdvdnav and libdvdcss.
All four live in `libs/` and are edited directly (see `libs/README.md`).

## Build and test

```bash
cargo build
cargo test --workspace
cargo clippy --workspace --all-targets
```

The build compiles the DVD libraries with the `cc` crate and FFmpeg with its
own configure + make (needs a C compiler, make, nasm, pkg-config).

## Layout

See `docs/ARCHITECTURE.md` and `libs/README.md`.

- `crates/disc-cli` — the `disc-remuxer` binary
- `crates/ffmpeg-sys` — builds `libs/ffmpeg`, C glue and declarations
- `crates/libdvd*-sys` — build `libs/libdvd*`

## Commits

Tag every commit subject: `[impl]` (disc-handling code), `[infra]` (workspace, build, docs,
logging), `[binding]` (C libraries and their wrappers), `[tooling]` (helper tools) or `[test]`.
