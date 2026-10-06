"""MPEG audio and AAC in the rip core (discrip_codecs.c). MPEG audio frames
last their own samples, are never sync units, and any MPEG audio track is
reviewed (user rule). AAC ADTS frames last 1024 samples per raw block and
are sync units when they carry a channel configuration or a program config
element; frames with the MPEG-4 ID count the same and are reviewed. LATM
takes its values from FFmpeg's decoder on the frame that carries the
StreamMuxConfig and is reviewed. Streams made by FFmpeg's encoders."""

from helpers import DATA, lib
from helpers.discrip import dur, run

MP2 = (DATA / "sine_11520.mp2").read_bytes()
ADTS = (DATA / "sine_11520.aac").read_bytes()
LATM = (DATA / "sine_11520.latm").read_bytes()
F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def adts_frames(s):
    """ADTS frames (13-bit frame length of each header): (start, length)."""
    out, i = [], 0
    while i < len(s):
        n = ((s[i + 3] & 3) << 11) | (s[i + 4] << 3) | (s[i + 5] >> 5)
        out.append((i, n))
        i += n
    return out


def header(r):
    return (r.audio.header.rate, r.audio.header.samples)


def test_mp2_frames_last_1152_samples_are_not_sync_units_and_are_reviewed():
    r = run("mp2", [(MP2, 3)])
    assert len(r.outs) == 10
    assert all(len(o.bytes) == 576 and o.dur == dur(1152, 48000) and o.flags == F_KEY for o in r.outs)
    assert header(r) == (48000, 1152)
    assert r.audio.review > 0


def test_adts_mpeg4_id_frames_are_kept_timed_sync_units_and_reviewed():
    r = run("aac", [(ADTS, 3)])
    assert len(r.outs) == len(adts_frames(ADTS))
    assert all(o.dur == dur(1024, 48000) and o.flags == F_KEY | F_SYNC for o in r.outs)
    assert r.audio.review > 0


def test_adts_mpeg2_id_frames_are_sync_units():
    s = bytearray(ADTS)
    for at, _ in adts_frames(ADTS):
        s[at + 1] |= 0x08  # ID = MPEG-2 (no CRC in these frames)
    r = run("aac", [(bytes(s), 3)])
    assert all(o.dur == dur(1024, 48000) and o.flags == F_KEY | F_SYNC for o in r.outs)
    assert header(r) == (48000, 1024)
    assert r.audio.review == 0


def test_latm_takes_its_values_from_the_frame_with_the_stream_mux_config():
    r = run("aac_latm", [(LATM, 3)])
    assert len(r.outs) == 13
    assert header(r) == (48000, 1024)
    assert all(o.dur == dur(1024, 48000) for o in r.outs)
    assert r.outs[0].flags == F_KEY | F_SYNC
    assert all(o.flags == F_KEY for o in r.outs[1:]), "useSameStreamMux frames are not sync units"
    assert r.audio.review > 0
