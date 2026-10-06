"""libavformat/discio_iso9660.c: ISO 9660 / Joliet volumes, built here byte
by byte (ECMA-119), and the corpus images when DISCIO_CORPUS names the
folder holding them."""

import os
import pathlib
import struct

import pytest

from helpers import ENOENT, averror, ffi, lib
from helpers.disc import Image, env_or_skip, free_file
from helpers.source import list_dir

INVALIDDATA = -0x41444E49  # FFERRTAG('I','N','D','A')


# ---- building images ----

def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def record(ident, loc, length, flags):
    """One directory record."""
    r = bytearray(33 + len(ident) + (1 if len(ident) % 2 == 0 else 0))
    r[0] = len(r)
    r[2:10] = both32(loc)
    r[10:18] = both32(length)
    r[25] = flags
    r[28:32] = both16(1)
    r[32] = len(ident)
    r[33:33 + len(ident)] = ident
    return bytes(r)


def ucs2(s):
    return s.encode("utf-16-be")


def descriptor(kind, label, root):
    """A volume descriptor: type, label field (32 bytes) and root record."""
    d = bytearray(2048)
    d[0] = kind
    d[1:6] = b"CD001"
    d[6] = 1
    d[0x28:0x48] = label
    d[0x80:0x84] = both16(2048)
    d[156:156 + len(root)] = root
    d[813:829] = b"2004030112000000"
    return d


IFO_SIZE = 3000


def image():
    """Sectors: 16 PVD, 17 SVD, 18 terminator, 20/21 root (ISO/Joliet),
    22/23 VIDEO_TS (ISO/Joliet), 24-25 VIDEO_TS.IFO data."""
    img = bytearray(26 * 2048)

    def at(s):
        return s * 2048

    def put(s, data):
        img[at(s):at(s) + len(data)] = data

    def dot(loc):
        return record(b"\0", loc, 2048, 2)

    def dotdot(loc):
        return record(b"\1", loc, 2048, 2)

    put(20, dot(20) + dotdot(20) + record(b"VIDEO_TS", 22, 2048, 2))
    put(22, dot(22) + dotdot(20) + record(b"VIDEO_TS.IFO;1", 24, IFO_SIZE, 0)
        + record(b"README.TXT;1", 25, 10, 1))  # hidden flag
    put(21, dot(21) + dotdot(21) + record(ucs2("VIDEO_TS"), 23, 2048, 2))
    put(23, dot(23) + dotdot(21) + record(ucs2("VIDEO_TS.IFO;1"), 24, IFO_SIZE, 0))
    put(24, bytes(i % 253 for i in range(IFO_SIZE)))
    label = bytearray(b" " * 32)
    label[:9] = b"TEST_DISC"
    label[9] = 0x80  # Windows-1252: euro sign
    put(16, descriptor(1, label, record(b"\0", 20, 2048, 2)))
    j = ucs2("Joliet Disc")
    jlabel = bytearray(32)
    jlabel[:len(j)] = j
    for k in range(len(j), 32, 2):
        jlabel[k:k + 2] = struct.pack(">H", 0x20)
    put(17, descriptor(2, jlabel, record(b"\0", 21, 2048, 2)))
    img[at(18)] = 0xff
    img[at(18) + 1:at(18) + 6] = b"CD001"
    return img


# ---- running the reader ----

class Iso:
    def __init__(self, tmp_path, data):
        path = tmp_path / "image.iso"
        path.write_bytes(bytes(data))
        self.img = Image.open(path)

    def mount(self, joliet):
        """A Vol, or the negative error code."""
        fs = ffi.new("DiscIOFS **")
        ret = lib.ff_discio_iso9660_mount(self.img.ptr, int(joliet), fs)
        if ret < 0:
            return ret
        from helpers.disc import Vol
        return Vol(fs[0], self.img)


def open_(v, path):
    """(size, [(sector, count)]) or the negative error code."""
    ret, f = v.open_file(path)
    if ret < 0:
        return ret
    out = (f.size, [(f.extents[i].sector, f.extents[i].count) for i in range(f.nb_extents)])
    free_file(f)
    return out


def read(v, path, pos, length):
    ret, f = v.open_file(path)
    assert ret == 0
    ret, data = v.read_file(f, pos, length)
    assert ret == 0
    free_file(f)
    return data


def listing(v, path):
    ret, entries = list_dir(v.fs, path)
    return ret if ret < 0 else entries


# ---- tests on built images ----

def test_iso9660_names_extents_data_and_label(tmp_path):
    v = Iso(tmp_path, image()).mount(False)
    assert v.label() == "TEST_DISC€"
    assert open_(v, "/VIDEO_TS/VIDEO_TS.IFO") == (3000, [(24, 2)])
    # separators: '\' too, empty components skipped
    assert open_(v, "\\VIDEO_TS\\VIDEO_TS.IFO")[0] == 3000
    assert open_(v, "//VIDEO_TS//VIDEO_TS.IFO")[0] == 3000
    assert open_(v, "/VIDEO_TS/VIDEO_TS.IFO;1")[0] == 3000
    assert open_(v, "/VIDEO_TS/README.TXT")[0] == 10, "hidden files are found"
    assert open_(v, "/VIDEO_TS/NOPE.IFO") == averror(ENOENT)
    assert open_(v, "/AUDIO_TS/X") == averror(ENOENT)
    assert open_(v, "/VIDEO_TS") == averror(ENOENT), "a directory is not a file"
    data = read(v, "/VIDEO_TS/VIDEO_TS.IFO", 1000, 1500)
    assert all(b == (i + 1000) % 253 for i, b in enumerate(data))
    assert listing(v, "/VIDEO_TS") == [(".", True), ("..", True), ("VIDEO_TS.IFO", False), ("README.TXT", False)]
    created = ffi.new("char[15]")
    assert lib.ff_discio_iso9660_creation_date(v.fs, created) == 0
    assert ffi.string(created) == b"20040301120000"


def test_joliet_names_and_label(tmp_path):
    v = Iso(tmp_path, image()).mount(True)
    assert v.label() == "Joliet Disc"
    assert open_(v, "/VIDEO_TS/VIDEO_TS.IFO") == (3000, [(24, 2)])
    assert listing(v, "/")[2] == ("VIDEO_TS", True)


def append_records(img, sector, recs):
    """Appends recs after the last record of the directory at sector."""
    off = sector * 2048
    while img[off] != 0:
        off += img[off]
    for r in recs:
        img[off:off + len(r)] = r
        off += len(r)


def test_a_name_starting_with_nul_is_empty_and_only_a_name_of_no_characters_is_x(tmp_path):
    img = image()
    append_records(img, 22, [record(b"\0A", 25, 1, 0), record(b"", 25, 1, 0)])
    append_records(img, 23, [record(b"\0\0\0A", 25, 1, 0), record(b"", 25, 1, 0)])
    iso = Iso(tmp_path, img)
    for joliet in [False, True]:
        names = [e[0] for e in listing(iso.mount(joliet), "/VIDEO_TS")]
        assert names[-2:] == ["", "x"], f"joliet {joliet}: {names}"


def test_corrupt_and_missing_structures(tmp_path):
    # zero record length at a directory's sector start: corrupt
    bad = image()
    bad[22 * 2048] = 0
    (tmp_path / "a").mkdir()
    v = Iso(tmp_path / "a", bad).mount(False)
    assert listing(v, "/VIDEO_TS") == INVALIDDATA
    assert open_(v, "/VIDEO_TS/VIDEO_TS.IFO") == averror(ENOENT)

    # no CD001 in the primary descriptor
    bad = image()
    bad[16 * 2048 + 1] = ord("X")
    (tmp_path / "b").mkdir()
    assert isinstance(Iso(tmp_path / "b", bad).mount(False), int)

    # Joliet without a supplementary descriptor
    bad = image()
    bad[17 * 2048] = 0xff
    (tmp_path / "c").mkdir()
    iso = Iso(tmp_path / "c", bad)
    assert isinstance(iso.mount(True), int)
    assert not isinstance(iso.mount(False), int)


def test_a_read_failure_after_the_primary_descriptor_keeps_the_volume(tmp_path):
    # the image ends after sector 16 (PVD) + 4 more sectors: sector 21+ unreadable
    cut = image()
    cut[17 * 2048] = 0  # no SVD, no terminator: the scan reads on
    cut[18 * 2048] = 0
    del cut[21 * 2048:]
    v = Iso(tmp_path, cut).mount(False)
    assert v.label() == "TEST_DISC€"


# ---- real discs: our ISO 9660 vs libdvdread's UDF, labels vs the reference ----

# Labels the reference program gives the corpus images it reads through
# ISO 9660 (archived corpus run of the file-system choice, 16 of 16 matching).
ISO_LABELS = {
    "AVBA_14083.iso": "AVBA_14083",
    "SAINTBEAST_OVA1_1.ISO": "ORS_1003",
    "SPACE_SYMPHONY_MAETEL_1.iso": "SPACE_SYMPHONY_MAETEL_1",
    "LABYRINTH_1.iso": "LABYRINTH_1",
    "LABYRINTH_2.iso": "LABYRINTH_2",
    "THE_THING_1998_DVD.ISO": "THING",
}


def images(folder):
    out = []
    for p in sorted(pathlib.Path(folder).rglob("*")):
        if p.suffix.lower() == ".iso":
            if p.exists():
                out.append(p)
            else:
                print(f"{p}: link to a missing file (drive not mounted?), skipped")
    return out


@pytest.mark.disc
def test_corpus_iso9660_matches_udf_extents_and_reference_labels():
    folder = env_or_skip("DISCIO_CORPUS")
    found = images(folder)
    assert found, f"no .iso under {folder}"
    checked_files, checked_labels, label_names = 0, 0, set()
    for path in found:
        img = Image.open(path)
        fs = ffi.new("DiscIOFS **")
        if lib.ff_discio_iso9660_mount(img.ptr, 0, fs) < 0:
            print(f"{path}: no ISO 9660 file system")
            continue
        from helpers.disc import Vol
        v = Vol(fs[0], img)
        name = path.name
        if name in ISO_LABELS:
            assert v.label() == ISO_LABELS[name], f"{name}: label"
            checked_labels += 1
            label_names.add(name)
        dvd = lib.DVDOpen(str(path).encode())
        assert dvd != ffi.NULL, f"{name}: libdvdread cannot open it"
        files = ["/VIDEO_TS/VIDEO_TS.IFO"] + [f"/VIDEO_TS/VTS_{n:02}_0.IFO" for n in range(1, 100)]
        for f in files:
            size = ffi.new("uint32_t *")
            start = lib.UDFFindFile(dvd, f.encode(), size)
            ours = open_(v, f)
            if start == 0:
                if f.endswith("_0.IFO") and isinstance(ours, int):
                    break  # past the last title set
                continue
            assert not isinstance(ours, int), f"{name}: {f} missing in ISO 9660 ({ours})"
            osize, ext = ours
            assert (ext[0][0], osize) == (start, size[0]), f"{name}: {f}"
            checked_files += 1
        lib.DVDClose(dvd)
    print(f"corpus: {len(found)} images, {checked_files} IFO files equal to libdvdread's UDF, "
          f"{checked_labels} labels = the reference")
    # (the corpus holds AVBA_14083.iso twice: a known duplicate)
    assert len(label_names) == len(ISO_LABELS), "every reference label checked"
