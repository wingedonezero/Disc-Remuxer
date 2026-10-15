# disc-remuxer

Rips DVD-Video, HD DVD and Blu-ray (UHD, 3-D) sources — disc images and disc
folders — into elementary-stream files with their chapters: the video, every
audio track (LPCM as WAVE), subtitles (VobSub, PGS, TextST), closed captions
as SRT, chapters as Matroska XML, and a log of everything the disc showed.
Matroska output comes later.

A GUI (`disc-remuxer-gui`, Qt 6) and a command line (`disc-remuxer`) on one
engine: what one can choose or show, the other can too.

## Building

Needs a C and C++ compiler, `make`, `nasm`, `pkg-config`, `cmake`, `rsync`
and the Qt 6 Widgets development files. Everything, including every library
in `libs/`, is built by:

```
./build.sh            # debug   -> dist/debug/
./build.sh release    # release -> dist/release/
```

`dist/<mode>/` holds only the finished programs (all libraries linked in);
the intermediate files stay in `build/<mode>/`. Linux is the only configured
platform so far.

## Using it

```
disc-remuxer-gui                               # the GUI
disc-remuxer info  <disc>                      # titles and tracks (--details: what the GUI shows)
disc-remuxer demux <disc> all [out folder]     # stream files, chapters, a log
disc-remuxer demux <disc> 1,3-5 [out folder]
disc-remuxer demux <folder of discs> all       # each disc in turn, into its own folder
disc-remuxer find  <folder>                    # the discs found and their output folders
disc-remuxer settings                          # settings in effect
disc-remuxer --json demux ...                  # JSON records instead of text
```

Settings live in `disc-remuxer.toml` next to the program (written with
every setting and its help on the first run), shared by the GUI and the
command line; `--set group.key=value` changes one for a single run. AACS key
files go into `keydb/` next to the program; anything else the libraries keep
goes into `.config/` there.

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
