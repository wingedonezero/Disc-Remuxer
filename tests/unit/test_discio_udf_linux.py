"""libavformat/discio_udf_linux.c: the UDF reader based on Linux fs/udf, on
small, fully valid images built here (volume recognition sequence, tag
locations, CRC lengths) and their damaged variants; and the corpus images
against reference dumps when DISCIO_UDF_REFERENCE names their folder."""

import collections
import hashlib
import os
import pathlib
import struct
import time

import pytest

from helpers import ENOENT, averror, ffi, lib
from helpers.disc import Image, Vol, env_or_skip

INVALIDDATA = -0x41444E49  # FFERRTAG('I','N','D','A')
S = 2048
PART_START = 300
PART_LEN = 219  # to the last sector of a 520-sector image


# ---- running the reader ----

def mount(im):
    """A Vol on the image (in memory), or the negative error code."""
    from synth.image import Img
    m = Img()
    m.d = bytearray(im.d)
    image = Image.memory(m)
    fs = ffi.new("DiscIOFS **")
    ret = lib.ff_discio_udf_linux_mount(image.ptr, fs)
    return ret if ret < 0 else Vol(fs[0], image)


def mounted(im):
    v = mount(im)
    assert not isinstance(v, int), f"mount failed: {v}"
    return v


def opened(v, path):
    """(size, [(sector, count)], embedded data or None), or the negative
    error code."""
    ret, f = v.open_file(path)
    if ret < 0:
        return ret
    from helpers.disc import free_file
    ext = [(f.extents[i].sector, f.extents[i].count) for i in range(f.nb_extents)]
    data = None if f.data == ffi.NULL else bytes(ffi.buffer(f.data, f.size))
    out = (f.size, ext, data)
    free_file(f)
    return out


def read_all(v, path, sink):
    """Reads the whole file in pieces of at most 1 MiB, handing each to
    sink; 0 or the negative error code."""
    ret, f = v.open_file(path)
    if ret < 0:
        return ret
    from helpers.disc import free_file
    try:
        size, pos = f.size, 0
        while pos < size:
            n = min(size - pos, 1 << 20)
            ret, data = v.read_file(f, pos, n)
            if ret < 0:
                return ret
            sink(data)
            pos += n
        return 0
    finally:
        free_file(f)


def read(v, path):
    out = bytearray()
    assert read_all(v, path, out.extend) == 0
    return bytes(out)


def raw_names(v, path):
    entries = v.list(path, raw=True)
    assert not isinstance(entries, int)
    return [n.decode() for n, _ in entries]


# ---- building images (ECMA-167 / OSTA UDF) ----

def crc16(data):
    """CRC-ITU-T (polynomial 0x1021, initial 0, MSB first)."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def put16(b, at, v):
    b[at:at + 2] = struct.pack("<H", v)


def put32(b, at, v):
    b[at:at + 4] = struct.pack("<I", v)


def put64(b, at, v):
    b[at:at + 8] = struct.pack("<Q", v)


def checksum(b):
    b[4] = 0
    b[4] = sum(x for i, x in enumerate(b[:16]) if i != 4) & 0xff


def tag(b, ident, loc, crclen):
    """A tag: id, version 2, tag location, CRC over crclen bytes, checksum."""
    put16(b, 0, ident)
    put16(b, 2, 2)
    put32(b, 12, loc)
    put16(b, 10, crclen)
    put16(b, 8, crc16(bytes(b[16:16 + crclen])))
    checksum(b)


def retag(b, loc):
    """The same tag with another tag location."""
    ident = struct.unpack_from("<H", b, 0)[0]
    crclen = struct.unpack_from("<H", b, 10)[0]
    tag(b, ident, loc, crclen)


def dstring(b, at, size, text):
    b[at] = 8
    b[at + 1:at + 1 + len(text)] = text.encode()
    b[at + size - 1] = 1 + len(text)


def osta(b, at):
    b[at] = 0
    b[at + 1:at + 24] = b"OSTA Compressed Unicode"


def n8(s):
    """An 8-bit OSTA name."""
    return b"\x08" + s.encode()


def fid(name, chars, icb, part):
    """One file identifier: (name as recorded, compression ID first; empty =
    none), characteristics, ICB block, ICB partition."""
    return (n8(name), chars, icb, part)


class Img:
    def __init__(self, sectors=520):
        self.d = bytearray(sectors * S)

    def sector(self, n):
        return memoryview(self.d)[n * S:(n + 1) * S]

    def put(self, absolute, data):
        self.d[absolute * S:absolute * S + len(data)] = data

    def file_entry(self, lbn, file_type, size, strategy, ads):
        """A file entry at partition block lbn with short ADs (len, lbn)."""
        b = bytearray(S)
        put16(b, 0x14, strategy)
        b[0x1b] = file_type
        put16(b, 0x30, 1)  # link count
        put64(b, 0x38, size)
        for i, (length, at) in enumerate(ads):
            put32(b, 0xb0 + 8 * i, length)
            put32(b, 0xb4 + 8 * i, at)
        put32(b, 0xac, 8 * len(ads))
        tag(b, 0x105, lbn, 176 + 8 * len(ads) - 16)
        self.put(PART_START + lbn, b)

    def directory(self, lbn, parent, fids):
        """Directory data at partition block lbn: parent entry, then fids."""
        out = bytearray()
        for name, chars, icb, part in [(b"", 0x0a, parent, 0)] + list(fids):
            length = (38 + len(name) + 3) & ~3
            b = bytearray(length)
            put16(b, 0x10, 1)
            b[0x12] = chars
            b[0x13] = len(name)
            put32(b, 0x14, 2048)
            put32(b, 0x18, icb)
            put16(b, 0x1c, part)
            b[0x26:0x26 + len(name)] = name
            tag(b, 0x101, lbn, length - 16)
            out += b
        self.put(PART_START + lbn, out)
        return len(out)


def volume(im, label, lvd_seq):
    """Volume structure: VRS (BEA01, NSR02, TEA01 at 16..18), anchor at 256,
    main VDS at 32 (PVD, IUVD, PD, LVD, TD), reserve at 48, LVID at 64."""
    for i, ident in enumerate([b"BEA01", b"NSR02", b"TEA01"]):
        v = im.sector(16 + i)
        v[1:6] = ident
        v[6] = 1
    a = im.sector(256)
    put32(a, 0x10, 16 * 2048)
    put32(a, 0x14, 32)
    put32(a, 0x18, 16 * 2048)
    put32(a, 0x1c, 48)
    tag(a, 2, 256, 496)
    p = im.sector(32)
    put16(p, 0x178 + 2, 2009)
    tag(p, 1, 32, 496)
    iu = im.sector(33)
    iu[0x15:0x21] = b"*UDF LV Info"
    put16(iu, 0x2c, 0x0102)
    tag(iu, 4, 33, 496)
    pd = im.sector(34)
    pd[0x14] = 1
    put16(pd, 0x16, 0)
    pd[0x19:0x1f] = b"+NSR02"
    put32(pd, 0xb8, 1)  # read-only access
    put32(pd, 0xbc, PART_START)
    put32(pd, 0xc0, PART_LEN)
    tag(pd, 5, 34, 496)
    lv = im.sector(35)
    put32(lv, 0x10, lvd_seq)
    osta(lv, 0x14)
    dstring(lv, 0x54, 128, label)
    put32(lv, 0xd4, 2048)
    lv[0xd9:0xd9 + 19] = b"*OSTA UDF Compliant"
    put32(lv, 0xf8, 2048)
    put32(lv, 0xfc, 0)
    put32(lv, 0x108, 6)
    put32(lv, 0x10c, 1)
    put32(lv, 0x1b0, 2048)
    put32(lv, 0x1b4, 64)
    lv[0x1b8] = 1
    lv[0x1b9] = 6
    tag(lv, 6, 35, 446 - 16)
    tag(im.sector(36), 8, 36, 0)
    for s in range(32, 37):
        d = bytearray(im.sector(s))
        retag(d, s + 16)
        im.sector(s + 16)[:] = d
    v = im.sector(64)
    put32(v, 0x1c, 1)
    put32(v, 0x48, 1)
    put32(v, 0x4c, 46)
    put16(v, 80 + 8 + 40, 0x0102)
    tag(v, 9, 64, 80 + 8 + 46 - 16)
    tag(im.sector(65), 8, 65, 0)


def fsd(im, lbn, root):
    f = bytearray(S)
    put32(f, 0x190, 2048)
    put32(f, 0x194, root)
    f[0x1a1:0x1a1 + 19] = b"*OSTA UDF Compliant"
    tag(f, 0x100, lbn, 496)
    im.put(PART_START + lbn, f)


def build(label, fids):
    """Root holds VIDEO_TS and README (5000 bytes at 10..12); VIDEO_TS holds
    fids (file entries at 20.., 100 + k bytes at 30..)."""
    im = Img()
    volume(im, label, 1)
    fsd(im, 0, 1)
    root = im.directory(2, 1, [fid("VIDEO_TS", 0x02, 3, 0), fid("README", 0, 4, 0)])
    im.file_entry(1, 4, root, 4, [(root, 2)])
    vt = im.directory(5, 1, fids)
    im.file_entry(3, 4, vt, 4, [(vt, 5)])
    base = (PART_START + 10) * S
    im.d[base:base + 5000] = bytes(i % 256 for i in range(5000))
    im.file_entry(4, 5, 5000, 4, [(5000, 10)])
    for k in range(4):
        im.file_entry(20 + k, 5, 100 + k, 4, [(100 + k, 30 + k)])
    return im


def standard():
    return build("TEST_DISC", [fid("VIDEO_TS.IFO", 0, 20, 0), fid("VTS_01_0.IFO", 0, 21, 0)])


def p(lbn):
    return PART_START + lbn


# ---- tests on built images ----

def test_mounts_lists_and_reads():
    v = mounted(standard())
    assert v.label_bytes() == b"TEST_DISC"
    assert v.revision() == 0x0102
    assert v.year() == 2009
    assert v.list("/", raw=True) == [(b"..", True), (b"VIDEO_TS", True), (b"README", False)]
    assert opened(v, "/README") == (5000, [(p(10), 3)], None)
    buf = read(v, "/README")
    assert all(x == i % 256 for i, x in enumerate(buf))
    assert opened(v, "/VIDEO_TS") == averror(ENOENT), "a directory is not a file"
    assert opened(v, "/NOPE") == averror(ENOENT)
    assert v.list("/README") == averror(ENOENT), "a file is not a directory"


def test_extents_of_a_valid_image():
    # (the archived test compared these with the NetBSD-based reader)
    v = mounted(standard())
    assert opened(v, "/VIDEO_TS/VIDEO_TS.IFO")[1] == [(p(30), 1)]
    assert opened(v, "/VIDEO_TS/VTS_01_0.IFO")[1] == [(p(31), 1)]
    assert opened(v, "/VIDEO_TS/VTS_01_0.IFO")[0] == 101


def test_adjacent_pieces_are_joined():
    # README in three allocation descriptors: 2 blocks at 10, 1 at 12 (adjacent), 1 at 40.
    im = standard()
    im.file_entry(4, 5, 4 * 2048, 4, [(2 * 2048, 10), (2048, 12), (2048, 40)])
    assert opened(mounted(im), "/README")[1] == [(p(10), 3), (p(40), 1)]


def test_blocks_past_the_allocation_are_one_unrecorded_run():
    # README records 2^50 bytes but has one block of allocation: the rest is
    # not recorded (and mapping it must not take one step per block).
    im = standard()
    im.file_entry(4, 5, 1 << 50, 4, [(2048, 10)])
    assert opened(mounted(im), "/README")[1] == [(p(10), 1), (-1, (1 << 39) - 1)]


def test_paths_follow_the_vfs():
    v = mounted(standard())
    assert not isinstance(opened(v, "/VIDEO_TS/./VIDEO_TS.IFO"), int)
    assert not isinstance(opened(v, "/VIDEO_TS/../VIDEO_TS/VIDEO_TS.IFO"), int)
    assert not isinstance(opened(v, "/../README"), int), "the root's parent is the root"
    assert opened(v, "\\VIDEO_TS\\VIDEO_TS.IFO") == averror(ENOENT), "'\\' is a plain character"
    assert raw_names(v, "/VIDEO_TS/..") == ["..", "VIDEO_TS", "README"]


def test_hidden_entries_are_skipped_and_duplicates_resolve_to_the_first():
    v = mounted(build("H", [fid("SEEN.IFO", 0, 20, 0), fid("HIDDEN.IFO", 0x01, 21, 0)]))
    assert raw_names(v, "/VIDEO_TS") == ["..", "SEEN.IFO"]
    assert opened(v, "/VIDEO_TS/HIDDEN.IFO") == averror(ENOENT)
    v = mounted(build("D", [fid("A.IFO", 0, 20, 0), fid("A.IFO", 0, 22, 0)]))
    assert opened(v, "/VIDEO_TS/A.IFO")[0] == 100
    assert raw_names(v, "/VIDEO_TS") == ["..", "A.IFO", "A.IFO"]


def test_names_keep_windows_characters_and_mangle_slash():
    n = raw_names(mounted(build("N", [fid("A:B?C.", 0, 20, 0), fid("a/b", 0, 21, 0)])), "/VIDEO_TS")
    assert n[1] == "A:B?C.", "only '/' and NUL are changed"
    assert n[2] == f"a_b#{crc16(b'a/b'):04X}"


def test_names_are_translated_as_linux_does():
    smiley = bytes([16, 0xD8, 0x3D, 0xDE, 0x00])  # U+1F600 as UTF-16: D83D DE00
    fids = [
        fid(".", 0, 20, 0),
        fid("..", 0, 21, 0),
        fid("x/y.txt", 0, 22, 0),
        (smiley, 0, 23, 0),
        (bytes([3, ord("a")]), 0, 20, 0),   # unknown compression ID: skipped
        (bytes([16, ord("a")]), 0, 20, 0),  # odd 16-bit length: skipped
    ]
    v = mounted(build("U", fids))
    assert raw_names(v, "/VIDEO_TS") == [
        "..",
        f".#{crc16(b'.'):04X}",
        f"..#{crc16(b'..'):04X}",
        f"x_y#{crc16(b'x/y.txt'):04X}.txt",
        "\U0001F600",
    ]
    assert opened(v, "/VIDEO_TS/\U0001F600")[0] == 103


def test_an_unconvertible_name_fails_a_lookup_in_its_directory():
    # Linux's lookup fails on a name it cannot convert (the listing skips it).
    v = mounted(build("B", [(bytes([3, ord("a")]), 0, 20, 0), fid("B.IFO", 0, 21, 0)]))
    assert opened(v, "/VIDEO_TS/B.IFO") == INVALIDDATA
    assert v.list("/VIDEO_TS") == INVALIDDATA, "the listing looks every name up"


def test_a_volume_without_a_recognition_sequence_is_refused():
    im = standard()
    for s in range(16, 19):
        im.sector(s)[:] = bytes(S)
    assert isinstance(mount(im), int)


def test_the_highest_sequence_number_wins():
    # A second LVD (label SECOND) with a LOWER sequence number after the
    # first: Linux keeps the first.
    im = standard()
    l2 = bytearray(im.sector(35))
    l2[0x54:0xd4] = bytes(0x80)
    dstring(l2, 0x54, 128, "SECOND")
    put32(l2, 0x10, 0)
    tag(l2, 6, 36, 446 - 16)
    td = bytearray(im.sector(36))
    tag(td, 8, 37, 0)
    im.sector(36)[:] = l2
    im.sector(37)[:] = td
    assert mounted(im).label_bytes() == b"TEST_DISC"


def test_the_reserve_sequence_is_used_when_the_main_one_has_no_primary_descriptor():
    im = standard()
    im.sector(32)[:] = bytes(S)
    v = mounted(im)
    assert v.label_bytes() == b"TEST_DISC"
    assert opened(v, "/README")[0] == 5000


def test_a_wrong_tag_location_is_not_a_descriptor():
    im = standard()
    moved = bytearray(im.sector(PART_START + 21))
    retag(moved, 99)
    im.put(PART_START + 21, moved)
    assert opened(mounted(im), "/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA


def test_strategy_4096_follows_an_indirect_entry_in_the_next_block():
    im = standard()
    # VTS_01_0.IFO (block 21): strategy 4096; block 22 holds an indirect
    # entry to block 40, which holds the real file entry (size 101).
    im.file_entry(40, 5, 101, 4, [(101, 31)])
    im.file_entry(21, 5, 7, 4096, [(7, 31)])
    ie = bytearray(S)
    put16(ie, 0x14, 4096)
    put32(ie, 0x24, 2048)
    put32(ie, 0x28, 40)
    tag(ie, 0x103, 22, 52 - 16)
    im.put(PART_START + 22, ie)
    assert opened(mounted(im), "/VIDEO_TS/VTS_01_0.IFO")[0] == 101


def test_an_unknown_strategy_or_file_type_is_an_error():
    im = standard()
    im.file_entry(21, 5, 101, 7, [(101, 31)])
    assert opened(mounted(im), "/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA
    im = standard()
    im.file_entry(21, 13, 101, 4, [(101, 31)])
    assert opened(mounted(im), "/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA


def test_a_directory_entry_whose_crc_length_does_not_match_breaks_the_directory():
    im = standard()
    at = (PART_START + 5) * S + 40
    b = memoryview(im.d)[at:at + 52]
    put16(b, 10, 0)
    checksum(b)
    assert mounted(im).list("/VIDEO_TS") == INVALIDDATA


def test_embedded_data_and_unrecorded_extents():
    im = standard()
    b = bytearray(S)
    put16(b, 0x14, 4)
    b[0x1b] = 5
    put16(b, 0x22, 3)
    put16(b, 0x30, 1)
    put64(b, 0x38, 5)
    put32(b, 0xac, 5)
    b[0xb0:0xb5] = b"hello"
    tag(b, 0x105, 21, 176 + 5 - 16)
    im.put(PART_START + 21, b)
    # VIDEO_TS.IFO: one not-recorded extent of 3000 bytes.
    im.file_entry(20, 5, 3000, 4, [((1 << 30) | 3000, 0)])
    v = mounted(im)
    assert opened(v, "/VIDEO_TS/VTS_01_0.IFO") == (5, [], b"hello")
    assert read(v, "/VIDEO_TS/VTS_01_0.IFO") == b"hello"
    # not recorded: sector -1 (ff_discio_file_read does not serve such runs yet)
    assert opened(v, "/VIDEO_TS/VIDEO_TS.IFO")[1] == [(-1, 2)]


def test_a_vat_in_the_last_block_maps_a_virtual_partition():
    im = Img()
    volume(im, "VAT", 1)
    # LVD: maps type 1 (partition 0) + virtual (partition 0, UDF 2.00); FSD
    # at virtual block 0.
    lv = im.sector(35)
    put32(lv, 0x108, 6 + 64)
    put32(lv, 0x10c, 2)
    put16(lv, 0x100, 1)
    m = 0x1b8 + 6
    lv[m] = 2
    lv[m + 1] = 64
    lv[m + 5:m + 5 + 22] = b"*UDF Virtual Partition"
    put16(lv, m + 28, 0x0200)
    put16(lv, m + 38, 0)
    tag(lv, 6, 35, 446 + 64 - 16)
    # Physical: FSD 10, root FE 11, root data 12, file FE 13, data 14.
    fsd(im, 10, 1)
    # tag locations of virtual-partition descriptors are their virtual block
    f = bytearray(im.sector(PART_START + 10))
    put16(f, 0x198, 1)
    tag(f, 0x100, 0, 496)
    im.put(PART_START + 10, f)
    root = im.directory(12, 1, [fid("F.TXT", 0, 3, 1)])
    base = (PART_START + 12) * S
    d = bytearray(im.d[base:base + root])
    at = 0
    while at < len(d):
        length = 16 + struct.unpack_from("<H", d, at + 10)[0]
        if at == 0:
            put16(d, at + 0x1c, 1)
        e = memoryview(d)[at:at + length]
        tag(e, 0x101, 2, length - 16)
        at += length
    im.put(PART_START + 12, d)
    im.file_entry(11, 4, root, 4, [(root, 2)])
    fe = bytearray(im.sector(PART_START + 11))
    retag(fe, 1)
    im.put(PART_START + 11, fe)
    im.file_entry(13, 5, 5, 4, [(5, 14)])
    fe = bytearray(im.sector(PART_START + 13))
    # Data through the type-1 partition: long AD (5, 14, part 0).
    fe[0x22] = 1
    put32(fe, 0xac, 16)
    put32(fe, 0xb0, 5)
    put32(fe, 0xb4, 14)
    put16(fe, 0xb8, 0)
    tag(fe, 0x105, 3, 176 + 16 - 16)
    im.put(PART_START + 13, fe)
    im.put(PART_START + 14, b"virt!")
    # VAT 2.00 data at partition block 217, its file entry in the last
    # block (518 = partition block 218).
    vat = bytearray(152 + 16)
    put16(vat, 0, 152)
    for k, ph in enumerate([10, 11, 12, 13]):
        put32(vat, 152 + 4 * k, ph)
    im.put(PART_START + 217, vat)
    im.file_entry(218, 248, len(vat), 4, [(len(vat), 217)])
    assert PART_START + 218 == len(im.d) // S - 2
    # Make the VAT entry the image's last block.
    im.d = im.d[:(PART_START + 219) * S]
    v = mounted(im)
    assert v.label_bytes() == b"VAT"
    assert read(v, "/F.TXT") == b"virt!"
    assert opened(v, "/F.TXT")[1] == [(p(14), 1)]


# ---- real discs: the reader against reference dumps ----

def sha256_hex(v, path):
    h = hashlib.sha256()
    ret = read_all(v, path, h.update)
    return ret if ret < 0 else h.hexdigest()


def dump(path, key, n):
    """The dump of one image, in the reference format: info, every listing by
    tree walk, every file's extents, sha256 of files under 64 MiB."""
    out = [f"image {key}"]
    image = Image.open(path)
    fs = ffi.new("DiscIOFS **")
    if lib.ff_discio_udf_linux_mount(image.ptr, fs) < 0:
        out.append("info rc 1")
        return out
    v = Vol(fs[0], image)
    out += ["info rc 0", f"label {v.label()}", f"revision {v.revision():#06x}", f"year {v.year()}"]
    queue = collections.deque(["/"])
    while queue:
        d = queue.popleft()
        n["listings"] += 1
        entries = v.list(d, raw=True)
        if isinstance(entries, int):
            out.append(f"ls {d} rc 1")
            continue
        out.append(f"ls {d} rc 0")
        base = d.rstrip("/")
        files = []
        for name, is_dir in entries:
            name = name.decode("utf-8", "replace")
            child = f"{base}/{name}"
            if is_dir:
                out.append(f"d {name}")
                if name != "..":
                    queue.append(child)
            else:
                f = opened(v, child)
                if isinstance(f, int):
                    out.append(f"o {name}" if f == averror(ENOENT) else f"f? {f} {name}")
                else:
                    out.append(f"f {f[0]} {name}")
                    files.append((child, f))
        for child, (size, ext, data) in files:
            n["files"] += 1
            out += [f"ext {child} rc 0", f"size {size}"]
            if data is not None:
                out.append(f"embedded {len(data)}")
            for s, c in ext:
                n["extents"] += 1
                out.append(f"- {c}" if s < 0 else f"{s} {c}")
            if size < 64 << 20:
                n["contents"] += 1
                h = sha256_hex(v, child)
                out.append(f"cat {child} rc 1 -" if isinstance(h, int) else f"cat {child} rc 0 {h}")
    return out


@pytest.mark.disc
def test_corpus_matches_the_reference_dumps():
    folder = pathlib.Path(env_or_skip("DISCIO_UDF_REFERENCE"))
    index = (folder / "index.tsv").read_text()
    images, diffs = 0, 0
    total = collections.Counter()
    for line in index.splitlines():
        if not line:
            continue
        key, path = line.split("\t", 1)
        if not os.path.exists(path):
            print(f"{path}: image missing (link to an unmounted drive?), skipped")
            continue
        want = (folder / f"{key}.txt").read_text().splitlines()
        n = collections.Counter(listings=0, files=0, extents=0, contents=0)
        got = dump(path, key, n)
        images += 1
        size = max(len(want), len(got))
        bad = [i for i in range(size)
               if (want[i] if i < len(want) else None) != (got[i] if i < len(got) else None)]
        if not bad:
            print(f"SAME {key}: {n['listings']} listings, {n['files']} files, {n['extents']} extents, "
                  f"{n['contents']} contents")
        else:
            diffs += len(bad)
            print(f"DIFF {key}: {len(bad)} of {size} lines")
            for i in bad[:8]:
                print(f"    line {i + 1}: reference {want[i] if i < len(want) else None!r} | "
                      f"ours {got[i] if i < len(got) else None!r}")
        total += n
    print(f"corpus: {images} images, {total['listings']} listings, {total['files']} files, "
          f"{total['extents']} extents, {total['contents']} contents compared, {diffs} differing lines")
    assert images > 0, "no image of the index exists"
    assert diffs == 0, "the reader differs from the reference dumps"


# ---- our loop guards (structural, never time-based) ----

def loop_image():
    """VIDEO_TS holds LOOP.IFO (an indirect entry chain 60 -> 62 -> 60) and
    AED.IFO (a 1 TiB file whose allocation extent at 72 continues at 72)."""
    im = build("LOOPS", [fid("LOOP.IFO", 0, 60, 0), fid("AED.IFO", 0, 70, 0)])
    # strategy 4096: the indirect entry is the block after the file entry
    for fe, nxt in [(60, 62), (62, 60)]:
        im.file_entry(fe, 5, 100, 4096, [(100, 30)])
        ie = bytearray(S)
        put16(ie, 0x14, 4)
        put32(ie, 36, 2048)
        put32(ie, 40, nxt)
        put16(ie, 44, 0)
        tag(ie, 259, fe + 1, 36)
        im.put(PART_START + fe + 1, ie)
    # the file entry's one descriptor continues in the allocation extent at 72,
    # which holds one recorded block and then continues at 72 again
    im.file_entry(70, 5, 1 << 40, 4, [((3 << 30) | 2048, 72)])
    aed = bytearray(S)
    put32(aed, 20, 16)
    put32(aed, 24, 2048)
    put32(aed, 28, 30)
    put32(aed, 32, (3 << 30) | 2048)
    put32(aed, 36, 72)
    tag(aed, 258, 72, 24)
    im.put(PART_START + 72, aed)
    return im


def test_an_indirect_entry_loop_stops_at_the_first_revisit():
    v = mounted(loop_image())
    t = time.monotonic()
    assert isinstance(opened(v, "/VIDEO_TS/LOOP.IFO"), int)
    # the first revisit, not upstream's 1024 entries
    assert time.monotonic() - t < 5


def test_an_allocation_extent_loop_is_an_error_not_a_hang():
    v = mounted(loop_image())
    t = time.monotonic()
    assert isinstance(opened(v, "/VIDEO_TS/AED.IFO"), int)
    # stopped at the first revisit (without the guard the walk runs on through
    # the 1 TiB the file entry records)
    assert time.monotonic() - t < 5
    # the rest of the volume is unaffected
    assert not isinstance(opened(v, "/README"), int)
