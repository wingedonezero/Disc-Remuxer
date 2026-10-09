#!/bin/sh
# Builds disc-remuxer, its GUI disc-remuxer-gui and every library they use
# (all from libs/, linked in; the GUI's Qt 6 is the host's).
#
#   ./build.sh            debug build    -> dist/debug/
#   ./build.sh release    release build  -> dist/release/
#   ./build.sh test [pytest arguments]
#                         debug build, then every test (tests/); e.g.
#                         ./build.sh test -k vc1
#   ./build.sh clean      removes build/ and dist/
#
# Only the finished programs land in dist/<mode>/; everything else stays in
# build/<mode>/. Running it again rebuilds only what changed.
set -e
cd "$(dirname "$0")"

mode=${1:-debug}
case $mode in
    debug|release|clean) ;;
    test) shift ;;
    *) echo "usage: $0 [debug|release|test [pytest arguments]|clean]" >&2; exit 2 ;;
esac

if [ "$mode" != clean ]; then
    missing=
    tools="cc c++ make nasm pkg-config cmake rsync"
    [ "$mode" = test ] && tools="$tools python3"
    for tool in $tools; do
        command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
    done
    if [ -n "$missing" ]; then
        echo "build.sh: missing tools:$missing" >&2
        exit 1
    fi
    # the GUI links the host's Qt 6 (every other library is built from libs/)
    if ! pkg-config --exists Qt6Widgets 2>/dev/null; then
        echo "build.sh: missing the Qt 6 Widgets development files, which the GUI (disc-remuxer-gui) is built" >&2
        echo "          with (Debian / Ubuntu: qt6-base-dev; Fedora: qt6-qtbase-devel; Arch: qt6-base)" >&2
        exit 1
    fi
fi

[ "$mode" = test ] && exec make test PYTEST_ARGS="$*"
exec make "$mode"
