"""libavformat/discio_udf_netbsd.c: the UDF reader based on NetBSD, on small
images built here (ECMA-167 / OSTA UDF layouts: UDF 1.02 with one type-1
partition, and virtual, sparable and metadata partitions), and on the corpus
images when DISCIO_UDF_REFERENCE names a folder of reference dumps
(index.tsv: disc id and image path; <disc id>.txt: the dump)."""

import collections
import hashlib
import os
import pathlib
import struct

import pytest

from helpers import EINVAL, ENOENT, averror, ffi, lib
from helpers.disc import Image, Vol, env_or_skip

INVALIDDATA = -0x41444E49  # FFERRTAG('I','N','D','A')
S = 2048
PART_START = 300


# ---------------------------------------------------------------------------
# Running the reader.

def mount(data):
    """A Vol on the image data (in memory), or the negative error code."""
    from synth.image import Img
    im = Img()
    im.d = bytearray(data)
    image = Image.memory(im)
    fs = ffi.new("DiscIOFS **")
    ret = lib.ff_discio_udf_netbsd_mount(image.ptr, fs)
    return ret if ret < 0 else Vol(fs[0], image)


def mounted(data):
    v = mount(data)
    assert not isinstance(v, int), f"mount failed: {v}"
    return v


# ---------------------------------------------------------------------------
# Descriptor builders.

def crc16(data):
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


def tag(b, ident, crclen):
    """Fills in a descriptor tag: id, version 2, CRC over crclen bytes,
    checksum."""
    put16(b, 0, ident)
    put16(b, 2, 2)
    put16(b, 10, crclen)
    put16(b, 8, crc16(bytes(b[16:16 + crclen])))
    checksum(b)


def dstring(b, at, size, text):
    b[at] = 8
    b[at + 1:at + 1 + len(text)] = text.encode()
    b[at + size - 1] = 1 + len(text)


def osta(b, at):
    b[at] = 0
    b[at + 1:at + 24] = b"OSTA Compressed Unicode"


# Allocation descriptors of a file entry: ("short", [(length with extent type
# bits, logical block)]), ("long", [(length, logical block, partition
# reference)]) or ("inline", data recorded in the file entry).

def file_entry(file_type, size, strategy, ads):
    """A file entry: file_type (4 directory, 5 file, 248 VAT), strategy."""
    b = bytearray(S)
    put16(b, 0x14, strategy)
    b[0x1b] = file_type
    put64(b, 0x38, size)
    put64(b, 0xa0, 0x12345678)  # unique id
    at = 0xb0
    kind, items = ads
    if kind == "short":
        for length, lbn in items:
            put32(b, at, length)
            put32(b, at + 4, lbn)
            at += 8
        flags = 0
    elif kind == "long":
        for length, lbn, part in items:
            put32(b, at, length)
            put32(b, at + 4, lbn)
            put16(b, at + 8, part)
            at += 16
        flags = 1
    else:
        b[at:at + len(items)] = items
        at += len(items)
        flags = 3
    put16(b, 0x22, flags)
    put32(b, 0xa8, 0)
    put32(b, 0xac, at - 0xb0)
    tag(b, 0x105, 0)
    return b


def indirect_entry(lbn, part):
    """An indirect entry pointing at (lbn, part)."""
    b = bytearray(S)
    put16(b, 0x14, 4)
    put32(b, 0x24, 2048)
    put32(b, 0x28, lbn)
    put16(b, 0x2c, part)
    tag(b, 0x103, 0)
    return b


def fid_raw(name, chars, lbn, part):
    """One file identifier descriptor with a recorded name of raw bytes
    (compression ID first; empty = no name)."""
    length = (38 + len(name) + 3) & ~3
    b = bytearray(length)
    put16(b, 0x10, 1)
    b[0x12] = chars
    b[0x13] = len(name)
    put32(b, 0x14, 2048)
    put32(b, 0x18, lbn)
    put16(b, 0x1c, part)
    b[0x26:0x26 + len(name)] = name
    tag(b, 0x101, 0)
    return b


def name8(s):
    return b"" if not s else b"\x08" + s.encode()


def directory(parent, fids):
    """Directory data: a parent entry, then fids (name, characteristics,
    ICB block, ICB partition)."""
    out = fid_raw(b"", 0x0a, parent[0], parent[1])
    for name, chars, lbn, part in fids:
        out += fid_raw(name8(name), chars, lbn, part)
    return out


def map_type1(part_num):
    m = bytearray([1, 6, 1, 0, 0, 0])
    put16(m, 4, part_num)
    return m


def map_type2(ident, part_num):
    m = bytearray(64)
    m[0] = 2
    m[1] = 64
    m[5:5 + len(ident)] = ident.encode()
    put16(m, 36, 1)
    put16(m, 38, part_num)
    return m


def sec(n):
    return slice(n * S, (n + 1) * S)


def volume(d, label, maps, fsd_at, partitions, integrity_closed):
    """Writes anchor (at 256), volume descriptor sequence (main at 32,
    reserve at 48) and integrity descriptor (at 64) into d. fsd_at: file set
    descriptor location (block, partition reference); partitions: (number,
    start, length)."""
    a = memoryview(d)[sec(256)]
    put32(a, 0x10, 16 * 2048)
    put32(a, 0x14, 32)
    put32(a, 0x18, 16 * 2048)
    put32(a, 0x1c, 48)
    tag(a, 2, 0)
    n = 32
    p = memoryview(d)[sec(n)]
    put16(p, 0x178 + 2, 2009)
    tag(p, 1, 0)
    n += 1
    iu = memoryview(d)[sec(n)]
    iu[0x15:0x21] = b"*UDF LV Info"
    put16(iu, 0x2c, 0x0102)
    tag(iu, 4, 0)
    n += 1
    for num, start, length in partitions:
        pd = memoryview(d)[sec(n)]
        pd[0x14] = 1
        put16(pd, 0x16, num)
        put32(pd, 0xbc, start)
        put32(pd, 0xc0, length)
        tag(pd, 5, 0)
        n += 1
    lv = memoryview(d)[sec(n)]
    osta(lv, 0x14)
    dstring(lv, 0x54, 128, label)
    put32(lv, 0xd4, 2048)
    lv[0xd9:0xd9 + 19] = b"*OSTA UDF Compliant"
    put32(lv, 0xf8, 2048)
    put32(lv, 0xfc, fsd_at[0])
    put16(lv, 0x100, fsd_at[1])
    at = 0x1b8
    for m in maps:
        lv[at:at + len(m)] = m
        at += len(m)
    put32(lv, 0x108, at - 0x1b8)
    put32(lv, 0x10c, len(maps))
    put32(lv, 0x1b0, 2048)
    put32(lv, 0x1b4, 64)
    tag(lv, 6, 0)
    n += 1
    tag(memoryview(d)[sec(n)], 8, 0)
    lvid = memoryview(d)[sec(64)]
    put32(lvid, 0x1c, int(integrity_closed))
    put32(lvid, 0x48, 1)
    tag(lvid, 9, 0)


def fsd(root):
    """A file set descriptor with the root directory at (lbn, part)."""
    f = bytearray(S)
    put32(f, 0x190, 2048)
    put32(f, 0x194, root[0])
    put16(f, 0x198, root[1])
    tag(f, 0x100, 0)
    return f


# ---------------------------------------------------------------------------
# The standard UDF 1.02 image.

class Built:
    def __init__(self, sectors=520):
        self.data = bytearray(sectors * S)

    def sector(self, n):
        return memoryview(self.data)[sec(n)]

    def lb(self, lbn):
        return self.sector(PART_START + lbn)

    def put_lb(self, lbn, data):
        at = (PART_START + lbn) * S
        self.data[at:at + len(data)] = data

    def file(self, lbn, file_type, size, data_lbn):
        self.put_lb(lbn, file_entry(file_type, size, 4, ("short", [(size, data_lbn)])))


def build_raw(label, vts):
    """A UDF 1.02 image: root holds VIDEO_TS (directory) and README (file);
    VIDEO_TS holds the entries vts (directory data at block 5; file entries
    at blocks 20.., 100 + k bytes of data at blocks 30..)."""
    im = Built()
    volume(im.data, label, [map_type1(0)], (0, 0), [(0, PART_START, 64)], True)
    im.put_lb(0, fsd((1, 0)))
    root = directory((1, 0), [("VIDEO_TS", 0x02, 3, 0), ("README", 0, 4, 0)])
    im.put_lb(2, root)
    im.file(1, 4, len(root), 2)
    im.put_lb(5, vts)
    im.file(3, 4, len(vts), 5)
    at = (PART_START + 10) * S
    im.data[at:at + 5000] = bytes(i % 256 for i in range(5000))
    im.file(4, 5, 5000, 10)
    for k in range(4):
        im.file(20 + k, 5, 100 + k, 30 + k)
    return im


def build(label, video_ts_fids):
    return build_raw(label, directory((1, 0), video_ts_fids))


def standard():
    return build("TEST_DISC", [("VIDEO_TS.IFO", 0, 20, 0), ("VTS_01_0.IFO", 0, 21, 0)])


# ---------------------------------------------------------------------------
# Tests on built images.

def test_mounts_and_reports_label_revision_and_year():
    v = mounted(standard().data)
    assert v.label_bytes() == b"TEST_DISC"
    assert v.revision() == 0x0102
    assert v.year() == 2009


def test_lists_in_directory_order_with_the_parent_entry():
    v = mounted(standard().data)
    assert v.list("/") == [("..", True), ("VIDEO_TS", True), ("README", False)]
    assert v.open("/README").size() == 5000


def test_opens_files_by_path_and_reads_them():
    v = mounted(standard().data)
    f = v.open("/README")
    assert f.size() == 5000
    assert f.extents() == [(PART_START + 10, 3)]
    buf = f.read(0, 5000)
    assert all(x == i % 256 for i, x in enumerate(buf))
    assert len(f.read(4990, 10)) == 10
    assert f.read(4995, 10) == averror(EINVAL), "reads stop at the end of the file"
    assert v.open("/VIDEO_TS/VTS_01_0.IFO").size() == 101


def test_the_allocation_type_is_the_low_two_bits_of_the_icb_flags():
    im = standard()
    # README's file entry (block 4): short descriptors with reserved flag bit 2 set.
    im.lb(4)[0x22] |= 0x04
    v = mounted(im.data)
    f = v.open("/README")
    assert f.size() == 5000
    assert f.extents()[0][0] == PART_START + 10


def test_extended_descriptors_with_an_empty_area_read_as_no_data():
    im = standard()
    lb = im.lb(4)
    lb[0x22] = (lb[0x22] & ~3) | 2
    lb[0xac:0xb0] = bytes(4)
    lb[0xb0:0xb8] = bytes(8)
    v = mounted(im.data)
    # The node loads; its 5000 bytes have no extent to come from.
    assert v.open("/README") == INVALIDDATA


def test_an_unreadable_root_entry_fails_the_mount():
    im = standard()
    im.lb(1)[0] ^= 0xff
    assert isinstance(mount(im.data), int)


def test_path_separators_and_empty_components():
    v = mounted(standard().data)
    assert not isinstance(v.open("\\VIDEO_TS\\VIDEO_TS.IFO"), int)
    assert not isinstance(v.open("//VIDEO_TS//VIDEO_TS.IFO"), int)
    assert v.open("VIDEO_TS/VIDEO_TS.IFO") == averror(ENOENT), "a path must start with a separator"
    assert v.open("/video_ts/VIDEO_TS.IFO") == averror(ENOENT), "names are case-sensitive"
    assert v.open("/VIDEO_TS") == averror(ENOENT), "a directory is not a file"
    assert not isinstance(v.list("/"), int)
    assert v.list("/README") == averror(ENOENT), "a file is not a directory"


def test_duplicate_names_resolve_to_the_last_entry():
    v = mounted(build("DUP", [("A.IFO", 0, 20, 0), ("A.IFO", 0, 22, 0)]).data)
    assert v.open("/VIDEO_TS/A.IFO").size() == 102
    assert v.names("/VIDEO_TS") == ["..", "A.IFO", "A.IFO"], "both entries are listed"


def test_deleted_entries_are_skipped():
    v = mounted(build("DEL", [("GONE.IFO", 0x04, 20, 0), ("KEPT.IFO", 0, 21, 0)]).data)
    assert v.open("/VIDEO_TS/GONE.IFO") == averror(ENOENT)
    assert v.names("/VIDEO_TS") == ["..", "KEPT.IFO"]


def test_hidden_entries_are_listed_and_found():
    v = mounted(build("HID", [("SEEN.IFO", 0, 20, 0), ("HIDDEN.IFO", 0x01, 21, 0)]).data)
    assert v.names("/VIDEO_TS") == ["..", "SEEN.IFO", "HIDDEN.IFO"]
    assert v.open("/VIDEO_TS/HIDDEN.IFO").size() == 101


def test_illegal_characters_follow_the_windows_name_rules():
    v = mounted(build("NAMES", [("A:B?C.", 0, 20, 0)]).data)
    assert v.names("/VIDEO_TS") == ["..", "A_B_C#D24F"]
    assert not isinstance(v.open("/VIDEO_TS/A_B_C#D24F"), int)
    assert v.open("/VIDEO_TS/A:B?C.") == averror(ENOENT), "a lookup does not apply the Windows rules"


def listed_as(raw):
    """The name a single recorded name in VIDEO_TS is listed as."""
    vts = fid_raw(b"", 0x0a, 1, 0) + fid_raw(raw, 0, 20, 0)
    names = mounted(build_raw("NAMES", vts).data).names("/VIDEO_TS")
    assert len(names) == 2
    return names[1]


def test_osta_name_translation():
    assert listed_as(name8("VIDEO_TS.IFO")) == "VIDEO_TS.IFO", "plain names are unchanged"
    trailing = listed_as(name8("ABC  "))
    assert trailing.startswith("ABC#") and len(trailing) == 8, f"trailing spaces are removed: {trailing}"
    dele = listed_as(name8("A\x7fB"))
    assert dele.startswith("A_B#"), f"DEL counts as not printable: {dele}"
    assert listed_as(bytes([3, ord("A")])) == "", "a bad compression ID gives an empty name"
    assert listed_as(bytes([16, 0x30, 0x42, 0x00, 0x41])) == "あA", "16-bit names"
    assert listed_as(bytes([254, ord("X"), ord("Y")])) == "XY", "compression ID 254 counts as 8"


def test_sixteen_bit_names_are_found_by_their_utf8():
    vts = fid_raw(b"", 0x0a, 1, 0) + fid_raw(bytes([16, 0x30, 0x42, 0x00, 0x41]), 0, 21, 0)
    v = mounted(build_raw("NAMES", vts).data)
    assert v.open("/VIDEO_TS/あA").size() == 101


def test_the_parent_entry_is_found_by_name():
    v = mounted(standard().data)
    assert v.names("/VIDEO_TS/..") == v.names("/")


def test_a_broken_entry_fails_the_listing_and_every_lookup():
    im = build("BROKEN", [("X.IFO", 0, 20, 0), ("Y.IFO", 0, 21, 0)])
    im.data[(PART_START + 5) * S + 40 + 4] ^= 0xff
    v = mounted(im.data)
    assert v.list("/VIDEO_TS") == INVALIDDATA
    assert v.open("/VIDEO_TS/X.IFO") == INVALIDDATA


def test_an_entry_with_a_bad_crc_but_a_consistent_length_is_used():
    im = build("CRC", [("A.IFO", 0, 20, 0), ("B.IFO", 0, 21, 0)])
    # The second entry after the parent (40 bytes) is 44 bytes long: CRC
    # length 28 (= 44 - 16) with a wrong CRC.
    at = (PART_START + 5) * S + 40
    b = memoryview(im.data)[at:at + 44]
    put16(b, 10, 28)
    put16(b, 8, 0xbeef)
    checksum(b)
    v = mounted(im.data)
    assert v.names("/VIDEO_TS") == ["..", "A.IFO", "B.IFO"]
    # CRC length 27: inconsistent, the entry is broken.
    put16(b, 10, 27)
    checksum(b)
    v = mounted(im.data)
    assert v.list("/VIDEO_TS") == INVALIDDATA


def test_a_blank_integrity_sector_fails_the_mount():
    im = standard()
    im.sector(64)[:] = bytes(S)
    assert mount(im.data) == INVALIDDATA


def test_an_unclean_volume_is_read():
    im = standard()
    put32(im.sector(64), 0x1c, 0)
    tag(im.sector(64), 9, 0)
    assert mounted(im.data).label_bytes() == b"TEST_DISC"


def test_anchors_at_512_or_at_the_end_are_enough():
    im = standard()
    im.sector(512)[:] = im.sector(256)
    im.sector(256)[:] = bytes(S)
    assert mounted(im.data).label_bytes() == b"TEST_DISC"
    for end in [519, 519 - 256]:
        im = standard()
        im.sector(end)[:] = im.sector(256)
        im.sector(256)[:] = bytes(S)
        assert mounted(im.data).label_bytes() == b"TEST_DISC", f"anchor at sector {end}"


def test_no_anchor_is_not_udf():
    im = standard()
    im.sector(256)[:] = bytes(S)
    assert mount(im.data) == INVALIDDATA


def copy_reserve(im):
    for s in range(32, 37):
        im.sector(s + 16)[:] = im.sector(s)


def test_the_reserve_sequence_is_used_when_the_main_one_fails():
    im = standard()
    copy_reserve(im)
    im.sector(32)[0] ^= 0xff
    assert mounted(im.data).label_bytes() == b"TEST_DISC"


def zero_size_descriptor():
    """An unallocated space descriptor whose size (24 + 8 * count) wraps to 0."""
    u = bytearray(S)
    put32(u, 0x14, 536870909)
    tag(u, 7, 0)
    return u


def test_a_descriptor_of_size_0_ends_the_sequence_with_an_error():
    im = standard()
    im.sector(33)[:] = zero_size_descriptor()
    assert isinstance(mount(im.data), int), "no reserve sequence: the mount fails (and does not hang)"
    im = standard()
    copy_reserve(im)
    im.sector(33)[:] = zero_size_descriptor()
    assert mounted(im.data).label_bytes() == b"TEST_DISC", "the reserve sequence is used"


def test_the_last_logical_volume_descriptor_wins():
    im = standard()
    # Sequence: PVD 32, IUVD 33, PD 34, LVD 35, TD 36. Put a second LVD with
    # another label at 36 and the terminator at 37.
    l2 = bytearray(im.sector(35))
    l2[0x54:0xd4] = bytes(0x80)
    dstring(l2, 0x54, 128, "SECOND")
    tag(l2, 6, 0)
    td = bytes(im.sector(36))
    im.sector(36)[:] = l2
    im.sector(37)[:] = td
    assert mounted(im.data).label_bytes() == b"SECOND"


def test_a_label_with_illegal_characters_follows_the_windows_name_rules():
    im = standard()
    lv = im.sector(35)
    lv[0x54:0xd4] = bytes(0x80)
    dstring(lv, 0x54, 128, "A:B?C.")
    tag(lv, 6, 0)
    assert mounted(im.data).label_bytes() == b"A_B_C#D24F"


def test_a_file_with_its_data_in_the_file_entry_cannot_be_opened():
    im = standard()
    im.put_lb(21, file_entry(5, 11, 4, ("inline", b"hello world")))
    v = mounted(im.data)
    assert v.open("/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA
    assert v.names("/VIDEO_TS") == ["..", "VIDEO_TS.IFO", "VTS_01_0.IFO"], "it is still listed"


def test_an_unknown_strategy_is_an_error_not_a_hang():
    im = standard()
    put16(im.lb(21), 0x14, 7)
    tag(im.lb(21), 0x105, 0)
    assert mounted(im.data).open("/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA


def test_indirect_entries_are_followed_and_loops_stopped():
    # VTS_01_0.IFO's ICB (block 21) is an indirect entry to block 40, which
    # holds the file entry.
    im = standard()
    im.put_lb(40, bytes(im.lb(21)))
    im.put_lb(21, indirect_entry(40, 0))
    assert mounted(im.data).open("/VIDEO_TS/VTS_01_0.IFO").size() == 101
    # Block 21 -> 40 -> 21.
    im = standard()
    im.put_lb(21, indirect_entry(40, 0))
    im.put_lb(40, indirect_entry(21, 0))
    assert mounted(im.data).open("/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA


def test_a_strategy_4096_chain_does_not_open():
    # NetBSD's direct strategy never sees the end of a 4096 chain: the
    # blank block after it reads as a tag-0 descriptor.
    im = standard()
    put16(im.lb(21), 0x14, 4096)
    tag(im.lb(21), 0x105, 0)
    assert mounted(im.data).open("/VIDEO_TS/VTS_01_0.IFO") == INVALIDDATA


def aed_image(n):
    """A file at ICB block 21 whose data is spread over n allocation extent
    descriptors (blocks 100..100+n), one data block each (blocks after
    them). Returns (image, first data block)."""
    data_lbn = 100 + n + 4
    part_len = data_lbn + n + 8
    im = Built(PART_START + part_len + 260)
    volume(im.data, "AED", [map_type1(0)], (0, 0), [(0, PART_START, part_len)], True)
    im.put_lb(0, fsd((1, 0)))
    root = directory((1, 0), [("F", 0, 21, 0)])
    im.put_lb(2, root)
    im.file(1, 4, len(root), 2)
    # File entry: one data block, then a redirect to AED 0.
    im.put_lb(21, file_entry(5, (n + 1) * 2048, 4, ("short", [(2048, data_lbn), ((3 << 30) | 2048, 100)])))
    for k in range(n):
        b = bytearray(S)
        ads = [(2048, data_lbn + 1 + k)]
        if k + 1 < n:
            ads.append(((3 << 30) | 2048, 101 + k))
        for i, (length, lbn) in enumerate(ads):
            put32(b, 24 + 8 * i, length)
            put32(b, 28 + 8 * i, lbn)
        put32(b, 20, 8 * len(ads))
        tag(b, 0x102, 0)
        im.put_lb(100 + k, b)
    for k in range(n + 1):
        im.lb(data_lbn + k)[0] = k % 256
    # the image size moves the end anchor: keep the one at 256 only
    return im, data_lbn


def test_allocation_extent_descriptors_and_their_limit():
    im, data_lbn = aed_image(3)
    v = mounted(im.data)
    f = v.open("/F")
    # One extent per allocation descriptor, even though the four blocks
    # are adjacent on the disc.
    assert f.extents() == [(PART_START + data_lbn + k, 1) for k in range(4)]
    assert f.read(3 * 2048, 1) == bytes([3])
    # 60 and 250 descriptors: within the limit of 250 (NetBSD: 50); 251 not.
    for n, ok in [(60, True), (250, True), (251, False)]:
        im, _ = aed_image(n)
        r = mounted(im.data).open("/F")
        assert (not isinstance(r, int)) == ok, f"{n} allocation extent descriptors"
        if ok:
            assert len(r.extents()) == n + 1


def vat_image(vat_at, with_vat):
    """A UDF image with a virtual partition (map 1 over physical map 0) and
    a UDF 2.00 VAT file at sector vat_at. Virtual block k = VAT entry k."""
    d = bytearray(520 * S)
    volume(d, "LVD_LABEL", [map_type1(0), map_type2("*UDF Virtual Partition", 0)], (0, 1),
           [(0, PART_START, 200)], False)

    def lb(lbn, data):
        at = (PART_START + lbn) * S
        d[at:at + len(data)] = data

    # Physical layout: FSD 10, root FE 11, root data 12, file FE 13, data 14.
    lb(10, fsd((1, 1)))
    root = directory((1, 1), [("F.TXT", 0, 3, 1)])
    lb(12, root)
    # Directory data through the virtual partition: short ADs take the
    # node's partition (1), so virtual block 2 -> physical 12.
    lb(11, file_entry(4, len(root), 4, ("short", [(len(root), 2)])))
    lb(13, file_entry(5, 5, 4, ("long", [(5, 14, 0)])))
    lb(14, b"virt!")
    if with_vat:
        # VAT data at absolute sector vat_at + 1: header (152 bytes, logical
        # volume identifier "VAT_LABEL"), entries 0..3 -> physical 10..13.
        vat = bytearray(152 + 16)
        put16(vat, 0, 152)
        dstring(vat, 4, 128, "VAT_LABEL")
        for k, p in enumerate([10, 11, 12, 13]):
            put32(vat, 152 + 4 * k, p)
        at = (vat_at + 1) * S
        d[at:at + len(vat)] = vat
        # VAT file entry (file type 248) at vat_at; it is read through the
        # raw map, so its short AD is an absolute sector.
        d[vat_at * S:(vat_at + 1) * S] = file_entry(248, len(vat), 4, ("short", [(len(vat), vat_at + 1)]))
    return d


def test_a_virtual_partition_reads_through_its_vat():
    v = mounted(vat_image(500, True))
    assert v.label_bytes() == b"VAT_LABEL", "the VAT's identifier replaces the descriptor's"
    f = v.open("/F.TXT")
    assert f.extents() == [(PART_START + 14, 1)]
    assert f.read(0, 5) == b"virt!"


def test_a_missing_vat_fails_the_mount_without_hanging():
    assert mount(vat_image(500, False)) == INVALIDDATA


def test_a_sparable_partition_reads_remapped_packets():
    d = bytearray(520 * S)
    m = map_type2("*UDF Sparable Partition", 0)
    put16(m, 40, 32)  # packet length
    m[42] = 1  # one sparing table
    put32(m, 44, 2048)
    put32(m, 48, 80)  # at sector 80
    volume(d, "SPARE", [m], (0, 0), [(0, PART_START, 128)], True)
    # Sparing table: packet 1 (blocks 32..63) is moved to sector 400.
    t = bytearray(S)
    put16(t, 48, 1)
    put32(t, 56, 1)
    put32(t, 60, 400)
    tag(t, 0, 0)
    d[80 * S:81 * S] = t

    def lb(absolute, data):
        d[absolute * S:absolute * S + len(data)] = data

    lb(PART_START, fsd((1, 0)))
    root = directory((1, 0), [("S", 0, 3, 0)])
    lb(PART_START + 2, root)
    lb(PART_START + 1, file_entry(4, len(root), 4, ("short", [(len(root), 2)])))
    lb(PART_START + 3, file_entry(5, 4, 4, ("short", [(4, 33)])))
    lb(PART_START + 33, b"orig")
    lb(401, b"good")
    f = mounted(d).open("/S")
    assert f.extents()[0][0] == 401
    assert f.read(0, 4) == b"good"


def metadata_image():
    """A UDF 2.50 image: metadata partition (map 1) over physical map 0; the
    metadata file (block 0) maps metadata blocks 0.. to physical 20..; the
    mirror file (block 1) maps them to physical 40..."""
    d = bytearray(520 * S)
    m = map_type2("*UDF Metadata Partition", 0)
    put32(m, 40, 0)  # metadata file
    put32(m, 44, 1)  # mirror file
    put32(m, 48, 0xffffffff)  # no bitmap
    volume(d, "META", [map_type1(0), m], (0, 1), [(0, PART_START, 128)], True)

    def lb(lbn, data):
        at = (PART_START + lbn) * S
        d[at:at + len(data)] = data

    lb(0, file_entry(250, 4 * 2048, 4, ("short", [(4 * 2048, 20)])))
    lb(1, file_entry(251, 4 * 2048, 4, ("short", [(4 * 2048, 40)])))
    # Metadata blocks: 0 FSD, 1 root FE, 2 root data, 3 file FE.
    root = directory((1, 1), [("M", 0, 3, 1)])
    f = file_entry(5, 4, 4, ("long", [(4, 60, 0)]))
    root_fe = file_entry(4, len(root), 4, ("short", [(len(root), 2)]))
    for base in [20, 40]:
        lb(base, fsd((1, 1)))
        lb(base + 1, root_fe)
        lb(base + 2, root)
        lb(base + 3, f)
    lb(60, b"meta")
    return d


def test_a_metadata_partition_reads_through_its_file_and_mirror():
    assert mounted(metadata_image()).open("/M").read(0, 4) == b"meta"
    # Main metadata file entry damaged: the mirror stands in.
    d = metadata_image()
    d[PART_START * S] ^= 0xff
    assert mounted(d).open("/M").read(0, 4) == b"meta"


def test_a_metadata_translation_loop_is_an_error_not_a_hang():
    # The metadata file maps its blocks into the metadata partition itself
    # (long AD, partition reference 1): block 0 -> 0 -> ...
    d = metadata_image()
    d[PART_START * S:(PART_START + 1) * S] = file_entry(250, 4 * 2048, 4, ("long", [(4 * 2048, 0, 1)]))
    d[(PART_START + 1) * S:(PART_START + 2) * S] = file_entry(251, 4 * 2048, 4, ("long", [(4 * 2048, 0, 1)]))
    assert isinstance(mount(d), int)


# ---------------------------------------------------------------------------
# Real discs: every image's dump equals the reference dump.

def dump(path, n):
    """The dump of one image, in the reference format: info, then a
    breadth-first tree walk with every listing, every regular file's extents
    and the sha256 of every file under 64 MiB. n: counts (dict)."""
    image = Image.open(path)
    fs = ffi.new("DiscIOFS **")
    if lib.ff_discio_udf_netbsd_mount(image.ptr, fs) < 0:
        return "@info 1\n"
    v = Vol(fs[0], image)
    out = [f"@info 0\nlabel {v.label()}\nrevision {v.revision():#06x}\nyear {v.year()}\n"]
    queue = collections.deque([b"/"])
    while queue:
        d = queue.popleft()
        n["listings"] += 1
        dtext = d.decode("utf-8", "replace")
        entries = v.list(d, raw=True)
        if isinstance(entries, int):
            out.append(f"@ls 1 {dtext}\n")
            continue
        body, files = [], []
        for name, is_dir in entries:
            text = name.decode("utf-8", "replace")
            child = d if d.endswith(b"/") else d + b"/"
            child += name
            if is_dir:
                body.append(f"d {text}\n")
                if name != b"..":
                    queue.append(child)
                continue
            f = v.open(child)
            if isinstance(f, int):
                body.append(f"o {text}\n" if f == averror(ENOENT) else f"f ?({f}) {text}\n")
            else:
                body.append(f"f {f.size()} {text}\n")
                files.append((child, f.size()))
        out.append(f"@ls 0 {dtext}\n")
        out += body
        for child, size in files:
            n["files"] += 1
            ftext = child.decode("utf-8", "replace")
            f = v.open(child)
            out.append(f"@ext 0 {ftext}\nsize {size}\n")
            for sector, count in f.extents():
                n["extents"] += 1
                out.append(f"- {count}\n" if sector < 0 else f"{sector} {count}\n")
            if size < 64 << 20:
                n["contents"] += 1
                data = f.read(0, size)
                if isinstance(data, int):
                    out.append(f"@sha 1 - {ftext}\n")
                else:
                    out.append(f"@sha 0 {hashlib.sha256(data).hexdigest()} {ftext}\n")
    return "".join(out)


def normalise(reference):
    """Exit codes other than 0 are written as 1 (failed)."""
    out = []
    for line in reference.splitlines():
        parts = line.split(" ", 2)
        if parts[0] in ("@info", "@ls", "@ext", "@sha") and len(parts) > 1 and parts[1] != "0":
            out.append(" ".join([parts[0], "1"] + parts[2:]))
        else:
            out.append(line)
    return "\n".join(out) + "\n"


@pytest.mark.disc
def test_corpus_dumps_equal_the_reference():
    folder = pathlib.Path(env_or_skip("DISCIO_UDF_REFERENCE"))
    index = (folder / "index.tsv").read_text()
    images, diffs, checks = 0, 0, 0
    total = collections.Counter()
    for line in index.splitlines():
        if not line:
            continue
        ident, path = line.split("\t", 1)
        if not os.path.exists(path):
            print(f"{ident}: {path} missing, skipped")
            continue
        reference = normalise((folder / f"{ident}.txt").read_text())
        n = collections.Counter(listings=0, files=0, extents=0, contents=0)
        ours = dump(path, n)
        images += 1
        a, b = reference.splitlines(), ours.splitlines()
        size = max(len(a), len(b))
        checks += size
        bad = [i for i in range(size) if (a[i] if i < len(a) else None) != (b[i] if i < len(b) else None)]
        if not bad:
            print(f"SAME {ident}: {n['listings']} listings, {n['files']} files, {n['extents']} extents, "
                  f"{n['contents']} contents")
        else:
            diffs += len(bad)
            print(f"DIFF {ident}: {len(bad)} of {size} lines")
            for i in bad[:8]:
                print(f"    line {i + 1}: reference {a[i] if i < len(a) else None!r} | "
                      f"ours {b[i] if i < len(b) else None!r}")
        total += n
    print(f"corpus: {images} images, {total['listings']} listings, {total['files']} files, "
          f"{total['extents']} extents, {total['contents']} contents compared, {checks} lines, "
          f"{diffs} differing lines")
    assert images > 0, f"no image in {folder}"
    assert diffs == 0, "the dumps differ from the reference"
