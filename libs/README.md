# libs — our libraries

These are **our copies** of the libraries the program is built on. They are
edited directly whenever the program needs different behaviour; nothing is
kept as a patch series. The first commit of each library is the unmodified
upstream release, so `git log -- libs/<name>` / `git diff <first commit> --
libs/<name>` show every change we made.

| Folder | Upstream release | Source | Check |
|---|---|---|---|
| `ffmpeg/` | FFmpeg 9.0.2 | https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz | tarball sha256 `8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`; signature good (FFmpeg release signing key FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8) |
| `libdvdread/` | libdvdread 7.1.1 | https://download.videolan.org/pub/videolan/libdvdread/7.1.1/ | tarball sha256 `a0d47876548bec806774bbf8dbf20bb19ba139464383156b32eb8e59915b90a9` (= published .sha256) |
| `libdvdnav/` | libdvdnav 7.0.0 | https://download.videolan.org/pub/videolan/libdvdnav/7.0.0/ | tarball sha256 `a2a18f5ad36d133c74bf9106b6445806fa253b09141a46392550394b647b221e` (= published .sha256) |
| `libdvdcss/` | libdvdcss 1.6.0 | https://download.videolan.org/pub/videolan/libdvdcss/1.6.0/ | tarball sha256 `7ea556c846b7bfc32d47b41cae56d1863a6b6d5f706bb162778d6f298490977c` (= published .sha256) |

All four are GPL-2.0-or-later as built here (FFmpeg with `--enable-gpl`,
which its DVD-Video demuxer requires).

## How they are built

`cargo build` builds everything; see the `crates/*-sys` build scripts.

- libdvdcss, libdvdread, libdvdnav: compiled with the `cc` crate; the
  `config.h` each one's `meson.build` would generate on Linux is written by
  the build script. libdvdread links libdvdcss directly (no `dlopen`).
- FFmpeg: its own `configure` + `make`, out of tree in Cargo's build folder,
  static libraries, autodetection off (every external library is enabled by
  name), DVD-Video demuxer on our libdvdnav / libdvdread. The `ffmpeg` and
  `ffprobe` programs are copied to `target/<profile>/` for testing.

## Our changes

Every change to a file that comes from upstream is listed here (newest last).
Format-specific copies of FFmpeg components (`dvdvideo_*`, `hddvd_*`,
`bluray_*`) are listed with the file they were copied from.

| # | File | Change | Why | Test |
|---|---|---|---|---|
| 1 | `ffmpeg/libavcodec/parser.c` (in place) | `av_parser_parse2` looks the next frame's timestamp up at that frame's real start (`next_frame_offset`), not at `cur_offset` | When a parser returns a negative index (the frame began in data buffered from the previous packet, e.g. an AC-3 header split across two packets), the frame got the NEXT packet's PTS, which belongs to the first frame starting in that packet (ISO/IEC 13818-1 2.4.3.7); the frame after it then got the same PTS. On DVDs whose audio packs split AC-3 headers this gave one frame in 21 a timestamp 32 ms late plus a duplicate, and the DVD-Video demuxer's AC-3 check then dropped every duplicate (audio drifting ahead of video) | `crates/ffmpeg-sys/tests/parser_timestamps.rs` (fails without the change) |
