"""HD DVD images built for the tests: an ISO 9660 tree with ADV_OBJ (the
playlist) and HVDVD_TS (the Advanced VTS information file, time maps and EVO
files); packs, frames and the AACS encryption of a pack."""

import dataclasses
import struct

from synth.image import Img, iso

S = 2048


def be16(v):
    return struct.pack(">H", v)


def be32(v):
    return struct.pack(">I", v)


@dataclasses.dataclass
class Evob:
    """An EVOB record of the VTI: file name, slot, start / end time (90 kHz)."""
    name: str
    slot: int
    start: int
    end: int


def evob(name, slot, secs):
    return Evob(name, slot, 1000, 1000 + secs * 90000)


def vti(evobs):
    """The VTI: one attribute record, the EVOB records."""
    return vti_with(evobs, [1] * len(evobs), [bytes(0x206)])


def vti_with(evobs, attr_of, attrs):
    """The VTI with attribute records attrs (0x206 bytes each) and, per EVOB,
    its attribute record (1-based)."""
    a = bytearray(8 + 4 * len(attrs))
    a[0:2] = be16(len(attrs))
    for i, r in enumerate(attrs):
        a[8 + 4 * i:12 + 4 * i] = be32(len(a))
        assert len(r) == 0x206
        a += r
    b = bytearray(8 + 4 * len(evobs))
    b[0:4] = be32(len(evobs))
    for i, e in enumerate(evobs):
        b[8 + 4 * i:12 + 4 * i] = be32(len(b))
        r = bytearray(0x140)
        r[2:2 + len(e.name)] = e.name.encode()
        r[0x106:0x10a] = be32(attr_of[i])
        r[0x10a:0x10e] = be32(e.start)
        r[0x10e:0x112] = be32(e.end)
        r[0x112:0x116] = be32(1)
        r[0x116:0x118] = be16(e.slot)
        b += r
    f = bytearray(S)
    f[:12] = b"ADVANCED-VTS"
    f[0xb8:0xbc] = be32(1)
    blocks_a = (len(a) + S - 1) // S
    f[0xbc:0xc0] = be32(1 + blocks_a)
    a += bytes(blocks_a * S - len(a))
    return bytes(f + a + b)


def attr_record(video, audio, subs, palette):
    """An attribute record: video attribute bytes 0x02..0x04, audio entries
    (first byte each), sub-picture entries (5 bytes each), 32 palette words."""
    r = bytearray(0x206)
    r[2:5] = bytes(video)
    r[0x0e:0x10] = be16(len(audio))
    for j, a in enumerate(audio):
        r[0x10 + 4 * j] = a
    r[0xe4:0xe6] = be16(len(subs))
    for j, p in enumerate(subs):
        r[0xe6 + 5 * j:0xeb + 5 * j] = bytes(p)
    for j, w in enumerate(palette):
        r[0x186 + 4 * j:0x18a + 4 * j] = be32(w)
    return bytes(r)


def tmap_blocks(blocks):
    """A time map of blocks blocks (entries of at most 8000 blocks)."""
    counts = []
    left = blocks
    while left > 0:
        n = min(left, 8000)
        counts.append(n)
        left -= n
    return tmap(counts, 0, 0, False)


def tmap(counts, value, byte14, two):
    """A time map: its first table's block counts (each entry's last u16),
    the first table's value, header byte 0x14, and a second table when two."""
    tables = 2 if two else 1
    m = bytearray(0x200)
    m[:12] = b"HDDVD_TMAP00"
    m[0x14] = byte14
    m[0x37:0x39] = be16(tables)
    for t in range(tables):
        d = 0x180 + 0x20 * t
        m[d:d + 4] = be32(len(m))
        m[d + 6:d + 8] = be16(len(counts))
        m[d + 8:d + 10] = be16(value if t == 0 else 1)
        for c in counts:
            m += b"\xab\xcd"  # first u16: not used
            m += be16(c | 0xe000)  # top 3 bits not counted
    return bytes(m)


def pack_header():
    s = bytearray(2048)
    s[:4] = bytes([0, 0, 1, 0xba])
    s[4] = 0x44
    s[0xd] = 0xf8  # no stuffing
    return s


def pack(packets):
    """A pack: pack header, the packets, then a padding packet over the rest."""
    s = pack_header()
    at = 14
    for p in packets:
        s[at:at + len(p)] = p
        at += len(p)
    rem = 2048 - at
    if rem > 0:
        assert rem >= 6, f"{rem} bytes left: no room for a padding packet"
        s[at:at + 4] = bytes([0, 0, 1, 0xbe])
        s[at + 4:at + 6] = be16(rem - 6)
        s[at + 6:] = b"\xff" * (rem - 6)
    return bytes(s)


def ps1_packet(sub, pts, payload):
    """A private stream 1 packet: sub-stream id, PTS (90 kHz) when given, the
    4-byte audio sub-stream header (1 frame, first access unit at 1),
    payload."""
    if pts is not None:
        t = pts
        hdr = bytes([0x81, 0x80, 5, 0x21 | ((t >> 29) & 0x0e), (t >> 22) & 0xff, 0x01 | ((t >> 14) & 0xfe),
                     (t >> 7) & 0xff, 0x01 | ((t << 1) & 0xfe)])
    else:
        hdr = bytes([0x81, 0x00, 0])
    length = len(hdr) + 4 + len(payload)
    return bytes([0, 0, 1, 0xbd]) + be16(length) + hdr + bytes([sub, 1, 0, 1]) + bytes(payload)


def video_packet(length):
    """A video packet (stream 0xe0, no PTS) of length bytes in all."""
    p = bytearray(length)
    p[:4] = bytes([0, 0, 1, 0xe0])
    p[4:6] = be16(length - 6)
    p[6] = 0x81
    return bytes(p)


def eac3_frame(size, strmtyp, chanmap):
    """An E-AC-3 frame of size bytes (48 kHz, 6 blocks, 2/0): stream type,
    and for a dependent stream the channel map."""
    f = bytearray(size)
    w = (strmtyp << 14) | (size // 2 - 1)
    f[0] = 0x0b
    f[1] = 0x77
    f[2:4] = be16(w)
    f[4] = (3 << 4) | (2 << 1)  # fscod 0, numblkscod 3, acmod 2, lfeon 0
    f[5] = 16 << 3  # bsid 16, dialnorm 0
    if chanmap is not None:
        # byte 6: dialnorm (2 bits), compre 0, chanmape 1, chanmap bits 15..12
        hi, lo = chanmap >> 8, chanmap & 0xff
        f[6] = 0x10 | (hi >> 4)
        f[7] = ((hi << 4) | (lo >> 4)) & 0xff
        f[8] = (lo << 4) & 0xff
    return bytes(f)


def ac3_frame():
    """An AC-3 frame: 48 kHz, 128 kb/s (512 bytes), bsid 8."""
    f = bytearray(512)
    f[0] = 0x0b
    f[1] = 0x77
    f[4] = 16  # fscod 0, frmsizecod 16 (128 kb/s)
    f[5] = 8 << 3
    return bytes(f)


def image(files):
    """The image: every file (folder, name, content) from sector 100 on, in
    its folder."""
    im = Img()
    nxt = 100
    dirs = []
    for d, name, data in files:
        blocks = max((len(data) + S - 1) // S, 1)
        end = (nxt + blocks) * S
        if len(im.d) < end:
            im.d += bytes(end - len(im.d))
        im.put(nxt, data)
        entry = (name, nxt, len(data))
        for dd in dirs:
            if dd[0] == d:
                dd[1].append(entry)
                break
        else:
            dirs.append((d, [entry]))
        nxt += blocks
    iso(im, "HDDVD", dirs)
    return im


def clip(m, begin, end, seamless):
    """A PrimaryAudioVideoClip element."""
    return (f'<PrimaryAudioVideoClip src="file:///dvddisc/HVDVD_TS/{m}" titleTimeBegin="{begin}" '
            f'titleTimeEnd="{end}" seamless="{"true" if seamless else "false"}"/>')


def playlist(title_set_attrs, body):
    """A playlist with one TitleSet holding body."""
    return (f'<?xml version="1.0"?><Playlist><TitleSet {title_set_attrs}>{body}</TitleSet></Playlist>').encode()


# ---- AACS ----

AACS_IV = bytes([0x0b, 0xa0, 0xf8, 0xdd, 0xfe, 0xa6, 0x1f, 0xb3, 0xd8, 0xdf, 0x9f, 0x56, 0x6a, 0x05, 0x0f, 0x78])


def aes(key, data, decrypt, iv=None):
    """AES-128 of data (whole blocks) with libavutil: ECB, or CBC with iv."""
    from helpers import ffi, lib
    out = ffi.new("uint8_t[]", len(data))
    a = lib.av_aes_alloc()
    assert lib.av_aes_init(a, bytes(key), 128, int(decrypt)) == 0
    ivp = ffi.new("uint8_t[16]", bytes(iv)) if iv is not None else ffi.NULL
    lib.av_aes_crypt(a, out, bytes(data), len(data) // 16, ivp, int(decrypt))
    lib.av_free(a)
    return bytes(out)


def aes_e(key, block):
    return aes(key, block, False)


def aes_d(key, block):
    return aes(key, block, True)


def aes_g(key, data):
    """AES-G: AES-128D(key, data) xor data."""
    return bytes(a ^ b for a, b in zip(aes_d(key, data), data))


def hexs(b):
    return bytes(b).hex().upper()


def tkf(vuk, keys):
    """A title key file: 64 title keys encrypted with vuk."""
    f = bytearray(0x9b0)
    f[:12] = b"DVD_HD_V_TKF"
    f[12:16] = be32(0x9b0)
    for j, k in enumerate(keys):
        f[0x84 + 0x24 * j:0x94 + 0x24 * j] = aes_e(vuk, k)
    return bytes(f)


def title_keys():
    return [bytes(((j * 16 + i) % 251) ^ 0x5a for i in range(16)) for j in range(64)]


def nav_pack(mode_bits, key_offset, seed):
    """A navigation pack: key mode bits (top two bits of byte 0x3c), title
    key id offset, 12 seed bytes."""
    s = pack_header()
    s[14:18] = bytes([0, 0, 1, 0xbb])
    s[18:20] = be16(0x15)  # ends at 0x29
    s[0x29:0x30] = bytes([0, 0, 1, 0xbf, 0x01, 0x01, 0x04])
    s[0x130:0x137] = bytes([0, 0, 1, 0xbf, 0x03, 0xd1, 0x00])
    s[0x3c] = (mode_bits << 6) & 0xff
    s[0x3e] = key_offset
    s[0x40:0x4c] = bytes(seed)
    s[0x507:0x50e] = bytes([0, 0, 1, 0xbf, 0x02, 0xf3, 0x01])
    return bytes(s)


def data_pack(sid, scr, fill):
    """A pack with one PES packet of stream sid (scrambling bits scr), its
    payload a counting pattern from fill."""
    s = pack_header()
    s[14:18] = bytes([0, 0, 1, sid])
    s[18:20] = be16(2048 - 20)
    s[20] = 0x80 | (scr << 4)
    s[22] = 0
    for i in range(23, 2048):
        s[i] = (fill + (i - 23) % 256) & 0xff
    return bytes(s)


def encrypt_pack(plain, key, seed):
    """Encrypt a pack as an HD DVD disc holds it (bytes 0x80.. AES-128-CBC
    with AES-G(title key, bytes 0x54..0x57 + seed))."""
    bk = aes_g(key, bytes(plain[0x54:0x58]) + bytes(seed))
    return bytes(plain[:0x80]) + aes(bk, plain[0x80:], False, AACS_IV)
