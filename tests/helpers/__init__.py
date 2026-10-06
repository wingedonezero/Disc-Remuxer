"""What every test uses: the build paths, the bridge into our C code (ffi,
lib) and FFmpeg's log."""

import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODE = os.environ.get("DR_MODE", "debug")
BUILD = ROOT / "build" / MODE
DIST = ROOT / "dist" / MODE

sys.path.insert(0, str(BUILD / "pytest"))
from _bridge import ffi, lib  # noqa: E402  (built by make test)

# cffi compares a struct's declaration with the compiler's layout the first
# time the struct is used: use every one now, so a wrong declaration fails
# every run rather than only the test that happens to touch it.
_typedefs, _structs, _unions = ffi.list_types()
for _name in _typedefs + [f"struct {n}" for n in _structs] + [f"union {n}" for n in _unions]:
    try:
        ffi.sizeof(_name)
    except ffi.error as e:  # declared only by name (opaque): nothing to check
        if "incomplete" not in str(e) and "opaque" not in str(e):
            raise

AV_NOPTS_VALUE = -(1 << 63)
EIO = 5
EINVAL = 22
ENOENT = 2


def averror(errno):
    """AVERROR(errno)."""
    return -errno


# ---- FFmpeg's log, collected per line ----

LOG = []


@ffi.def_extern()
def tb_py_log(level, line):
    LOG.append(ffi.string(line).decode("utf-8", "replace"))


def install_log(level=None):
    """Collect FFmpeg's log lines (and the libraries' through it) into LOG,
    emptied first."""
    LOG.clear()
    lib.tb_log_install(lib.tb_py_log, lib.AV_LOG_TRACE if level is None else level)


def logged(part):
    """Whether a collected log line contains part."""
    return any(part in line for line in LOG)


def error_text(code):
    buf = ffi.new("char[]", 256)
    lib.av_strerror(code, buf, 256)
    return ffi.string(buf).decode()
