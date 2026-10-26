#!/bin/sh
# Builds this FFmpeg's ffmpeg and ffprobe for our apps.
#
#   ./build.sh            debug build    -> dist/debug/
#   ./build.sh release    release build  -> dist/release/
#   ./build.sh clean      removes build/ and dist/
#
# Out of tree: objects and the install prefix stay in build/<mode>/; only
# ffmpeg and ffprobe land in dist/<mode>/. FFmpeg's own libraries are linked
# in statically; the external libraries in LIBS come from the host's -dev
# packages. Autodetection is off, so the build contains exactly that list and
# nothing that merely happens to be installed. configure runs again only when
# its arguments change; make rebuilds only what changed.
set -e
cd "$(dirname "$0")"
root=$(pwd)

mode=${1:-debug}
case $mode in
    debug|release) ;;
    clean) rm -rf build dist; exit 0 ;;
    *) echo "usage: $0 [debug|release|clean]" >&2; exit 2 ;;
esac

# pkg-config name : Debian package, one per external library we enable.
#   dvdnav/dvdread  DVD demuxer (dvdvideo)
#   rubberband      rubberband filter (PAL / linear drift correction)
#   soxr            -resampler soxr (analysis decodes)
#   zlib/bzip2/lzma compressed Matroska tracks and other compressed payloads
LIBS="dvdnav:libdvdnav-dev dvdread:libdvdread-dev rubberband:librubberband-dev
soxr:libsoxr-dev zlib:zlib1g-dev bzip2:libbz2-dev liblzma:liblzma-dev"

missing=
for tool in cc make nasm pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ -n "$missing" ]; then
    echo "build.sh: missing tools:$missing" >&2
    exit 1
fi
missing=
for entry in $LIBS; do
    pkg-config --exists "${entry%%:*}" 2>/dev/null || missing="$missing ${entry#*:}"
done
if [ -n "$missing" ]; then
    echo "build.sh: missing development packages:$missing" >&2
    echo "          (Debian / Ubuntu: sudo apt install$missing)" >&2
    exit 1
fi

build=$root/build/$mode
dist=$root/dist/$mode

args="--prefix=$build/install --enable-gpl --disable-autodetect
--enable-static --disable-shared --disable-doc --disable-ffplay
--enable-zlib --enable-bzlib --enable-lzma --enable-iconv
--enable-libdvdnav --enable-libdvdread --enable-librubberband --enable-libsoxr"
[ "$mode" = debug ] && args="$args --disable-stripping"
args=$(echo $args)

mkdir -p "$build/obj" "$dist"
if [ ! -f "$build/obj/ffbuild/config.mak" ] || [ "$(cat "$build/args" 2>/dev/null)" != "$args" ] \
        || [ configure -nt "$build/obj/ffbuild/config.mak" ]; then
    # shellcheck disable=SC2086  # args is a word list on purpose
    (cd "$build/obj" && "$root/configure" $args)
    printf '%s' "$args" > "$build/args"
fi

make -C "$build/obj" -j"$(nproc 2>/dev/null || echo 4)"
make -C "$build/obj" install >/dev/null
cp -f "$build/install/bin/ffmpeg" "$build/install/bin/ffprobe" "$dist/"

echo "build.sh: $mode build in $dist"
"$dist/ffmpeg" -hide_banner -version | head -1
