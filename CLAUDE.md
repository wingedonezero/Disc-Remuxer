# Disc-Remuxer

@~/.claude/disc-remuxer.local.md

> The line above imports the maintainer's private instructions, kept outside
> this repository. When that file is present it holds all project rules and
> is authoritative — read it first. **This file stays minimal: never add
> private details to it, to code, or to commit messages.**

A Rust CLI (`disc-remuxer`) for extracting elementary streams from DVD,
Blu-ray and UHD sources, built on upstream OSS libraries (libdvdread,
libdvdcss, libdvdnav).

## Build and test

```bash
git submodule update --init vendor/libdvd*   # fresh clones and worktrees
cargo build
cargo test --workspace
```

The build compiles libdvdread / libdvdcss / libdvdnav from source, so it needs
meson, ninja and a C compiler (see `README.md`).

## Layout

- `crates/disc-core` — format-agnostic model, selection, errors, checks
- `crates/disc-dvd` — DVD backend and the rip pipeline
- `crates/disc-cli` — the `disc-remuxer` binary
- `crates/libdvd*-sys` — FFI bindings; `vendor/` holds the C libraries as
  git submodules

## Commits

Tag every commit subject: `[infra]`, `[tooling]`, `[test]` or `[binding]`.
