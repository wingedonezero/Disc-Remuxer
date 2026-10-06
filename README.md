# disc-remuxer

Rips DVD, Blu-ray and HD DVD sources into Matroska files or standalone
elementary streams.

Early development, DVD first.

## Building

Needs a C compiler, `make`, `nasm`, `pkg-config`, `cmake` and `rsync`.
Everything, including our copies of FFmpeg and every library in `libs/`, is
built by:

```
./build.sh            # debug   -> dist/debug/
./build.sh release    # release -> dist/release/
```

`dist/<mode>/` holds only the finished programs (all libraries linked in);
the intermediate files stay in `build/<mode>/`. Linux is the only configured
platform so far.

While the program moves from Rust to C, the Rust crates still build the
current `disc-remuxer` and run the tests (`cargo build`, `cargo test
--workspace`).

## Layout

See `docs/ARCHITECTURE.md` and `libs/README.md`.

## License

GPL-2.0-only for our code; the libraries in `libs/` keep their own licenses
(see `libs/README.md`).
