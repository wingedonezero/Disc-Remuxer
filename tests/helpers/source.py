"""DiscIOSource objects whose reads are answered by Python."""

from helpers import extern, ffi, lib

_OPS = ffi.new("DiscIOSourceOps *", {"read_at": lib.tb_py_read_at, "close": ffi.NULL})


@extern
def tb_py_read_at(opaque, pos, buf, length):
    reader = ffi.from_handle(opaque)
    data = reader.read_at(pos, length)
    if isinstance(data, int):
        return data
    ffi.memmove(buf, data, len(data))
    return len(data)


@extern
def tb_py_source_close(opaque):
    pass


@extern
def tb_py_dir_entry(opaque, name, is_dir):
    return ffi.from_handle(opaque)(ffi.string(name).decode("utf-8", "replace"), bool(is_dir))


@extern
def tb_py_dir_entry_raw(opaque, name, is_dir):
    return ffi.from_handle(opaque)(ffi.string(name), bool(is_dir))


class Source:
    """A DiscIOSource over reader.read_at(pos, length) -> bytes, or a
    negative AVERROR code. size: bytes (len(reader.data) when not given).
    Freed with close() or at the end of a with block."""

    def __init__(self, reader, size=None, name="test source"):
        self.reader = reader
        self._handle = ffi.new_handle(reader)
        if size is None:
            size = len(reader.data)
        self.ptr = lib.ff_discio_source_new(ffi.NULL, name.encode(), size, _OPS, self._handle)
        assert self.ptr != ffi.NULL

    def close(self):
        if self.ptr != ffi.NULL:
            p = ffi.new("DiscIOSource **", self.ptr)
            lib.ff_discio_source_free(p)
            self.ptr = ffi.NULL

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()


class Memory:
    """A reader over bytes in memory."""

    def __init__(self, data):
        self.data = bytes(data)

    def read_at(self, pos, length):
        return self.data[pos:pos + length]


def list_dir(fs, path, raw=False):
    """The entries of a directory of a mounted DiscIOFS: (return code,
    [(name, is_dir)]); names as bytes with raw, else decoded (UTF-8, bad
    bytes replaced). path: str or bytes."""
    entries = []

    def cb(name, is_dir):
        entries.append((name, is_dir))
        return 0

    h = ffi.new_handle(cb)
    if isinstance(path, str):
        path = path.encode()
    ret = fs.ops.list_dir(fs, path, lib.tb_py_dir_entry_raw if raw else lib.tb_py_dir_entry, h)
    return ret, entries
