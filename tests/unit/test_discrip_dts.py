"""DTS in the rip core (discrip_codecs.c, ETSI TS 102 114): a core frame
lasts its own npcmblocks x 32 samples; a DTS-HD unit (core + extension
substream, kept together by FFmpeg's parser) is timed by its core; a
core-only track keeps only the core frame; every unit is a sync unit. Core
frames made by FFmpeg's DTS encoder (tests/data/README.md)."""

from helpers import DATA, lib
from helpers.discrip import dur, run

DTS = (DATA / "sine_5120.dts").read_bytes()
FRAME = 1884
F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def exss(size, tag):
    """A DTS-HD extension substream of size bytes (sync 64 58 20 25; header
    size type 0: 8-bit header size, 16-bit frame size), filled with tag."""
    x = bytearray([tag]) * size
    x[:4] = bytes([0x64, 0x58, 0x20, 0x25])
    x[4] = 0  # user defined bits
    # nExtSSIndex (2) = 0, bHeaderSizeType (1) = 0, nuExtSSHeaderSize - 1 (8), nuExtSSFsize - 1 (16), 5 bits 0
    bits = (15 << 21) | ((size - 1) << 5)
    x[5:9] = bits.to_bytes(4, "big")
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
    r = run("dts", [(dtshd_stream(), 1)])
    assert len(r.outs) == 10
    assert all(len(o.bytes) == FRAME + 600 and o.dur == dur(512, 48000) for o in r.outs)
    assert r.audio.cut_bytes == 0


def test_a_core_only_track_keeps_only_the_core_frames():
    r = run("dts", [(dtshd_stream(), 1)], flags=lib.DR_AUDIO_CORE_ONLY)
    assert len(r.outs) == 10
    for o, f in zip(r.outs, frames()):
        assert o.bytes == f
    assert r.audio.cut_bytes == 10 * 600
