# disc-remuxer

Rips DVD, Blu-ray and HD DVD sources into Matroska files or standalone
elementary streams.

Early development, DVD first.

## Building

Needs a Rust toolchain (1.85 or later), a C compiler, `make`, `nasm` and
`pkg-config`. Everything, including our copies of FFmpeg, libdvdread,
libdvdnav and libdvdcss in `libs/`, is built by:

```
cargo build --release
cargo test --workspace
```

The binary is `target/release/disc-remuxer`; the libraries are linked
statically. Linux is the only configured platform so far.

## Layout

See `docs/ARCHITECTURE.md` and `libs/README.md`.

## License

GPL-2.0-only for our code; the libraries in `libs/` keep their own licenses
(see `libs/README.md`).
