"""libdvdcss handles reading a host file through its stream callbacks."""

from helpers import extern, ffi, lib


@extern
def tb_py_css_seek(p, pos):
    try:
        ffi.from_handle(p).seek(pos)
        return 0
    except OSError:
        return -1


@extern
def tb_py_css_read(p, buf, length):
    try:
        data = ffi.from_handle(p).read(length)
    except OSError:
        return -1
    ffi.memmove(buf, data, len(data))
    return len(data)


@extern
def tb_py_css_log(p_log, level, fmt):
    ffi.from_handle(p_log).append((level, ffi.string(fmt).decode("utf-8", "replace")))


class Handle:
    """dvdcss_open_stream_uncached() on a host file; log: a list that gets
    (level, format) of every message, or None for no log."""

    def __init__(self, path, log=None):
        self.file = open(path, "rb")
        self._file = ffi.new_handle(self.file)
        self.cb = ffi.new("dvdcss_stream_cb *", {"pf_seek": lib.tb_py_css_seek, "pf_read": lib.tb_py_css_read})
        self._log = ffi.new_handle(log) if log is not None else ffi.NULL
        self.css = lib.tb_dvdcss_open_stream_uncached(self._file, self.cb, int(log is not None), self._log)
        assert self.css != ffi.NULL

    def close(self):
        if self.css is not None:
            lib.dvdcss_close(self.css)
            self.css = None
            self.file.close()

    def __del__(self):
        self.close()


def dvdcss_read(path, start, n):
    """libdvdcss's own decrypting read of n blocks from block start of path."""
    h = Handle(path)
    out = ffi.new("uint8_t[]", n * 2048)
    assert lib.dvdcss_seek(h.css, start, lib.DVDCSS_SEEK_KEY) == start
    got = lib.dvdcss_read(h.css, out, n, lib.DVDCSS_READ_DECRYPT)
    h.close()
    assert got == n
    return bytes(out)
