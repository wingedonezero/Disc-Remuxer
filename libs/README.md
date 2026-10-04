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
which its DVD-Video demuxer requires). Our FFmpeg also holds a UDF reader
derived from Linux fs/udf (`libavformat/discio_udf_linux.c`), which is
GPL-2.0-only, so the program as built is GPL-2.0-only. The UDF reader derived
from NetBSD (`discio_udf_netbsd.c`) keeps NetBSD's BSD licence notice.

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
| 2 | `libdvdnav/src/vm/decoder.[ch]`, `vm.c`, `play.c` | VM commands are evaluated with their VM (`vmEval_CMD(..., vm, ...)`) instead of only its registers; no behaviour change | the next change needs the VM while a command runs | (no behaviour change; corpus dump identical) |
| 3 | `libdvdnav/src/vm/*.c`, `navigation.c`, `dvdnav/dvdnav.h` | every `assert()` the VM makes about the disc's navigation data records the failure on the VM (count + first `file:line: condition`), logs it, stops the VM and leaves on a safe path; new `dvdnav_get_vm_failures()`; the `DVDNAV_STRICT` checks stay asserts | built as we build it (asserts on), a malformed disc aborted the whole program, the DVD-Video demuxer included; built with NDEBUG the VM would carry on, in places reading past the end of a table | `crates/libdvdnav-sys/tests/vm_failures.rs` (aborts with the upstream assert) |
| 4 | `ffmpeg/libavformat/dvdvideodec.c` -> `dvdvideodec.c` + `dvdvideo_internal.h`, `dvdvideo_ifo.c`, `dvdvideo_play.c`, `dvdvideo_chapters.c`, `dvdvideo_streams.c`; `libavformat/Makefile` | the DVD-Video demuxer split into helper files by whole functions; functions used across files get the `ff_` prefix; no behaviour change | room for the DVD orchestrator's new parts (scan, title plan, reading layer, joiner) in their own files | corpus dump identical before / after |
| 5 | `ffmpeg/libavformat/discio.h`, `discio_read.c` (new, ours); `libavformat/Makefile` | disc I/O shared by the disc demuxers: sources (host files, test sources) and 2048-byte block reads with retries (attempt count, 0 = 1), block by block after the first retry, 0xFF completion of a source's last partial block, empty / partial-block reads as failed attempts, more-than-asked = error; every attempt, retry, padding and final failure logged | the disc readers live in our FFmpeg (step P1) | `crates/ffmpeg-sys/tests/discio_read.rs` |
| 6 | `ffmpeg/libavformat/discio_fs.c`, `discio_iso9660.c` (new, ours); `discio.h` | file-system interface shared by all disc file systems (open file by path -> size + sector extents, list a directory, label; file reads over extents; path walking), and the ISO 9660 / Joliet reader: first primary / supplementary volume descriptor of sectors 16-127 (CD001, 2048-byte blocks), labels (Windows-1252 / Latin-1, UCS-2) within 161 bytes, directories read whole (33 bytes - 1 MiB), exact name lookup with or without `;1`, one extent per file; known limits listed in the file | the disc readers live in our FFmpeg (step P1) | `crates/ffmpeg-sys/tests/discio_iso9660.rs` (built images; corpus check with `DISCIO_CORPUS`) |
| 7 | `ffmpeg/libavformat/discio_udf_netbsd.c` (new; derived from NetBSD sys/fs/udf + sys/kern/vfs_dirhash.c, BSD notice kept) | UDF reader, NetBSD-based (the default): the read side of NetBSD's driver adapted to disc images; compatibility rules as named switches (unclosed volumes read, 250 allocation extent descriptors, directory entries with a bad CRC used when their length is consistent, Windows name rules, stream directory not loaded, hidden entries listed, embedded file data not opened, one extent per allocation descriptor); structural guards against endless loops (never time-based); undefined behaviour of the C made errors; every case listed in the file header | the disc readers live in our FFmpeg (step P1) | `crates/ffmpeg-sys/tests/discio_udf_netbsd.rs` (35 tests; corpus: `DISCIO_UDF_REFERENCE`) |
| 8 | `ffmpeg/libavformat/discio_udf_linux.c` (new; derived from Linux fs/udf, GPL-2.0-only notice kept) | UDF reader, Linux-based (selectable): the read side of Linux's driver mounted read-only with default options, adapted to disc images; adjacent pieces joined, embedded data returned in memory; bounds checks where the C would read past a buffer; blocks past a file's allocation mapped in one step; every case listed in the file header | as above | `crates/ffmpeg-sys/tests/discio_udf_linux.rs` (19 tests; corpus: `DISCIO_UDF_REFERENCE`) |
| 9 | `discio.h`, `discio_fs.c`, `discio_iso9660.c` | label room 260 bytes (UDF labels decode up to 258; the ISO 9660 reader keeps its own 161-byte rule); `DISCIO_SECTOR_NOT_RECORDED` runs read as zeros; each reader's path rules documented | the two UDF readers | `crates/ffmpeg-sys/tests/discio_read.rs` |
