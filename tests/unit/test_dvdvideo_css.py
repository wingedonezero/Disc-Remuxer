"""libavformat/dvdvideo_css.c: which sectors are scrambled, and the content
check of a descrambled sector (AC-3 CRCs, MPEG-2 quantiser matrices)."""

import struct

from helpers import ffi, lib

S = 2048


def scrambled_pes(sec):
    assert len(sec) == S
    return lib.ff_dvdvideo_css_scrambled_pes(bytes(sec))


def valid(sec):
    assert len(sec) == S
    return lib.ff_dvdvideo_css_content_valid(bytes(sec)) != 0


def can_test(sec):
    assert len(sec) == S
    return lib.ff_dvdvideo_css_can_test(bytes(sec)) != 0


def pack_header(stuffing, first_stuffing):
    """An MPEG-2 pack header with stuffing stuffing bytes, the first of them
    first_stuffing (the rest 0xFF)."""
    s = bytearray([0, 0, 1, 0xBA, 0x44, 0, 4, 0, 4, 1, 0x01, 0x89, 0xc3, 0xf8 | stuffing])
    if stuffing > 0:
        s.append(first_stuffing)
        s += b"\xff" * (stuffing - 1)
    return s


def pack(ident, fl, stuffing, first_stuffing):
    """A sector with a pack header and one PES packet of stream ident whose
    flag byte (byte 6 of the PES) is fl."""
    s = pack_header(stuffing, first_stuffing)
    s += bytes([0, 0, 1, ident])
    s += struct.pack(">H", S - len(s) - 6)
    s += bytes([fl, 0, 0])
    s += b"\x55" * (S - len(s))
    return s


# ---- which sectors are scrambled ----

def test_usable_packs():
    for ident in [0xBD, 0xE0, 0xC0, 0xC7, 0xBF, 0xBE, 0xE1, 0xC8]:
        assert scrambled_pes(pack(ident, 0x80, 0, 0)) == 0, hex(ident)
    # scrambling bits on a stream that is not checked
    assert scrambled_pes(pack(0xE1, 0xb0, 0, 0)) == 0
    assert scrambled_pes(pack(0xBF, 0xb0, 0, 0)) == 0


def test_scrambled_packs():
    assert scrambled_pes(pack(0xBD, 0x90, 0, 0)) == 14
    assert scrambled_pes(pack(0xE0, 0xa0, 0, 0)) == 14
    assert scrambled_pes(pack(0xC3, 0xb0, 3, 0xff)) == 17


def test_stuffing_counts_only_when_its_first_byte_is_not_zero():
    # stuffing length 2 with a first stuffing byte of 0: the PES must be at 14
    # (there it finds the stuffing bytes, not a start code)
    assert scrambled_pes(pack(0xE0, 0xb0, 2, 0)) == -1
    assert scrambled_pes(pack(0xE0, 0xb0, 2, 0xff)) == 16


def test_sectors_that_are_not_usable_packs():
    assert scrambled_pes(b"\x5a" * S) == -1
    # an MPEG-1 pack header (marker bits 0010)
    m = pack(0xE0, 0xb0, 0, 0)
    m[4] = 0x21
    assert scrambled_pes(m) == -1
    # a pack header without a PES start code after it
    m = pack(0xE0, 0xb0, 0, 0)
    m[16] = 7
    assert scrambled_pes(m) == -1


# ---- the content check: AC-3 ----

def crc16(data):
    """CRC-16 of A/52 (polynomial 0x8005, MSB first, initial 0), bit by bit."""
    crc = 0
    for b in data:
        for i in range(7, -1, -1):
            bit = (b >> i) & 1
            top = (crc >> 15) & 1
            crc = (crc << 1) & 0xffff
            if top != bit:
                crc ^= 0x8005
    return crc


def ac3_sector(fau):
    """A sector with an AC-3 packet: two 768-byte frames (48 kHz, 192 kb/s,
    bsid 8, acmod 2), the first starting at payload byte fau."""
    frame = bytearray(768)
    frame[:2] = b"\x0b\x77"
    frame[4] = 0x14
    frame[5] = 8 << 3
    frame[6] = 0b01000000  # acmod 2 (bits 7-5), dsurmod 0
    frame[7] = 0x40
    for i in range(8, 768):
        frame[i] = i * 7 % 251
    # each CRC region (bytes 2..480 and 480..768) checks to 0 with its CRC at its end
    frame[478:480] = struct.pack(">H", crc16(frame[2:478]))
    frame[766:768] = struct.pack(">H", crc16(frame[480:766]))
    s = pack_header(0, 0)
    payload = bytearray([0x80, 2, 0, fau + 1]) + b"\xaa" * fau
    payload += frame + frame[:8]
    s += bytes([0, 0, 1, 0xBD]) + struct.pack(">H", 3 + len(payload)) + bytes([0x81, 0, 0]) + payload
    s += b"\xff" * (S - len(s))
    return s


def test_ac3_content():
    s = ac3_sector(10)
    assert can_test(s)
    assert valid(s)
    # a changed byte in the scrambled part breaks a CRC
    t = bytearray(s)
    t[600] ^= 0x10
    assert can_test(t), "the header is unchanged"
    assert not valid(t)
    # the next frame's header must match
    t = bytearray(s)
    t[14 + 9 + 4 + 10 + 768] = 0
    assert not valid(t)
    # a frame header past the clear bytes cannot be tested
    late = ac3_sector(100)
    assert not can_test(late)
    assert not valid(late)


def test_ac3_needs_bsid_8_or_6_and_a_known_frame_size():
    s = ac3_sector(10)
    s[14 + 9 + 4 + 10 + 5] = 9 << 3
    assert not can_test(s), "bsid 9"
    s = ac3_sector(10)
    s[14 + 9 + 4 + 10 + 4] = 0xd4
    assert not can_test(s), "fscod 3 (reserved)"


# ---- the content check: MPEG-2 video ----

def video_sector(col):
    """A sector whose video payload starts with a sequence header loading
    both quantiser matrices (the non-intra matrix at offset 99, its column
    values col), a sequence extension and another start code."""
    s = pack_header(0, 0)
    s += bytes([0, 0, 1, 0xE0])
    s += struct.pack(">H", S - len(s) - 6)
    s += bytes([0x81, 0, 0])  # no PES header data: the payload starts at 23
    s += bytes(S - len(s))
    s[23:27] = bytes([0, 0, 1, 0xB3])
    s[27:34] = bytes([0x2d, 0x01, 0xe0, 0x24, 0xff, 0xff, 0xe0])
    s[34] = 0x02  # load_intra_quantiser_matrix
    s[98] = 0x01  # load_non_intra_quantiser_matrix (its last bit)
    v = 99
    for k in range(64):
        s[v + k] = col[k // 8] + k % 8
    for k, c in enumerate(col):
        s[v + 8 * k] = c
    s[v + 0x40:v + 0x45] = bytes([0, 0, 1, 0xB5, 0x14])  # sequence extension
    s[v + 0x45:v + 0x4a] = bytes([0x8a, 0, 1, 0, 0x55])
    s[v + 0x4a:v + 0x4d] = bytes([0, 0, 1])  # the next start code
    return s


RISING = [16, 17, 18, 19, 20, 21, 22, 23]


def test_video_content():
    s = video_sector(RISING)
    assert can_test(s)
    assert valid(s)
    # a matrix column that falls
    c = list(RISING)
    c[4] = 10
    assert can_test(video_sector(c)), "the matrix lies in the scrambled part"
    assert not valid(video_sector(c))
    # the first step may fall a little, not much
    assert valid(video_sector([20, 17, 18, 19, 20, 21, 22, 23]))
    assert not valid(video_sector([40, 17, 18, 19, 20, 21, 22, 23]))
    # no sequence extension
    s = video_sector(RISING)
    s[99 + 0x43] = 0xB8
    assert not valid(s)
    # only one matrix loaded: nothing to test
    s = video_sector(RISING)
    s[98] = 0
    assert not can_test(s)


def test_video_probe_with_a_zero_run_to_the_sector_end():
    # video payload: zeros up to a 01 in the last byte (no sequence header)
    s = pack_header(0, 0)
    s += bytes([0, 0, 1, 0xE0]) + struct.pack(">H", 2028) + bytes([0x80, 0, 0])
    s += bytes(S - len(s))
    s[S - 1] = 1
    assert not can_test(s)
    assert not valid(s)
