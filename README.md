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

## Using it

```
disc-remuxer info  <disc>                      # titles and tracks
disc-remuxer demux <disc> all <out folder>     # stream files, chapters, a log
disc-remuxer demux <disc> 1,3-5 <out folder>
disc-remuxer settings                          # settings in effect
disc-remuxer --json demux ...                  # JSON records for a GUI
```

Settings live in `disc-remuxer.toml` next to the program (written with
every setting and its help on the first run); `--set group.key=value`
changes one for a single run. Anything the libraries keep (key caches, AACS
key files) goes into `.config/` next to the program. Only HD DVD images so
far.

## Testing

```
./build.sh test           # the debug build, then every test (pytest)
./build.sh test -k vc1    # arguments go to pytest
```

Needs `python3` with `venv` and its headers; the test tools are installed
into `build/venv` on the first run.

## Layout

See `docs/ARCHITECTURE.md` and `libs/README.md`.

## License

GPL-2.0-only for our code; the libraries in `libs/` keep their own licenses
(see `libs/README.md`).
