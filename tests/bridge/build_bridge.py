"""Builds the bridge the unit tests call our C code through.

cffi compiles bridge.c (our headers + a few test helpers) with the
declarations in cdef/ (read in name order) into the module _bridge, linked
with the static libraries of one build mode. The C compiler checks every
declaration against our headers: structs are declared with only the fields
the tests use and "...;", constants as "...", so cffi takes the layout and
the values from the compiler, and a wrong declaration fails the build.

    build_bridge.py <output folder>

BRIDGE_CFLAGS and BRIDGE_LIBS (environment) come from mk/test.mk.
"""

import os
import pathlib
import shlex
import sys

from cffi import FFI

HERE = pathlib.Path(__file__).resolve().parent


def static_libs(flags):
    """-lNAME as the path of libNAME.a when one of the -L folders holds it:
    the link line setuptools builds names the system library folder before
    ours, where another FFmpeg (or DVD library) may be installed."""
    dirs = [f[2:] for f in flags if f.startswith("-L")]
    out = []
    for f in flags:
        if f.startswith("-l"):
            found = [pathlib.Path(d) / f"lib{f[2:]}.a" for d in dirs]
            found = [p for p in found if p.exists()]
            if found:
                out.append(str(found[0]))
                continue
        if not f.startswith("-L"):
            out.append(f)
    return out


def main():
    out = pathlib.Path(sys.argv[1])
    ffi = FFI()
    for f in sorted((HERE / "cdef").glob("*.cdef")):
        ffi.cdef(f.read_text(), override=False)
    ffi.set_source(
        "_bridge",
        (HERE / "bridge.c").read_text(),
        extra_compile_args=shlex.split(os.environ["BRIDGE_CFLAGS"])
        + ["-g", "-Werror=incompatible-pointer-types", "-Werror=int-conversion",
           "-Werror=implicit-function-declaration", "-Wno-unused-function"],
        # The python executable links the system's libexpat: without these
        # its symbols would take the place of our expat's inside the bridge.
        extra_link_args=["-Wl,-Bsymbolic", "-Wl,--exclude-libs,ALL"]
        + static_libs(shlex.split(os.environ["BRIDGE_LIBS"])),
    )
    ffi.compile(tmpdir=str(out))


if __name__ == "__main__":
    main()
