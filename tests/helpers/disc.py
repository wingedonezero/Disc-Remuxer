"""Disc images as sources (in memory or a host file) and the file systems
mounted on them."""

import os

import pytest

from helpers import EIO, averror, error_text, ffi, lib
from helpers.source import Source, list_dir

S = 2048


class BadSectors:
    """An image in memory; reads touching a sector in bad fail with EIO."""

    def __init__(self, data, bad=()):
        self.data = bytes(data)
        self.bad = set(bad)

    def read_at(self, pos, length):
        first, end = pos // S, (pos + length + S - 1) // S
        if any(s in self.bad for s in range(first, end)):
            return averror(EIO)
        return self.data[pos:pos + length]


class Image:
    """A disc image as a source: memory(im) for a built Img, open(path) for
    a host file."""

    def __init__(self, ptr, keep=None):
        self.ptr = ptr
        self._keep = keep

    @classmethod
    def memory(cls, im):
        src = Source(BadSectors(im.d, im.bad), name="memory")
        return cls(src.ptr, keep=src)

    @classmethod
    def open(cls, path):
        p = ffi.new("DiscIOSource **")
        ret = lib.ff_discio_source_open_file(ffi.NULL, str(path).encode(), p)
        assert ret == 0, f"opening {path}: {error_text(ret)}"
        return cls(p[0])

    def close(self):
        if self._keep is not None:
            self._keep.close()
            self._keep = None
        elif self.ptr != ffi.NULL:
            lib.ff_discio_source_free(ffi.new("DiscIOSource **", self.ptr))
        self.ptr = ffi.NULL

    def __del__(self):
        self.close()

    def mount(self, opts=None):
        """ff_discio_mount_image() (opts: (udf_reader, prefer_iso_for_old_udf102)
        or None for the defaults): a Vol, or the negative error code."""
        fs = ffi.new("DiscIOFS **")
        o = ffi.NULL if opts is None else ffi.new("DiscIOImageOptions *", list(opts))
        ret = lib.ff_discio_mount_image(self.ptr, o, fs)
        if ret < 0:
            return ret
        return Vol(fs[0], self)

    def mount_one(self, kind):
        """One file system mounted directly: "iso", "joliet", "netbsd" or
        "linux"."""
        fs = ffi.new("DiscIOFS **")
        ret = self.mount_ret(kind, fs)
        assert ret == 0, f"mounting {kind}: {error_text(ret)}"
        return Vol(fs[0], self)

    def mount_ret(self, kind, fs):
        if kind == "iso":
            return lib.ff_discio_iso9660_mount(self.ptr, 0, fs)
        if kind == "joliet":
            return lib.ff_discio_iso9660_mount(self.ptr, 1, fs)
        if kind == "netbsd":
            return lib.ff_discio_udf_netbsd_mount(self.ptr, fs)
        return lib.ff_discio_udf_linux_mount(self.ptr, fs)


class Vol:
    """A mounted file system with its image (closed before the image)."""

    def __init__(self, fs, image):
        self.fs = fs
        self.image = image

    def close(self):
        if self.fs != ffi.NULL:
            lib.ff_discio_fs_close(ffi.new("DiscIOFS **", self.fs))
            self.fs = ffi.NULL

    def __del__(self):
        self.close()

    def name(self):
        return ffi.string(self.fs.ops.name).decode()

    def kind(self):
        return self.fs.ops.kind

    def label(self):
        return ffi.string(self.fs.label).decode("utf-8", "replace")

    def label_bytes(self):
        return ffi.string(self.fs.label)

    def check(self):
        r = lib.ff_discio_dvd_video_check(self.fs)
        assert r in (0, 1), f"check returned {r}"
        return r == 1

    def format(self):
        return lib.ff_discio_disc_format(self.fs)

    def find_dir(self, path):
        return self.fs.ops.find_dir(self.fs, path.encode())

    def open_file(self, path):
        """(return code, DiscIOFile pointer or NULL); free with free_file().
        path: str or bytes."""
        out = ffi.new("DiscIOFile **")
        if isinstance(path, str):
            path = path.encode()
        ret = self.fs.ops.open_file(self.fs, path, out)
        return ret, out[0]

    def open(self, path):
        """An OpenFile, or the negative error code."""
        ret, f = self.open_file(path)
        return ret if ret < 0 else OpenFile(self, f)

    def list(self, path, raw=False):
        """[(name, is_dir)] in directory order, or the negative error code."""
        ret, entries = list_dir(self.fs, path, raw)
        return ret if ret < 0 else entries

    def names(self, path):
        entries = self.list(path)
        assert not isinstance(entries, int), f"listing {path}: {entries}"
        return [n for n, _ in entries]

    def revision(self):
        return self.fs.udf_revision

    def year(self):
        t = self.fs.udf_recording_time
        return t[2] | t[3] << 8

    def read_file(self, f, pos, length):
        """(return code, bytes) of ff_discio_file_read()."""
        b = bytearray(length)
        ret = lib.ff_discio_file_read(self.fs, f, pos, ffi.from_buffer(b), length)
        return ret, bytes(b)


class OpenFile:
    """An open DiscIOFile of a Vol (freed with it)."""

    def __init__(self, vol, f):
        self.vol = vol
        self.f = f

    def __del__(self):
        if self.f != ffi.NULL:
            free_file(self.f)
            self.f = ffi.NULL

    def size(self):
        return self.f.size

    def extents(self):
        """[(sector, count)]"""
        return [(self.f.extents[i].sector, self.f.extents[i].count) for i in range(self.f.nb_extents)]

    def read(self, pos, length):
        """bytes, or the negative error code."""
        ret, data = self.vol.read_file(self.f, pos, length)
        return ret if ret < 0 else data


def free_file(f):
    lib.ff_discio_file_free(ffi.new("DiscIOFile **", f))


def env_or_skip(*names, need="a real disc"):
    """The values of environment variables a test needs (a real disc, by
    default); skips the test (shown as SKIPPED) when one is not set."""
    values = [os.environ.get(n) for n in names]
    missing = [n for n, v in zip(names, values) if not v]
    if missing:
        pytest.skip(f"needs {need}: {', '.join(missing)} not set")
    return values[0] if len(values) == 1 else values
