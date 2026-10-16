"""The DVD part's packet reader for its stream probe (disc/dvd/dvd_stream_probe.h,
DVDPackPacket_rec()) against the demuxer's (src/mux/ps_disc.c,
ff_mpegps_disc_packet(), with DVD-Video's 3-byte LPCM audio frame header):
the same packets, offsets, keys and reasons on built sectors of every kind,
on those sectors with damaged bytes, and on random bytes after a pack
header. The stream probe finds a title's streams by the first; the demuxer
reads them by the second: they must agree."""

import random

from helpers import ffi, lib
from synth import dvd
from synth.dvd import S, _pts, _sector
from test_dvd_read import ac3_pack, pack, video_pack

FIELDS = ("at", "len", "key", "payload", "first_au", "extra")


def walk(sector, reader):
    """Every packet of the sector as reader reads them, until it is done or
    one does not parse: (parsed, next offset, the packet's fields or why)."""
    buf = ffi.new("uint8_t[]", bytes(sector))
    off = ffi.new("int *", 0)
    out = []
    for _ in range(16):
        if reader == "dvdread":
            p = ffi.new("dvd_pack_packet_t *")
            ok = lib.DVDPackPacket_rec(buf, off, p)
        else:
            p = ffi.new("MpegPsDiscPacket *")
            ok = lib.ff_mpegps_disc_packet(buf, off, p, 0)
        if not ok:
            out.append((0, ffi.string(p.why).decode() if p.why else None))
            break
        out.append((1, off[0]) + tuple(getattr(p, f) for f in FIELDS))
        if off[0] >= S:
            break
    return out


def same(sector):
    a, b = walk(sector, "dvdread"), walk(sector, "mpeg")
    assert a == b, (bytes(sector[:48]).hex(), a, b)
    return a


def lpcm_pack(body, first_au=1):
    """Private stream 1 LPCM (sub-stream 0xA0): its 4-byte sub-stream header,
    the 3-byte audio frame header, body."""
    return _sector(0xbd, _pts(0), bytes([0xa0, 1, first_au >> 8, first_au & 0xff, 0, 0x01, 0x80]) + body)


def built():
    """Sectors of every kind the probe meets."""
    v = bytearray(video_pack())
    v[20] &= 0xcf
    out = [ac3_pack(), video_pack(), bytes(v), dvd.nav(0, 10, None).block(),
           _sector(0xc0, _pts(0), bytes(1500)),                          # MPEG audio
           _sector(0xbd, _pts(0), bytes([0x88, 1, 0, 1]) + bytes(1000)),  # DTS
           _sector(0xbd, _pts(0), bytes([0x20]) + bytes(800)),            # sub-picture
           _sector(0xbd, b"", bytes([0xb0, 0, 0, 3, 0]) + bytes(900)),    # MLP
           lpcm_pack(bytes(1600)),
           lpcm_pack(b""),                                                # only its audio frame header
           lpcm_pack(bytes(100), first_au=2),                             # first frame inside the header
           pack(0xe0, bytes(2000), False),
           bytes(S), bytes([0xff]) * S]
    # padding near the end followed by a packet, and by fill bytes
    s = bytearray(_sector(0xe0, _pts(0), bytes(1530)))
    out.append(bytes(s))
    s[0x7f0:0x7f6] = bytes([0, 0, 1, 0xbe, 0, 4])
    out.append(bytes(s))
    # stuffing in the pack header
    t = bytearray(ac3_pack())
    t[13] = (t[13] & 0xf8) | 2
    t = t[:14] + b"\xff\xff" + t[14:-2]
    out.append(bytes(t))
    return out


def test_both_readers_read_built_sectors_alike():
    results = [same(s) for s in built()]
    assert any(r[0][0] and r[0][4] == 0x80bd for r in results), "an AC-3 packet was read"
    assert any(r and r[0][0] == 0 and "LPCM" in r[0][1] for r in results), "an LPCM packet that does not parse"


def test_both_readers_read_damaged_sectors_alike():
    rng = random.Random(20261011)
    base = built()
    for _ in range(4000):
        s = bytearray(rng.choice(base))
        for _ in range(rng.randint(1, 6)):
            at = rng.randrange(64) if rng.random() < 0.7 else rng.randrange(S)
            s[at] = rng.randrange(256)
        same(s)


def test_both_readers_read_random_packets_alike():
    rng = random.Random(7)
    head = ac3_pack()[:14]
    for _ in range(2000):
        s = bytearray(head + bytes(rng.randrange(256) for _ in range(S - 14)))
        at = 14
        while at < S - 6 and rng.random() < 0.8:       # some packet starts at random places
            s[at:at + 4] = bytes([0, 0, 1, rng.choice([0xbd, 0xbe, 0xbf, 0xc0, 0xe0, 0xfd, 0xba])])
            at += rng.randrange(6, 700)
        same(s)
