"""The stock cutters (src/parse/stock.c): MPEG-1/2 video and AC-3 / E-AC-3
cut exactly as the processing library's own parsers cut them. Both run side
by side over the same bytes in the same pieces (tb_stock_compare in the
bridge): every call must give the same bytes taken, the same unit (bytes and
stream offset) and the same samples and rate. The streams: the MPEG-2 test
stream; MPEG video built from random start-code sequences (frame and field
pictures, sequence ends, junk); AC-3 / E-AC-3 frames built here (AC-3,
independent and dependent E-AC-3, other substreams, byte-swapped frames,
junk, good and bad CRCs) — each cut into many random piece sizes, damaged
copies too."""

import random

from helpers import DATA, ffi, lib

MPEG2VIDEO, AC3, EAC3 = 2, 86019, 86056          # the codec ids (the library's values)
STOCK_MPEGVIDEO, STOCK_AC3 = 0, 1
M2V = (DATA / "testsrc_1200.m2v").read_bytes()


def compare(codec, stock, data, chunks):
    calls = ffi.new("int *")
    buf = ffi.new("uint8_t[]", bytes(data) or b"\0")
    ch = ffi.new("int[]", list(chunks) or [0])
    bad = lib.tb_stock_compare(codec, stock, buf, len(data), ch, len(chunks), calls)
    assert bad == -1, ffi.string(lib.tb_stock_why()).decode()
    return calls[0]


def pieces(rng, size, small=1, large=4096):
    out, left = [], size
    while left > 0:
        n = min(left, rng.randint(small, large))
        out.append(n)
        left -= n
    return out


def damaged(rng, data, n=20):
    d = bytearray(data)
    for _ in range(n):
        at = rng.randrange(len(d))
        kind = rng.randrange(3)
        if kind == 0:
            d[at:at + rng.randint(1, 40)] = bytes(rng.randrange(256) for _ in range(rng.randint(1, 40)))
        elif kind == 1:
            del d[at:at + rng.randint(1, 300)]
        else:
            d[at:at] = bytes(rng.randrange(4) for _ in range(rng.randint(1, 50)))
    return bytes(d)


# ---- MPEG video ----

def mpeg_stream(rng, units=400):
    out = bytearray()
    for _ in range(units):
        k = rng.randrange(14)
        if k == 0:
            out += b"\0\0\1\xb3" + bytes(rng.randrange(256) for _ in range(8))        # sequence header
        elif k == 1:
            out += b"\0\0\1\xb5" + bytes([0x10 | rng.randrange(16)]) + bytes(5)     # sequence extension
        elif k == 2:
            out += b"\0\0\1\xb8" + bytes(rng.randrange(256) for _ in range(4))        # GOP
        elif k in (3, 4):
            out += b"\0\0\1\x00" + bytes(rng.randrange(256) for _ in range(4))        # picture
        elif k in (5, 6):                                                           # picture coding extension
            out += b"\0\0\1\xb5" + bytes([0x80 | rng.randrange(16), rng.randrange(256),
                                          rng.choice([1, 2, 3, rng.randrange(256)]), rng.randrange(256)])
        elif k in (7, 8, 9):
            out += b"\0\0\1" + bytes([rng.randint(1, 0xaf)]) + bytes(rng.randrange(256) for _ in range(rng.randint(0, 60)))
        elif k == 10:
            out += b"\0\0\1\xb2" + bytes(rng.randrange(256) for _ in range(rng.randint(0, 20)))   # user data
        elif k == 11:
            out += b"\0\0\1\xb7"                                                    # sequence end
        elif k == 12:
            out += bytes(rng.choice([0, 0, 1, rng.randrange(256)]) for _ in range(rng.randint(1, 12)))
        else:
            out += b"\0\0\1" + bytes([rng.randrange(256)])
    return bytes(out)


def test_the_mpeg2_stream_is_cut_as_the_library_cuts_it():
    rng = random.Random(1)
    calls = compare(MPEG2VIDEO, STOCK_MPEGVIDEO, M2V, [len(M2V)])
    assert calls > 1200                                   # one unit per picture at least
    for _ in range(20):
        compare(MPEG2VIDEO, STOCK_MPEGVIDEO, M2V, pieces(rng, len(M2V)))
    compare(MPEG2VIDEO, STOCK_MPEGVIDEO, M2V[:20000], [1] * 20000)


def test_damaged_and_built_mpeg_video_is_cut_as_the_library_cuts_it():
    rng = random.Random(2)
    for i in range(60):
        data = damaged(rng, M2V, 30) if i % 2 else mpeg_stream(rng)
        compare(MPEG2VIDEO, STOCK_MPEGVIDEO, data, pieces(rng, len(data), 1, rng.choice([16, 300, 5000])))


# ---- AC-3 / E-AC-3 ----

class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, value, n):
        self.v = (self.v << n) | (value & ((1 << n) - 1))
        self.n += n

    def bytes(self, size):
        pad = size * 8 - self.n
        return (self.v << pad).to_bytes(size, "big")


def crc16(data):
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def with_crc(frame, good):
    body = frame[:-2]
    crc = crc16(body[2:]) if good else 0x1234
    return body + crc.to_bytes(2, "big")


AC3_SIZES = [64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224, 224, 256, 256, 320, 320, 384,
             384, 448, 448, 512, 512, 640, 640, 768, 768, 896, 896, 1024, 1024, 1152, 1152, 1280, 1280]


def ac3_frame(rng, good=True):
    code = rng.randrange(38)
    size = AC3_SIZES[code] * 2                           # 48 kHz
    b = Bits()
    b.put(0x0B77, 16); b.put(0, 16); b.put(0, 2); b.put(code, 6); b.put(8, 5); b.put(0, 3)
    b.put(rng.randrange(8), 3)
    head = b.bytes(7)
    frame = head + bytes(rng.randrange(256) for _ in range(size - 7))
    return with_crc(frame, good)


def eac3_frame(rng, strmtyp=0, substream=0, good=True, words=None):
    words = words or rng.randint(8, 400)
    size = words * 2
    acmod = rng.randrange(8)
    b = Bits()
    b.put(0x0B77, 16); b.put(strmtyp, 2); b.put(substream, 3); b.put(words - 1, 11)
    fscod = rng.choice([0, 1, 2, 3])
    b.put(fscod, 2)
    b.put(rng.randrange(3) if fscod == 3 else rng.randrange(4), 2)
    b.put(acmod, 3); b.put(rng.randrange(2), 1); b.put(16, 5)
    for _ in range(1 if acmod else 2):
        b.put(rng.randrange(32), 5)
        compre = rng.randrange(2)
        b.put(compre, 1)
        if compre:
            b.put(rng.randrange(256), 8)
    if strmtyp == 1:
        chanmape = rng.randrange(2)
        b.put(chanmape, 1)
        if chanmape:
            b.put(rng.randrange(1 << 16), 16)
    nbytes = (b.n + 7) // 8
    if nbytes + 2 > size:
        size = nbytes + 2 + (nbytes % 2)
        return eac3_frame(rng, strmtyp, substream, good, size // 2 + 1)
    head = b.bytes(nbytes)
    frame = head + bytes(rng.randrange(256) for _ in range(size - nbytes))
    return with_crc(frame, good)


def swapped(frame):
    out = bytearray(frame)
    for i in range(0, len(out) - 1, 2):
        out[i], out[i + 1] = out[i + 1], out[i]
    return bytes(out)


def ac3_stream(rng, n=300):
    out = bytearray()
    for _ in range(n):
        k = rng.randrange(12)
        good = rng.randrange(4) > 0
        if k < 3:
            out += eac3_frame(rng, 0, 0, good)
        elif k < 5:
            out += eac3_frame(rng, 1, rng.choice([0, 0, 1]), good)        # dependent frames
        elif k == 5:
            out += eac3_frame(rng, 0, rng.randint(1, 7), good)            # another independent substream
        elif k == 6:
            out += eac3_frame(rng, 2, 0, good)                            # "AC-3 convert" frame type
        elif k in (7, 8):
            out += ac3_frame(rng, good)
        elif k == 9:
            out += swapped(ac3_frame(rng, good))
        elif k == 10:
            out += bytes(rng.choice([0x0b, 0x77, 0, rng.randrange(256)]) for _ in range(rng.randint(1, 30)))
        else:
            out += eac3_frame(rng, 3, 0, good)                            # reserved frame type
    return bytes(out)


def test_ac3_and_e_ac3_are_cut_as_the_library_cuts_them():
    rng = random.Random(3)
    ac3 = (DATA / "sine_7680.ac3").read_bytes()
    for codec in (AC3, EAC3):
        compare(codec, STOCK_AC3, ac3, [len(ac3)])
        compare(codec, STOCK_AC3, ac3, [1] * len(ac3))
    for i in range(80):
        data = ac3_stream(rng)
        if i % 3 == 2:
            data = damaged(rng, data, 15)
        compare(EAC3 if i % 2 else AC3, STOCK_AC3, data, pieces(rng, len(data), 1, rng.choice([7, 200, 3000, 20000])))


def test_e_ac3_keeps_a_frame_and_its_dependent_frames_in_one_unit():
    # the cut itself, as the frame step sees it: an independent frame, two
    # dependent frames, the next independent frame
    rng = random.Random(4)
    frames = [eac3_frame(rng, 0, 0), eac3_frame(rng, 1, 0), eac3_frame(rng, 1, 0), eac3_frame(rng, 0, 0)]
    data = b"".join(frames)
    assert compare(EAC3, STOCK_AC3, data, [len(data)]) >= 2
