#!/bin/sh
# Builds disc-remuxer and every library it uses (all from libs/, linked in).
#
#   ./build.sh            debug build    -> dist/debug/
#   ./build.sh release    release build  -> dist/release/
#   ./build.sh clean      removes build/ and dist/
#
# Only the finished programs land in dist/<mode>/; everything else stays in
# build/<mode>/. Running it again rebuilds only what changed.
set -e
cd "$(dirname "$0")"

mode=${1:-debug}
case $mode in
    debug|release|clean) ;;
    *) echo "usage: $0 [debug|release|clean]" >&2; exit 2 ;;
esac

if [ "$mode" != clean ]; then
    missing=
    for tool in cc make nasm pkg-config cmake rsync; do
        command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
    done
    if [ -n "$missing" ]; then
        echo "build.sh: missing tools:$missing" >&2
        exit 1
    fi
fi

exec make "$mode"
