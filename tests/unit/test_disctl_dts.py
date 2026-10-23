"""DTS in the title timeline (disctl_codecs.c, ETSI TS 102 114): a core frame
lasts its own npcmblocks x 32 samples; a DTS-HD unit (core + extension
substream, kept together by the DTS rule) is timed by its core; a
core-only track keeps only the core frame; every unit is a sync unit. Core
frames made by FFmpeg's DTS encoder (tests/data/README.md)."""

from helpers import DATA, ffi, lib
from helpers.disctl import dur, run

DTS = (DATA / "sine_5120.dts").read_bytes()
FRAME = 1884
F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def crc16_ccitt(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def with_crc(header, start):
    """header with its last 2 bytes the CRC-16 (CCITT, from 0xFFFF) of
    header[start:-2]: the span from start to its end then gives 0."""
    return header[:-2] + crc16_ccitt(header[start:-2]).to_bytes(2, "big")


def exss(size, tag):
    """A DTS-HD extension substream of size bytes (sync 64 58 20 25; header
    size type 0: 8-bit header size, 16-bit frame size; a 16-byte header
    without static fields, its CRC right), filled with tag."""
    x = bytearray([tag]) * size
    x[:4] = bytes([0x64, 0x58, 0x20, 0x25])
    x[4] = 0  # user defined bits
    # nExtSSIndex (2) = 0, bHeaderSizeType (1) = 0, nuExtSSHeaderSize - 1 (8), nuExtSSFsize - 1 (16), 5 bits 0
    bits = (15 << 21) | ((size - 1) << 5)
    x[5:9] = bits.to_bytes(4, "big")
    x[:16] = with_crc(bytes(x[:16]), 5)
    return bytes(x)


def frames():
    return [DTS[i:i + FRAME] for i in range(0, len(DTS), FRAME)]


def test_dts_core_frames_last_512_samples():
    r = run("dts", [(f, 9) for f in frames()])
    assert len(r.outs) == 10
    for o, f in zip(r.outs, frames()):
        assert o.bytes == f
        assert o.dur == dur(512, 48000)
        assert o.flags == F_KEY | F_SYNC
    assert (r.audio.header.rate, r.audio.header.samples) == (48000, 512)
    assert r.audio.review == 0


def dtshd_stream():
    return b"".join(f + exss(600, k) for k, f in enumerate(frames()))


def test_a_dtshd_unit_keeps_core_and_extension_and_is_timed_by_the_core():
    r = run("dts", [(dtshd_stream(), 1)], flags=lib.DR_AUDIO_EXTENDED)
    assert len(r.outs) == 10
    assert all(len(o.bytes) == FRAME + 600 and o.dur == dur(512, 48000) for o in r.outs)
    assert r.audio.cut_bytes == 0


def test_a_dts_track_taken_out_of_dts_hd_keeps_only_the_dts_frames():
    r = run("dts", [(dtshd_stream(), 1)], flags=lib.DR_AUDIO_EXTRACTED | lib.DR_AUDIO_EXTENDED)
    assert len(r.outs) == 10
    for o, f in zip(r.outs, frames()):
        assert o.bytes == f
    assert r.audio.cut_bytes == 10 * 600


# ---- DTS-HD without a core: DTS Express (LBR), lossless only (XLL) ----

class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, value, n):
        self.v = (self.v << n) | (value & ((1 << n) - 1))
        self.n += n

    def pad_to(self, n):
        self.put(0, n - self.n)

    def bytes(self):
        return (self.v << (-self.n % 8)).to_bytes((self.n + 7) // 8, "big")


def coreless(kind, body, rate_index=12, xll_log2=(4, 5), asset_type=None):
    """One extension substream frame with one presentation and one asset
    (ETSI TS 102 114 7.4-7.5): static fields, an asset descriptor in coding
    mode 2 (LBR: body after its sync word 0A 80 19 21) or 1 (lossless only:
    body after an XLL header, sync 41 A2 95 47)."""
    if kind == "lbr":
        comp = bytes.fromhex("0A801921") + body
    elif kind == "xll_frame":
        comp = body                                             # a whole XLL frame (xll_frame())
    else:
        x = Bits()
        x.put(0x41A29547, 32); x.put(0, 4); x.put(15, 8); x.put(0, 5); x.put(0, 1)   # version, header size, 1 bit
        x.put(0, 4); x.put(xll_log2[0], 4); x.put(xll_log2[1], 4)
        x.pad_to(14 * 8)
        comp = with_crc(x.bytes() + b"\0\0", 4) + body         # a 16-byte header, its CRC right
    for header_size in range(16, 64):
        b = Bits()
        b.put(0x64582025, 32); b.put(0, 8)
        b.put(0, 2); b.put(0, 1)                                    # substream 0, short sizes
        b.put(header_size - 1, 8); b.put(header_size + len(comp) - 1, 16)
        b.put(1, 1)                                                 # static fields
        b.put(0, 2); b.put(0, 3); b.put(0, 1)                       # clock, frame duration, no time code
        b.put(0, 3); b.put(0, 3)                                    # one presentation, one asset
        b.put(1, 1); b.put(1, 8)                                    # substream 0 active, asset 0 active
        b.put(0, 1)                                                 # no mixing metadata
        b.put(len(comp) - 1, 16)                                    # the asset's size
        d = Bits()
        d.put(0, 3)                                                 # asset index
        if asset_type is None:
            d.put(0, 1)                                             # no asset type
        else:
            d.put(1, 1); d.put(asset_type, 4)
        d.put(0, 1); d.put(0, 1)                                    # no language, text
        d.put(23, 5); d.put(rate_index, 4); d.put(1, 8)             # 24 bits, max rate, 2 channels
        d.put(0, 1); d.put(0, 3)                                    # no one-to-one map, representation
        d.put(0, 1); d.put(0, 1)                                    # no DRC, no dialog normalization
        if kind == "lbr":
            d.put(2, 2); d.put(len(comp) - 1, 14); d.put(0, 1)
        else:
            d.put(1, 2); d.put(len(comp) - 1, 16); d.put(0, 1); d.put(0, 3)
        size = (9 + d.n + 7) // 8                                   # the descriptor, its size field too
        d0 = b.n
        if d0 + size * 8 > header_size * 8 - 16:
            continue
        b.put(size - 1, 9); b.put(d.v, d.n)
        b.pad_to(d0 + size * 8)
        b.pad_to(header_size * 8)
        return with_crc(b.bytes(), 5) + comp                       # the header's CRC right
    raise AssertionError("header does not fit")


def lbr_chunk(data, good=True):
    """An LBR frame chunk with a checksum (id 0x04, 8-bit length): the id,
    the length's bytes and the data added up, before the data."""
    n = len(data) + 2
    s = (0x04 + (n >> 8) + (n & 0xFF) + sum(data) + (0 if good else 1)) & 0xFFFF
    return bytes([0x04, n]) + s.to_bytes(2, "big") + data


def express(n, rate_code=12, every=4, bad=()):
    """n DTS Express frames: a decoder-init LBR header (the rate code and 10
    more bytes) every `every` frames, sync-only headers between, then the
    frame chunk (its checksum wrong in the frames listed in bad)."""
    out = []
    for k in range(n):
        head = bytes([2, rate_code]) + bytes(10) if k % every == 0 else bytes([1])
        out.append(coreless("lbr", head + lbr_chunk(bytes([k]) * (200 + k % 7), k not in bad)))
    return out


def test_dts_express_frames_are_cut_one_by_one_and_timed_by_their_lbr_header():
    fs = express(12)
    r = run("dts", [(b"".join(fs[:5]), 1), (b"".join(fs[5:]), 2)], flags=lib.DR_AUDIO_EXT_ONLY)
    assert [o.bytes for o in r.outs] == fs
    assert all(o.dur == dur(4096, 48000) for o in r.outs)                 # 1024 << 2 samples, 85.33 ms
    assert all(o.flags == F_KEY | F_SYNC for o in r.outs)
    assert (r.audio.header.rate, r.audio.header.samples) == (48000, 4096)
    assert r.audio.review == 0 and r.frames.skipped == 0


def test_dts_express_takes_the_rate_code_of_the_last_decoder_init_header():
    r = run("dts", [(b"".join(express(8, rate_code=11)), 1)], flags=lib.DR_AUDIO_EXT_ONLY)
    assert len(r.outs) == 8 and all(o.dur == dur(2048, 24000) for o in r.outs)   # 24 kHz: 1024 << 1


def test_dts_express_frames_before_the_first_decoder_init_header_take_its_rate():
    # a title that starts on sync-only frames: the stream's rate is known
    # from its first decoder-init header, and the frames before are timed by it
    fs = express(9)[1:]
    r = run("dts", [(b"".join(fs), 1)], flags=lib.DR_AUDIO_EXT_ONLY)
    assert [o.bytes for o in r.outs] == fs
    assert all(o.dur == dur(4096, 48000) for o in r.outs)


def test_a_lossless_only_stream_is_timed_by_its_xll_header_and_reviewed():
    fs = [coreless("xll", bytes([k]) * 300) for k in range(6)]
    r = run("dts", [(b"".join(fs), 1)], flags=lib.DR_AUDIO_EXT_ONLY)
    assert [o.bytes for o in r.outs] == fs
    assert all(o.dur == dur(512, 48000) for o in r.outs)                 # 1 << (4 + 5)
    assert r.audio.review != 0


def test_core_less_frames_are_not_taken_by_a_dts_or_dts_hd_track():
    # the rule applies only where the disc says the stream has no core
    fs = express(6)
    for flags in (0, lib.DR_AUDIO_EXTENDED, lib.DR_AUDIO_EXTRACTED | lib.DR_AUDIO_EXTENDED):
        r = run("dts", [(b"".join(fs), 1)], flags=flags)
        assert not r.outs


def test_a_track_without_a_core_keeps_no_core_frame():
    # core frames in a stream the disc says has none are not its frames
    fs = express(4)
    r = run("dts", [(b"".join(fs) + frames()[0] + b"".join(express(4)), 1)], flags=lib.DR_AUDIO_EXT_ONLY)
    assert all(o.bytes[:4] == bytes.fromhex("64582025") for o in r.outs)


# ---- the checksums an extension substream carries (verify only) ----

def sums(frame):
    why = ffi.new("const char **")
    ok = lib.dr_dca_exss_checksums(frame, len(frame), why)
    return "ok" if ok else ffi.string(why[0]).decode()


def flip(frame, byte, bit=0):
    b = bytearray(frame)
    b[byte] ^= 1 << bit
    return bytes(b)


def test_the_extension_substream_checksums_pass_and_a_changed_bit_fails():
    lbr = express(1)[0]
    xll = coreless("xll", bytes(300))
    hd = exss(600, 1)
    for f in (lbr, xll, hd):
        assert sums(f) == "ok"
    assert sums(flip(lbr, 8)) == "the DTS-HD extension substream header's CRC"
    assert sums(flip(hd, 9)) == "the DTS-HD extension substream header's CRC"
    assert sums(express(1, bad=(0,))[0]) == "the DTS Express frame's checksum"
    assert sums(flip(lbr, len(lbr) - 1)) == "the DTS Express frame's checksum"
    assert sums(flip(xll, len(xll) - 300 - 4)) == "the DTS-HD lossless (XLL) frame header's CRC"


# ---- the lossless frame walked: its channel set sub-headers' and NAVI table's CRCs, DTS:X by its marker ----

XLL_SYNC = bytes.fromhex("41A29547")


def xll_frame(marker=None, nchsets=1, nsegs_log2=1, rate_index=12, band=bytes(range(96))):
    """A whole XLL frame (ETSI TS 102 114 clause 8): a 16-byte common header (its CRC right), nchsets 16-byte
    channel set sub-headers (2 channels, 24 bits, the sampling frequency of rate_index; each CRC right), the NAVI
    table (16-bit segment sizes, per segment and channel set; its CRC right), the band data (band, cut into the
    segments), then at the next 32-bit boundary the DTS:X marker (or zeros) and 8 more bytes."""
    nsegs = 1 << nsegs_log2
    n = nsegs * nchsets
    sizes = [len(band) // n] * n
    sizes[-1] += len(band) - sum(sizes)
    navi = Bits()
    for size in sizes:
        navi.put(size - 1, 16)
    navi_b = with_crc(navi.bytes() + b"\0\0", 0)
    chs = []
    for _ in range(nchsets):
        c = Bits()
        c.put(16 - 1, 10); c.put(2 - 1, 4); c.put(3, 2); c.put(23, 5); c.put(23, 5); c.put(rate_index, 4)
        c.pad_to(14 * 8)
        chs.append(with_crc(c.bytes() + b"\0\0", 0))
    body = b"".join(chs) + navi_b + band
    pad = -(16 + len(body)) % 4
    tail = bytes(pad) + (marker or 0).to_bytes(4, "big") + bytes(8)
    frame_size = 16 + len(body) + len(tail)
    h = Bits()
    h.put(0x41A29547, 32); h.put(0, 4); h.put(16 - 1, 8)              # version, header size
    h.put(20 - 1, 5); h.put(frame_size - 1, 20)                       # frame size in 20 bits
    h.put(nchsets - 1, 4); h.put(nsegs_log2, 4); h.put(5, 4)          # channel sets, segments, samples (log2)
    h.put(16 - 1, 5); h.put(0, 2); h.put(0, 1); h.put(8 - 1, 5)       # segment size bits, no band CRC, no LSB split
    h.pad_to(14 * 8)
    return with_crc(h.bytes() + b"\0\0", 4) + body + tail


def dtsx(unit):
    marker, imax = ffi.new("uint32_t *"), ffi.new("int *")
    walked = lib.dr_dca_dtsx(unit, len(unit), marker, imax)
    return walked, marker[0], imax[0]


def test_dts_x_is_found_by_its_marker_after_the_band_data():
    # 0x02000850 and the 0xF14000D0-D4 range are DTS:X; a frame without one is plain DTS-HD MA
    for marker in (0x02000850, 0xF14000D0, 0xF14000D3, 0xF14000D4, None):
        unit = coreless("xll_frame", xll_frame(marker))
        assert sums(unit) == "ok"
        assert dtsx(unit) == (1, marker or 0, 0), hex(marker or 0)
        # the same with a core frame before the extension substream
        assert dtsx(frames()[0] + unit) == (1, marker or 0, 0)
    assert dtsx(coreless("xll_frame", xll_frame(0xF14000D5)))[1] == 0, "not a DTS:X marker"


def test_two_channel_sets_and_several_segments_are_walked():
    unit = coreless("xll_frame", xll_frame(0xF14000D3, nchsets=2, nsegs_log2=2))
    assert sums(unit) == "ok" and dtsx(unit) == (1, 0xF14000D3, 0)


def test_an_imax_asset_type_is_said():
    assert dtsx(coreless("xll_frame", xll_frame(0x02000850), asset_type=13)) == (1, 0x02000850, 1)


def test_a_channel_set_above_96_khz_is_not_walked_but_its_sub_header_is_checked():
    # above 96 kHz a channel set has 2 frequency bands, or more (a flag deep in its sub-header): not counted
    unit = coreless("xll_frame", xll_frame(0x02000850, rate_index=14))
    assert dtsx(unit)[0] == 0 and sums(unit) == "ok"
    x = unit.index(XLL_SYNC)
    assert sums(flip(unit, x + 16 + 3)) == "a DTS-HD lossless (XLL) channel set sub-header's CRC"


def test_a_changed_bit_in_a_sub_header_or_the_navi_table_fails_and_one_in_the_band_data_does_not():
    unit = coreless("xll_frame", xll_frame(0xF14000D3, nchsets=2))
    x = unit.index(XLL_SYNC)
    for byte in range(16, 48):                                        # both channel set sub-headers
        assert sums(flip(unit, x + byte, byte % 8)) == "a DTS-HD lossless (XLL) channel set sub-header's CRC"
    navi = x + 48
    for byte in range(navi, navi + 6):                                # 4 segment sizes of 16 bits... and the CRC
        assert sums(flip(unit, byte, byte % 8)) == "the DTS-HD lossless (XLL) NAVI table's CRC"
    assert sums(flip(unit, navi + 12)) == "ok", "the band data has no CRC here"
    assert dtsx(flip(unit, x + 20))[0] == 0, "a frame whose check fails is not walked"
