"""libavformat/discrip_lpcm.c and the LPCM rules of the rip core: the audio
frame header of DVD-Video (3 bytes) and HD DVD (5 bytes, read in the DVD
layout), the sample conversion to little-endian PCM, and units of one frame
through the cutter and the audio stage. The 16-bit stereo 48 kHz case is the
one the reference program was observed on (a byte swap of each sample); the
other sample sizes follow the DVD-Video sample layout."""

from helpers import ffi, lib
from helpers.discrip import Handles, codec_id, fn

F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def header(p, h, hd):
    return lib.ff_discrip_lpcm_header(p, ffi.NULL, bytes(h), len(h), int(hd))


def convert(p, data):
    out = ffi.new("uint8_t[]", len(data) * 6 // 5 + 16)
    n = lib.ff_discrip_lpcm_convert(p, bytes(data), len(data), out)
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
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    audio = ffi.new("DRAudio **")
    c = ffi.new("DRCutter **")
    assert lib.ff_discrip_audio_open(audio, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame)) == 0
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_audio_unit"), audio[0]) == 0
    lib.ff_discrip_cutter_set_state(c[0], p)
    lib.ff_discrip_audio_set_state(audio[0], p)
    for k in range(0, len(stream), 2000):
        chunk = stream[k:k + 2000]
        assert header(p, [k // 2000, 0x01, 0x80], False) == 0
        assert lib.ff_discrip_cutter_write(c[0], chunk, len(chunk), (k // 2000) * 1000) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    assert lib.ff_discrip_audio_flush(audio[0]) == 0
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_audio_close(audio)
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
