"""The HD DVD demuxer's parts on built images: the title plan, a disc opened
as the demuxer opens it (VTI, playlists, marks, AACS, title plan)."""

from helpers import extern, ffi, lib
from helpers.source import Memory, Source


def take_text(p):
    """A string the C code allocated (freed here)."""
    s = ffi.string(p).decode()
    lib.av_free(p)
    return s


class Opened:
    """A disc opened as the demuxer opens it; open() / open_file() build one.
    error: the AACS open's error code (the rest is then not opened)."""

    def __init__(self, src, key_files):
        self._src = src
        self.ptr = src.ptr if isinstance(src, Source) else src
        self.fs = ffi.new("DiscIOFS **")
        self.vti = ffi.new("HDDVDVTI **")
        self.xs = ffi.new("HDDVDXpl ***")
        self.nb = ffi.new("int *")
        self.aacs = ffi.new("HDDVDAACS **")
        self.plan = ffi.new("HDDVDTitlePlan **")
        self.error = 0
        files = [ffi.new("char[]", f.encode()) for f in key_files]
        ptrs = ffi.new("const char *[]", files) if files else ffi.NULL
        assert lib.ff_discio_mount_image(self.ptr, ffi.NULL, self.fs) == 0
        assert lib.ff_hddvd_vti_open(ffi.NULL, self.fs[0], self.vti) == 0
        assert lib.ff_hddvd_xpl_load(ffi.NULL, self.fs[0], self.xs, self.nb) == 0
        lib.ff_hddvd_evob_marks(ffi.NULL, self.vti[0], self.xs[0], self.nb[0])
        ret = lib.ff_hddvd_aacs_open(ffi.NULL, self.fs[0], ptrs, len(files), self.nb[0], self.aacs)
        if ret < 0:
            self.error = ret
            return
        assert lib.ff_hddvd_titles_plan(ffi.NULL, self.fs[0], self.vti[0], self.xs[0], self.nb[0], 0, self.plan) == 0

    def close(self):
        if self.fs is None:
            return
        lib.ff_hddvd_titles_free(self.plan)
        lib.ff_hddvd_aacs_close(self.aacs)
        lib.ff_hddvd_xpl_free_all(self.xs, self.nb[0])
        lib.ff_hddvd_vti_free(self.vti)
        lib.ff_discio_fs_close(self.fs)
        if isinstance(self._src, Source):
            self._src.close()
        else:
            lib.ff_discio_source_free(ffi.new("DiscIOSource **", self._src))
        self.fs = None

    def __del__(self):
        self.close()

    def tracks(self):
        """Build every title's tracks (titles that cannot be used leave the
        plan), then the plan as text, or the negative error code."""
        ret = lib.ff_hddvd_tracks_build(ffi.NULL, self.fs[0], self.aacs[0], self.vti[0], self.xs[0], self.nb[0],
                                        self.plan[0])
        return ret if ret < 0 else take_text(lib.ff_hddvd_titles_dump(self.plan[0]))

    def block(self, slot, block):
        """Block of EVOB slot's stream, made usable: (result, bytes)."""
        buf = ffi.new("uint8_t[]", 2048)
        c = lib.ff_hddvd_titles_clip(self.plan[0], slot)
        assert c != ffi.NULL, f"no clip for slot {slot}"
        r = lib.ff_hddvd_clip_block(ffi.NULL, self.aacs[0], self.fs[0], c, block, buf)
        return r, bytes(buf)


def open_image(im, key_files=()):
    """Open a built image (an Opened; check .error)."""
    return Opened(Source(Memory(im.d), name="memory"), list(key_files))


def open_file(path, key_files=()):
    """Open an image file (an Opened; check .error)."""
    p = ffi.new("DiscIOSource **")
    assert lib.ff_discio_source_open_file(ffi.NULL, str(path).encode(), p) == 0
    return Opened(p[0], list(key_files))


def plan(im, min_length):
    """The title plan of a built image: its dump, or the negative error code."""
    src = Source(Memory(im.d), name="memory")
    fs = ffi.new("DiscIOFS **")
    vti = ffi.new("HDDVDVTI **")
    xs = ffi.new("HDDVDXpl ***")
    nb = ffi.new("int *")
    p = ffi.new("HDDVDTitlePlan **")
    assert lib.ff_discio_mount_image(src.ptr, ffi.NULL, fs) == 0
    assert lib.ff_hddvd_vti_open(ffi.NULL, fs[0], vti) == 0
    assert lib.ff_hddvd_xpl_load(ffi.NULL, fs[0], xs, nb) == 0
    ret = lib.ff_hddvd_titles_plan(ffi.NULL, fs[0], vti[0], xs[0], nb[0], min_length, p)
    if ret < 0:
        assert p[0] == ffi.NULL
        out = ret
    else:
        out = take_text(lib.ff_hddvd_titles_dump(p[0]))
    lib.ff_hddvd_titles_free(p)
    lib.ff_hddvd_xpl_free_all(xs, nb[0])
    lib.ff_hddvd_vti_free(vti)
    lib.ff_discio_fs_close(fs)
    src.close()
    return out


@extern
def tb_py_hddvd_read(opaque, pos, buf, length):
    """HDDVDReadFn over a Python function read(pos, length) -> exactly
    length bytes, or a negative error code."""
    r = ffi.from_handle(opaque)(pos, length)
    if isinstance(r, int):
        return r
    assert len(r) == length
    ffi.memmove(buf, r, length)
    return 0
