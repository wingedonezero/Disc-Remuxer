"""libavformat/disctl_lpcm.c and the LPCM rules of the title timeline: the audio
frame header of DVD-Video (3 bytes) and HD DVD (5 bytes, read in the DVD
layout), the sample conversion to little-endian PCM, and units of one frame
through the frame step and the audio stage. The 16-bit stereo 48 kHz case is the
one the reference program was observed on (a byte swap of each sample); the
other sample sizes follow the DVD-Video sample layout."""

from helpers import ffi, lib
from helpers.disctl import Handles, codec_id, fn

F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def header(p, h, hd):
    return lib.ff_disctl_lpcm_header(p, ffi.NULL, bytes(h), len(h), int(hd))


def convert(p, data):
    out = ffi.new("uint8_t[]", len(data) * 6 // 5 + 16)
    n = lib.ff_disctl_lpcm_convert(p, bytes(data), len(data), out)
    return list(bytes(ffi.buffer(out, n)))


def lpcm():
    return ffi.new("DRLpcm *")


def test_the_dvd_header_and_its_checks():
    # 16 bits, 48 kHz, 2 channels, dynamic range off (as on the corpus disc)
    p = lpcm()
    assert header(p, [0x00, 0x01, 0x80], False) == 0
    assert (p.bits, p.rate, p.channels, p.spf, p.frame_bytes) == (16, 48000, 2, 80, 320)
    # the frame number and the dynamic range may change; nothing else
    assert header(p, [0x05, 0x01, 0x40], False) == 0
    assert p.drc == 0x40
    assert header(p, [0x00, 0x11, 0x80], False) < 0, "another sampling frequency"
    assert header(p, [0x20, 0x01, 0x80], False) < 0, "the reserved bit"
    # 24 bits, 96 kHz, 6 channels
    q = lpcm()
    assert header(q, [0x00, 0x95, 0x80], False) == 0
    assert (q.bits, q.rate, q.channels, q.spf, q.frame_bytes, q.out_frame_bytes) == (24, 96000, 6, 160, 2880, 2880)
    assert header(lpcm(), [0x00, 0xC1, 0x80], False) < 0, "no 28-bit samples"
    assert header(lpcm(), [0x00, 0x31, 0x80], False) < 0, "no fourth frequency"


def test_the_hd_dvd_header_is_read_in_the_dvd_layout():
    # quantisation 20 bits (byte 0 bit 0 = 0, byte 1 bit 7 = 1), frequency
    # code 1 (96 kHz), channel code 1 (2 channels), channel assignment 3
    p = lpcm()
    assert header(p, [0x00, 0x91, 0x80, 0x00, 0x03], True) == 0
    assert (p.bits, p.rate, p.channels, p.spf, p.chmask) == (20, 96000, 2, 80, 0x33)
    assert p.frame_bytes == 20 * 2 * 80 // 8
    assert header(lpcm(), [0x00, 0x48, 0x80, 0, 3], True) < 0, "frequency code 4"
    assert header(lpcm(), [0x00, 0x08, 0x80, 0, 3], True) < 0, "channel code 8"


def test_samples_become_little_endian():
    p = lpcm()
    header(p, [0x00, 0x01, 0x80], False)
    assert convert(p, [0x12, 0x34, 0xAB, 0xCD]) == [0x34, 0x12, 0xCD, 0xAB]
    # 24 bits: a group of 4 samples = their high 16 bits, then their low bytes
    q = lpcm()
    header(q, [0x00, 0x81, 0x80], False)
    g = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0xA1, 0xA2, 0xA3, 0xA4]
    assert convert(q, g) == [0xA1, 0x22, 0x11, 0xA2, 0x44, 0x33, 0xA3, 0x66, 0x55, 0xA4, 0x88, 0x77]
    # 20 bits: the low 4 bits of two samples share a byte (high nibble first)
    r = lpcm()
    header(r, [0x00, 0x41, 0x80], False)
    g = [0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x9A, 0xBC]
    assert convert(r, g) == [0x90, 0x22, 0x11, 0xA0, 0x44, 0x33, 0xB0, 0x66, 0x55, 0xC0, 0x88, 0x77]


def test_units_are_frames_of_the_stated_size():
    # 16-bit stereo 48 kHz: frames of 320 bytes (80 samples, 1/600 s); PES
    # payloads of 2000 bytes each with a header and a time; the bytes after
    # the last whole frame are no unit
    cid = codec_id("pcm_dvd")
    stream = bytes(i % 251 for i in range(4100))
    p = lpcm()
    outs = []

    def on_frame(f):
        outs.append((bytes(ffi.buffer(f.data, f.size)), f.time, f.dur, f.flags))
        lib.ff_disctl_frame_unref(f)
        return 0
    keep = Handles()
    audio = ffi.new("DRAudio **")
    c = ffi.new("DRFrames **")
    assert lib.ff_disctl_audio_open(audio, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame)) == 0
    assert lib.ff_disctl_frames_open(c, ffi.NULL, cid, fn("ff_disctl_audio_unit"), audio[0]) == 0
    lib.ff_disctl_audio_set_state(audio[0], p)
    for k in range(0, len(stream), 2000):
        chunk = stream[k:k + 2000]
        assert header(p, [k // 2000, 0x01, 0x80], False) == 0
        # the frame format for FFmpeg's LPCM parser, as the title gives it
        lib.ff_disctl_frames_set_pcm_format(c[0], p.channels, p.bits, p.spf)
        assert lib.ff_disctl_frames_write(c[0], chunk, len(chunk), (k // 2000) * 1000) == 0
    assert lib.ff_disctl_frames_flush(c[0]) == 0
    assert lib.ff_disctl_audio_flush(audio[0]) == 0
    lib.ff_disctl_frames_close(c)
    lib.ff_disctl_audio_close(audio)
    assert len(outs) == 4100 // 320
    for k, o in enumerate(outs):
        raw = stream[k * 320:(k + 1) * 320]
        swapped = b"".join(bytes([raw[i + 1], raw[i]]) for i in range(0, len(raw), 2))
        assert o[0] == swapped
        assert o[2] == 80 * 1080000000 // 48000
        assert o[3] & (F_KEY | F_SYNC) == F_KEY, "key frames, never sync units"
    # the first frame starting in each payload takes its time
    assert outs[0][1] == 0
    assert outs[7][1] == 1000, "frame 7 (byte 2240) is the first to start in the second payload"


# ---- Blu-ray: a 4-byte header in front of every unit ----

def bd_header(p, h, size=None):
    h = bytes(h)
    return lib.ff_disctl_lpcm_bd_header(p, ffi.NULL, h, len(h) if size is None else size)


def bd(payload, ca, fs, q, low=0):
    """A Blu-ray LPCM header: payload size, channel assignment, frequency code, bits code."""
    return [payload >> 8, payload & 0xFF, ca << 4 | fs, q << 6 | low]


def test_the_blu_ray_header_sets_the_format():
    p = lpcm()
    # the corpus disc's header: 1440 bytes, stereo, 48 kHz, 24 bits
    assert bd_header(p, [0x05, 0xA0, 0x31, 0xC0]) == 0
    assert (p.bd, p.bits, p.rate, p.channels, p.coded, p.payload, p.spf) == (1, 24, 48000, 2, 2, 1440, 240)
    assert (p.chmask, list(p.map)[:2], p.frame_bytes, p.out_frame_bytes) == (0, [0, 1], 1444, 1440)
    q = lpcm()
    assert bd_header(q, bd(960, 9, 4, 1)) == 0, "3/2+LFE, 96 kHz, 16 bits"
    assert (q.bits, q.rate, q.channels, q.coded, q.spf) == (16, 96000, 6, 6, 80)
    r = lpcm()
    assert bd_header(r, bd(1200, 1, 5, 2)) == 0, "mono, 192 kHz, 20 bits"
    assert (r.bits, r.rate, r.channels, r.coded, r.spf) == (20, 192000, 1, 2, 200), "mono is stored as 2 channels"


def test_a_blu_ray_header_that_states_no_format_is_refused():
    for h, why in [(bd(1440, 2, 1, 3), "channel assignment 2"), (bd(1440, 12, 1, 3), "channel assignment 12"),
                   (bd(1440, 3, 2, 3), "frequency code 2"), (bd(1440, 3, 1, 0), "bits code 0"),
                   (bd(0, 3, 1, 3), "no payload")]:
        assert bd_header(lpcm(), h) < 0, why
    assert bd_header(lpcm(), [0x05, 0xA0, 0x31], 3) < 0, "a unit shorter than its header"


def test_every_later_blu_ray_header_must_state_the_same_format():
    p = lpcm()
    assert bd_header(p, bd(1440, 3, 1, 3)) == 0
    assert bd_header(p, bd(1440, 3, 1, 3, low=0x2A)) == 0, "the 6 reserved bits are not compared"
    assert bd_header(p, bd(960, 3, 1, 3)) < 0, "another payload size"
    assert bd_header(p, bd(1440, 3, 1, 2)) < 0, "another sample size"
    assert bd_header(p, bd(1440, 3, 4, 3)) < 0, "another sampling frequency"
    assert bd_header(p, bd(1440, 9, 1, 3)) < 0, "another channel assignment"


FL, FR, FC, LFE, BL, BR, BC, SL, SR = 0x1, 0x2, 0x4, 0x8, 0x10, 0x20, 0x100, 0x200, 0x400


def test_blu_ray_channels_leave_in_the_channel_mask_order():
    # (channel assignment, stored order on the disc, channels shown)
    cases = [
        (4, [FL, FR, FC, 0], 3), (5, [FL, FR, BC, 0], 3), (6, [FL, FR, FC, BC], 4), (7, [FL, FR, SL, SR], 4),
        (8, [FL, FR, FC, SL, SR, 0], 5), (9, [FL, FR, FC, SL, SR, LFE], 6),
        (10, [FL, FR, FC, SL, BL, BR, SR, 0], 7), (11, [FL, FR, FC, SL, BL, BR, SR, LFE], 8),
    ]
    for ca, stored, n in cases:
        p = lpcm()
        assert bd_header(p, bd(len(stored) * 2 * 4, ca, 1, 1)) == 0
        mask = 0
        for c in stored:
            mask |= c
        assert (p.channels, p.coded, p.chmask) == (n, len(stored), mask), ca
        order = sorted(c for c in stored if c)
        assert [stored[k] for k in list(p.map)[:n]] == order, "ascending mask bits, padding left out (%d)" % ca


def test_blu_ray_samples_become_little_endian_in_the_mask_order():
    # 3/2+LFE, 16 bits: stored L R C Ls Rs LFE -> FL FR FC LFE SL SR
    p = lpcm()
    bd_header(p, bd(12, 9, 1, 1))
    frame = [0x10, 0x11, 0x20, 0x21, 0x30, 0x31, 0x40, 0x41, 0x50, 0x51, 0x60, 0x61]
    assert convert(p, frame) == [0x11, 0x10, 0x21, 0x20, 0x31, 0x30, 0x61, 0x60, 0x41, 0x40, 0x51, 0x50]
    # 3/0 (padded to 4), 24 bits: the padding channel is dropped
    q = lpcm()
    bd_header(q, bd(12, 4, 1, 3))
    frame = [0x01, 0x02, 0x03, 0x11, 0x12, 0x13, 0x21, 0x22, 0x23, 0xEE, 0xEE, 0xEE]
    assert convert(q, frame) == [0x03, 0x02, 0x01, 0x13, 0x12, 0x11, 0x23, 0x22, 0x21]
    # mono, 20 bits (left-aligned in 3 bytes): the second stored channel is padding
    r = lpcm()
    bd_header(r, bd(6, 1, 1, 2))
    assert convert(r, [0xAB, 0xCD, 0xE0, 0x77, 0x77, 0x77]) == [0xE0, 0xCD, 0xAB]


def run_bd(stream, writes):
    """Blu-ray LPCM through the frame step (FFmpeg's parser, disc mode) and the
    audio stage; writes: [(start, end, time)] of stream."""
    cid = codec_id("pcm_bluray")
    p = lpcm()
    p.bd = 1
    outs = []

    def on_frame(f):
        outs.append((bytes(ffi.buffer(f.data, f.size)), f.time, f.dur, f.flags))
        lib.ff_disctl_frame_unref(f)
        return 0
    keep = Handles()
    audio = ffi.new("DRAudio **")
    c = ffi.new("DRFrames **")
    assert lib.ff_disctl_audio_open(audio, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame)) == 0
    assert lib.ff_disctl_frames_open(c, ffi.NULL, cid, fn("ff_disctl_audio_unit"), audio[0]) == 0
    lib.ff_disctl_audio_set_state(audio[0], p)
    rets = [lib.ff_disctl_frames_write(c[0], stream[a:b], b - a, t) for a, b, t in writes]
    rets.append(lib.ff_disctl_frames_flush(c[0]))
    rets.append(lib.ff_disctl_audio_flush(audio[0]))
    lib.ff_disctl_frames_close(c)
    lib.ff_disctl_audio_close(audio)
    return p, outs, rets


def test_blu_ray_units_are_header_and_payload_through_the_parser():
    # stereo 16 bits 48 kHz, 8-byte payloads (2 samples); bytes that start no
    # unit (a header that states no format) are given up, then a unit follows
    unit = lambda k: bytes(bd(8, 3, 1, 1)) + bytes([k, 1, k, 2, k, 3, k, 4])
    junk = bytes(bd(8, 2, 1, 1)) + bytes(8)
    stream = unit(1) + unit(2) + junk + unit(3)
    p, outs, rets = run_bd(stream, [(0, 12, 0), (12, 24, 1000), (24, 36, 2000), (36, 48, 3000)])
    assert all(r == 0 for r in rets)
    assert [o[0] for o in outs] == [bytes([1, k, 2, k, 3, k, 4, k]) for k in (1, 2, 3)], "header off, samples swapped"
    assert [o[1] for o in outs] == [0, 1000, 3000]
    assert all(o[2] == 2 * 1080000000 // 48000 for o in outs), "2 samples each"
    assert all(o[3] & (F_KEY | F_SYNC) == F_KEY for o in outs)


def test_a_blu_ray_unit_of_another_format_stops_the_track():
    stream = bytes(bd(8, 3, 1, 1)) + bytes(8) + bytes(bd(4, 3, 1, 1)) + bytes(4)
    p, outs, rets = run_bd(stream, [(0, len(stream), 0)])
    assert len(outs) == 1
    assert any(r < 0 for r in rets), "a payload size that changes is an error"
