"""Disc images built byte by byte: ISO 9660 / Joliet trees (ECMA-119) and a
UDF 1.02 volume (ECMA-167 / OSTA UDF) over the same sectors, as on
DVD-Video bridge discs."""

import struct

S = 2048


# ---- little helpers ----

def put16(b, at, v):
    b[at:at + 2] = struct.pack("<H", v)


def put32(b, at, v):
    b[at:at + 4] = struct.pack("<I", v)


def put64(b, at, v):
    b[at:at + 8] = struct.pack("<Q", v)


def both32(b, at, v):
    """ECMA-119 both-byte-order 32-bit field."""
    b[at:at + 8] = struct.pack("<I", v) + struct.pack(">I", v)


def ucs2(s):
    return s.encode("utf-16-be")


# ---- ISO 9660 / Joliet ----

def record(ident, flags, extent, size):
    """A directory record: identifier bytes, flags, extent, size."""
    length = (33 + len(ident) + 1) & ~1
    r = bytearray(length)
    r[0] = length
    both32(r, 2, extent)
    both32(r, 10, size)
    r[25] = flags
    r[32] = len(ident)
    r[33:33 + len(ident)] = ident
    return r


# Sectors of the ISO 9660 tree (root, then one per directory) and of the
# Joliet tree.
ISO_DIRS = 24
JOLIET_DIRS = 26
# UDF partition: start sector and length.
PART_START = 300
PART_LEN = 200
# VMG sectors used by the DVD trees (absolute sectors, inside the partition
# so both file systems can point at them).
VMG = PART_START + 30
BUP = PART_START + 32
VTS1 = PART_START + 34
# A sector that holds no VMG.
NO_VMG = PART_START + 40


class Img:
    """An image of 520 sectors; bad: sectors that fail to read (used by the
    test sources)."""

    def __init__(self):
        self.d = bytearray(520 * S)
        self.bad = []

    def sector(self, n):
        """A writable view of sector n."""
        return memoryview(self.d)[n * S:(n + 1) * S]

    def put(self, n, data):
        self.d[n * S:n * S + len(data)] = data

    def vmg(self, n, ident, title_sets):
        """A VMG sector: identifier and number of title sets."""
        v = self.sector(n)
        v[:12] = ident
        v[0x3e:0x40] = struct.pack(">H", title_sets)

    def volume_descriptor(self, n, kind, label, root):
        v = self.sector(n)
        v[0] = kind
        v[1:6] = b"CD001"
        v[6] = 1
        v[40:72] = (b"\0" if kind == 2 else b" ") * 32
        v[40:40 + len(label)] = label
        v[128:130] = struct.pack("<H", 2048)
        r = record(b"\0", 2, root, 2048)
        v[156:156 + len(r)] = r

    def tree(self, base, dirs, name):
        """One tree: root at base, directory k at base + 1 + k. dirs:
        [(directory, [(file, first sector, bytes)])]."""
        root = record(b"\0", 2, base, 2048) + record(b"\1", 2, base, 2048)
        for k, (directory, files) in enumerate(dirs):
            at = base + 1 + k
            root += record(name(directory), 2, at, 2048)
            d = record(b"\0", 2, at, 2048) + record(b"\1", 2, base, 2048)
            for f, sector, size in files:
                d += record(name(f"{f};1"), 0, sector, size)
            self.put(at, d)
        self.put(base, root)


def iso(im, label, dirs, joliet=None):
    """The ISO 9660 descriptors (PVD at 16, SVD at 17 with Joliet given as
    (label, dirs), then the terminator); returns the sector after them."""
    im.volume_descriptor(16, 1, label.encode(), ISO_DIRS)
    im.tree(ISO_DIRS, dirs, lambda s: s.encode())
    n = 17
    if joliet is not None:
        jlabel, jdirs = joliet
        im.volume_descriptor(17, 2, ucs2(jlabel), JOLIET_DIRS)
        im.tree(JOLIET_DIRS, jdirs, ucs2)
        n = 18
    t = im.sector(n)
    t[0] = 0xff
    t[1:6] = b"CD001"
    t[6] = 1
    return n + 1


# ---- UDF ----

def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def tag(b, ident, loc, crclen):
    """A descriptor tag: id, version 2, location, CRC over crclen bytes."""
    put16(b, 0, ident)
    put16(b, 2, 2)
    put32(b, 12, loc)
    put16(b, 10, crclen)
    put16(b, 8, crc16(bytes(b[16:16 + crclen])))
    b[4] = 0
    b[4] = sum(x for i, x in enumerate(b[:16]) if i != 4) & 0xff


def dstring(b, at, size, text):
    b[at] = 8
    b[at + 1:at + 1 + len(text)] = text.encode()
    b[at + size - 1] = 1 + len(text)


def osta(b, at):
    b[at] = 0
    b[at + 1:at + 24] = b"OSTA Compressed Unicode"


class Udf:
    """What the UDF volume of a built image records. files: files of
    VIDEO_TS (name, absolute sector, bytes), inside the partition; pieces:
    files of VIDEO_TS recorded in several pieces (name, [(absolute sector,
    bytes)])."""

    def __init__(self, label, revision, year, files, pieces=()):
        self.label = label
        self.revision = revision
        self.year = year
        self.files = list(files)
        self.pieces = list(pieces)


def file_entry_ads(im, lbn, file_type, size, ads):
    """A file entry at partition block lbn with short allocation descriptors
    [(bytes, partition block)]."""
    b = bytearray(S)
    put16(b, 0x14, 4)
    b[0x1b] = file_type
    put16(b, 0x30, 1)  # link count
    put64(b, 0x38, size)
    for i, (length, at) in enumerate(ads):
        put32(b, 0xb0 + 8 * i, length)
        put32(b, 0xb4 + 8 * i, at)
    put32(b, 0xac, 8 * len(ads))
    tag(b, 0x105, lbn, 176 + 8 * len(ads) - 16)
    im.put(PART_START + lbn, b)


def file_entry(im, lbn, file_type, size, data_lbn):
    """A file entry at partition block lbn with one short allocation
    descriptor."""
    file_entry_ads(im, lbn, file_type, size, [(size, data_lbn)])


def directory(im, lbn, parent, entries):
    """Directory data at partition block lbn: the parent entry, then
    (name, characteristics, file entry block); returns its length."""
    out = bytearray()
    for name, chars, icb in [("", 0x0a, parent)] + list(entries):
        raw = b"" if not name else b"\x08" + name.encode()
        length = (38 + len(raw) + 3) & ~3
        b = bytearray(length)
        put16(b, 0x10, 1)
        b[0x12] = chars
        b[0x13] = len(raw)
        put32(b, 0x14, 2048)
        put32(b, 0x18, icb)
        b[0x26:0x26 + len(raw)] = raw
        tag(b, 0x101, lbn, length - 16)
        out += b
    im.put(PART_START + lbn, out)
    return len(out)


def udf(im, vrs, u):
    """Volume recognition sequence from sector vrs, anchor at 256, main
    volume descriptor sequence at 32, reserve at 48, integrity at 64; file
    set at block 0, root (VIDEO_TS only) at 1-2, VIDEO_TS at 3 and 5, file
    entries from block 20."""
    for i, ident in enumerate([b"BEA01", b"NSR02", b"TEA01"]):
        v = im.sector(vrs + i)
        v[1:6] = ident
        v[6] = 1
    a = im.sector(256)
    put32(a, 0x10, 16 * 2048)
    put32(a, 0x14, 32)
    put32(a, 0x18, 16 * 2048)
    put32(a, 0x1c, 48)
    tag(a, 2, 256, 496)
    p = im.sector(32)
    put16(p, 0x178 + 2, u.year)
    tag(p, 1, 32, 496)
    iu = im.sector(33)
    iu[0x15:0x21] = b"*UDF LV Info"
    put16(iu, 0x2c, u.revision)
    tag(iu, 4, 33, 496)
    pd = im.sector(34)
    pd[0x14] = 1
    pd[0x19:0x1f] = b"+NSR02"
    put32(pd, 0xb8, 1)  # read-only access
    put32(pd, 0xbc, PART_START)
    put32(pd, 0xc0, PART_LEN)
    tag(pd, 5, 34, 496)
    lv = im.sector(35)
    put32(lv, 0x10, 1)
    osta(lv, 0x14)
    dstring(lv, 0x54, 128, u.label)
    put32(lv, 0xd4, 2048)
    lv[0xd9:0xd9 + 19] = b"*OSTA UDF Compliant"
    put32(lv, 0xf8, 2048)
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
        ident = struct.unpack_from("<H", d, 0)[0]
        crclen = struct.unpack_from("<H", d, 10)[0]
        tag(d, ident, s + 16, crclen)
        im.sector(s + 16)[:] = d
    v = im.sector(64)
    put32(v, 0x1c, 1)
    put32(v, 0x48, 1)
    put32(v, 0x4c, 46)
    put16(v, 80 + 8 + 40, 0x0102)
    tag(v, 9, 64, 80 + 8 + 46 - 16)
    tag(im.sector(65), 8, 65, 0)

    f = bytearray(S)
    put32(f, 0x190, 2048)
    put32(f, 0x194, 1)
    f[0x1a1:0x1a1 + 19] = b"*OSTA UDF Compliant"
    tag(f, 0x100, 0, 496)
    im.put(PART_START, f)
    root = directory(im, 2, 1, [("VIDEO_TS", 0x02, 3)])
    file_entry(im, 1, 4, root, 2)
    names = [x[0] for x in u.files] + [x[0] for x in u.pieces]
    vt = directory(im, 5, 1, [(n, 0, 20 + k) for k, n in enumerate(names)])
    file_entry(im, 3, 4, vt, 5)
    for k, (_, sector, size) in enumerate(u.files):
        file_entry(im, 20 + k, 5, size, sector - PART_START)
    for k, (_, pieces) in enumerate(u.pieces):
        size = sum(p[1] for p in pieces)
        ads = [(nbytes, sector - PART_START) for sector, nbytes in pieces]
        file_entry_ads(im, 20 + len(u.files) + k, 5, size, ads)


# ---- the DVD-Video trees ----

def dvd_files():
    """VIDEO_TS of a valid DVD-Video volume."""
    return [("VIDEO_TS.IFO", VMG, 4096), ("VIDEO_TS.BUP", BUP, 4096), ("VTS_01_0.IFO", VTS1, 4096)]


def broken_files():
    """VIDEO_TS whose IFO points at a sector without a VMG."""
    return [("VIDEO_TS.IFO", NO_VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)]


def bridge(u, iso_files, joliet_files=None):
    """A bridge image: the UDF volume u (if any), an ISO 9660 tree with
    iso_files labelled ISO_LABEL, and a Joliet tree with joliet_files
    labelled JOLIET_LABEL (if given). The VMG sectors are valid."""
    im = Img()
    dirs = [("VIDEO_TS", iso_files)]
    joliet = None if joliet_files is None else ("JOLIET_LABEL", [("VIDEO_TS", joliet_files)])
    nxt = iso(im, "ISO_LABEL", dirs, joliet)
    if u is not None:
        udf(im, nxt, u)
    im.vmg(VMG, b"DVDVIDEO-VMG", 1)
    im.vmg(BUP, b"DVDVIDEO-VMG", 1)
    return im


def udf102(year, files):
    return Udf("UDF_DISC_LABEL", 0x0102, year, files)
