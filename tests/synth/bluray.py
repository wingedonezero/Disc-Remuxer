"""Blu-ray disc folders built for the tests, byte by byte in the published
layouts (BD-ROM: index.bdmv, MovieObject.bdmv, id.bdmv, .mpls playlists,
.clpi clip information; BD-RE / BDAV: info.bdav, .rpls playlists; AVCHD
folders), written into a directory."""

import dataclasses
import pathlib
import struct


def be16(v):
    return struct.pack(">H", v)


def be32(v):
    return struct.pack(">I", v)


def header(kind, version="0200"):
    return kind.encode() + version.encode()


# ---- playlists ----

@dataclasses.dataclass
class Stream:
    """An STN stream: entry type 1 (a PID of the item's clip) or another
    entry type with its bytes; coding type and attribute bytes."""
    pid: int = 0x1011
    coding: int = 0x1b
    attr: bytes = b"\x61"
    entry_type: int = 1
    entry: bytes = None

    def bytes(self):
        e = self.entry if self.entry is not None else be16(self.pid) + bytes(6)
        e = bytes([self.entry_type]) + e
        a = bytes([self.coding]) + self.attr
        return bytes([len(e)]) + e + bytes([len(a)]) + a


@dataclasses.dataclass
class Item:
    clip: str = "00001"
    in_time: int = 0
    out_time: int = 45000 * 60
    connection: int = 1
    stc_id: int = 0
    angles: list = None          # [(clip, stc_id)] further angles
    videos: list = None          # default: one video stream
    audio: list = dataclasses.field(default_factory=list)
    pg: list = dataclasses.field(default_factory=list)

    def bytes(self):
        multi = bool(self.angles)
        b = self.clip.encode() + b"M2TS"
        b += bytes([0, (0x10 if multi else 0) | self.connection, self.stc_id])
        b += be32(self.in_time) + be32(self.out_time) + bytes(8) + bytes(4)
        if multi:
            b += bytes([len(self.angles) + 1, 0])
            for c, stc in self.angles:
                b += c.encode() + b"M2TS" + bytes([stc])
        videos = self.videos if self.videos is not None else [Stream()]
        stn = bytes(2) + bytes([len(videos), len(self.audio), len(self.pg), 0, 0, 0, 0]) + bytes(5)
        for s in videos + self.audio + self.pg:
            stn += s.bytes()
        b += be16(len(stn)) + stn
        return be16(len(b)) + b


@dataclasses.dataclass
class SubPath:
    type: int = 5
    clips: list = dataclasses.field(default_factory=lambda: ["00002"])

    def bytes(self):
        items = b""
        for c in self.clips:
            it = c.encode() + b"M2TS" + bytes([0, 0, 0, 0x02, 0]) + be32(0) + be32(45000) + be16(0) + be32(0)
            items += be16(len(it)) + it
        b = bytes([0, self.type, 0, 0, 0, len(self.clips)]) + items
        return be32(len(b)) + b


def ext_block(entries):
    """ExtensionData: entries [(id1, id2, data)]."""
    n = len(entries)
    start = 12 + 12 * n
    head, data = b"", b""
    for id1, id2, d in entries:
        head += be16(id1) + be16(id2) + be32(start + len(data)) + be32(len(d))
        data += d
    body = be32(start) + bytes(3) + bytes([n]) + head + data
    return be32(len(body)) + body


def mpls(items, subpaths=(), marks=(), ext=None, playback_type=1, version="0200"):
    app = bytes([0, playback_type]) + be16(0) + bytes(8) + be16(0x4000)
    app = be32(len(app)) + app
    pl_body = bytes(2) + be16(len(items)) + be16(len(subpaths))
    pl_body += b"".join(i.bytes() for i in items) + b"".join(s.bytes() for s in subpaths)
    playlist = be32(len(pl_body)) + pl_body
    mk = b"".join(bytes([0, t]) + be16(item) + be32(time) + be16(0xffff) + be32(0) for t, item, time in marks)
    mark_block = be32(len(mk) + 2) + be16(len(marks)) + mk
    p = 0x28 + len(app)
    m = p + len(playlist)
    x = m + len(mark_block) if ext is not None else 0
    data = header("MPLS", version) + be32(p) + be32(m) + be32(x) + bytes(20) + app + playlist + mark_block
    if ext is not None:
        data += ext_block(ext)
    return data


# ---- clip information ----

def clpi(packets=1000, stcs=((0, 0x1001, 0, 27000000, 27000000 + 90000),), streams=((0x1011, 0x1b, b"\x61"),),
         ts_info_len=0x1e, kind="HDMV", version="0200", stream_type=1, ext=None, atcs=None):
    """stcs: (spn_stc_start, pcr_pid, ...) per STC of one ATC sequence, or
    atcs: [(spn_atc_start, offset_stc_id, [stcs])]."""
    info = bytes(2) + bytes([stream_type, 1, 0, 0, 0, 0]) + be32(48000000) + be32(packets) + bytes(128)
    info += be16(ts_info_len) + bytes([0x80]) + b"HDMV" + bytes(ts_info_len - 5)
    info = be32(len(info)) + info
    atcs = atcs if atcs is not None else [(0, 0, stcs)]
    seq = bytes([0, len(atcs)])
    for spn, off, ss in atcs:
        seq += be32(spn) + bytes([len(ss), off])
        for s in ss:
            spn_stc, pid, start, end = s[0], s[1], s[2], s[3]
            seq += be16(pid) + be32(spn_stc) + be32(start) + be32(end)
    seq = be32(len(seq)) + seq
    prog = bytes([0, 1]) + be32(0) + be16(0x100) + bytes([len(streams), 0])
    for pid, coding, attr in streams:
        ci = bytes([coding]) + attr
        prog += be16(pid) + bytes([len(ci)]) + ci
    prog = be32(len(prog)) + prog
    cpi = be32(4) + be16(1) + bytes(2)
    s = 0x28 + len(info)
    p = s + len(seq)
    c = p + len(prog)
    x = c + len(cpi) if ext is not None else 0
    data = header(kind, version) + be32(s) + be32(p) + be32(c) + be32(0) + be32(x) + bytes(12)
    data += info + seq + prog + cpi
    if ext is not None:
        data += ext_block(ext)
    return data


# ---- index, id ----

def index_entry(obj_type, ref, playback=0):
    e = bytes([obj_type << 6, 0, 0, 0, playback << 6, 0])
    if obj_type == 1:
        e += be16(ref) + bytes(4)
    elif obj_type == 2:
        e += b"%05d" % ref + b"\x00"
    else:
        e += bytes(6)
    return e


def index(titles=((1, 0),), version="0200"):
    body = index_entry(1, 0) + index_entry(1, 1) + be16(len(titles))
    for t, r in titles:
        body += index_entry(t, r)
    indexes = be32(len(body)) + body
    return header("INDX", version) + be32(0x28) + be32(0) + bytes(24) + indexes


def disc_id(org=0x7fff1234, did=bytes(range(16))):
    return header("BDID") + bytes(0x20) + be32(org) + did + bytes(8)


# ---- BDAV ----

def info_bdav(name=b"", charset=0x10, size=0x140):
    b = bytearray(size)
    b[0:8] = header("BDAV")
    b[0x28:0x2c] = b"\x00\x00\x01\x14"
    b[0x2e] = charset
    b[0x40] = len(name)
    b[0x41:0x41 + len(name)] = name
    return bytes(b)


def rpls(items, playlist_type=1, name=b""):
    """items: [(clip, stc_id, in, out)]"""
    b = bytearray(0x158)
    b[0:8] = header("PLST")
    b[8:12] = be32(0x158)
    b[0x58] = len(name)
    b[0x59:0x59 + len(name)] = name
    body = bytes([0, playlist_type]) + be16(len(items)) + bytes(2)
    for clip, stc, i, o in items:
        it = clip.encode() + b"M2TS" + bytes([0, 1, stc]) + be32(i) + be32(o)
        body += be16(len(it)) + it
    return bytes(b) + be32(len(body)) + body


# ---- disc library ----

def bdmt(name, lang="eng", ns=True):
    p = "di:" if ns else ""
    return (f'<?xml version="1.0" encoding="utf-8"?>\n<disclib xmlns:di="urn:BDA:bdmv;discinfo">'
            f'<{p}discinfo><{p}title><{p}name>{name}</{p}name></{p}title>'
            f'<{p}language>{lang}</{p}language></{p}discinfo></disclib>').encode()


# ---- a disc folder ----

def write(root, files):
    """files: {path: bytes}, paths relative to root."""
    root = pathlib.Path(root)
    for name, data in files.items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return root


def bdmv(clips=None, playlists=None, extra=None, stream_files=True, root="BDMV"):
    """The files of a BDMV disc: clips {num: clpi bytes}, playlists {num: mpls bytes}."""
    clips = clips if clips is not None else {1: clpi()}
    playlists = playlists if playlists is not None else {1: mpls([Item()])}
    f = {f"{root}/index.bdmv": index(), f"{root}/MovieObject.bdmv": header("MOBJ")}
    for n, d in clips.items():
        f[f"{root}/CLIPINF/{n:05d}.clpi"] = d
        if stream_files:
            f[f"{root}/STREAM/{n:05d}.m2ts"] = bytes(192)
    for n, d in playlists.items():
        f[f"{root}/PLAYLIST/{n:05d}.mpls"] = d
    f.update(extra or {})
    return f
