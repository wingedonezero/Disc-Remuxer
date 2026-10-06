"""libavformat/discrip_audio.c with the AC-3 / E-AC-3 rules of
discrip_codecs.c: units of one segment get their duration (samples / rate
of their own first syncframe, ATSC A/52), the sync-unit flag and their final
bytes (an AC-3 track keeps only AC-3 syncframes, an E-AC-3 track only E-AC-3
ones; dependent frames and further substreams stay in the unit of their
independent frame and are reported for review)."""

from helpers import AV_NOPTS_VALUE, lib
from helpers.discrip import dur, run, timed

F_KEY, F_SYNC, F_MARKER = lib.DR_F_KEY, lib.DR_F_SYNC, lib.DR_F_MARKER


def ac3(tag):
    """AC-3 syncframe: 48 kHz, 192 kbit/s (768 bytes), bsid 8, 2/0; tag
    marks the payload so frames can be told apart."""
    return bytes([0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]) + bytes([tag]) * (768 - 7)


def eac3(strmtyp, sub, numblkscod, size, tag):
    """E-AC-3 syncframe (bsid 16, 48 kHz, 2/0): stream type (0 independent,
    1 dependent), substream id, numblkscod (0..3 = 1, 2, 3, 6 blocks), size."""
    f = bytearray([tag]) * size
    w = (strmtyp << 14) | (sub << 11) | (size // 2 - 1)
    f[0] = 0x0B
    f[1] = 0x77
    f[2:4] = w.to_bytes(2, "big")
    f[4] = (numblkscod << 4) | (2 << 1)
    f[5] = 16 << 3
    f[6] = 0
    f[7] = 0
    return bytes(f)


def header(r):
    return (r.audio.header.rate, r.audio.header.samples)


def test_ac3_units_take_1536_samples_and_are_sync_units():
    frames = [ac3(k) for k in range(10)]
    r = run("ac3", timed(frames))
    assert len(r.outs) == 10
    for k, o in enumerate(r.outs):
        assert o.bytes == frames[k]
        assert o.time == k * 1000
        assert o.dur == 34560000, "1536 samples at 48 kHz"
        assert o.flags == F_KEY | F_SYNC
    assert header(r) == (48000, 1536)
    assert r.audio.review == 0


def test_eac3_one_block_frames_take_256_samples():
    frames = [eac3(0, 0, 0, 1024, k) for k in range(12)]
    r = run("eac3", timed(frames))
    assert len(r.outs) == 12
    assert all(o.dur == dur(256, 48000) and o.flags == F_KEY | F_SYNC for o in r.outs)
    assert header(r) == (48000, 256)
    assert r.audio.review == 0


def test_eac3_dependent_frames_join_their_independent_frame_and_are_reviewed():
    # independent 5.1-style frame + dependent frame (extra channels), 8 times;
    # each pair in its own payload, the payload time at the independent frame
    data = [eac3(0, 0, 3, 768, k) + eac3(1, 0, 3, 512, 0x80 | k) for k in range(8)]
    r = run("eac3", timed(data))
    assert len(r.outs) == 8, "one unit per independent frame"
    for k, o in enumerate(r.outs):
        assert o.bytes == data[k], "independent + dependent bytes kept together"
        assert o.dur == dur(1536, 48000), "one duration for the pair"
        assert o.time == k * 1000
    assert r.audio.review > 0


def test_eac3_second_program_joins_substream_0_and_is_reviewed():
    data = [eac3(0, 0, 3, 768, k) + eac3(0, 1, 3, 512, 0x80 | k) for k in range(6)]
    r = run("eac3", timed(data))
    assert len(r.outs) == 6
    assert all(len(o.bytes) == 768 + 512 and o.dur == dur(1536, 48000) for o in r.outs)
    assert r.audio.review > 0


def test_ac3_track_keeps_only_its_ac3_syncframes():
    # AC-3 core + E-AC-3 dependent frame (core cut), and AC-3 + E-AC-3
    # independent frame (the E-AC-3 frame is not a unit of an AC-3 track)
    for strmtyp in [1, 0]:
        stream = b"".join(ac3(k) + eac3(strmtyp, 0, 3, 512, 0x80 | k) for k in range(6))
        r = run("ac3", [(stream, 0)])
        assert len(r.outs) == 6, f"stream type {strmtyp}"
        for k, o in enumerate(r.outs):
            assert o.bytes == ac3(k), f"stream type {strmtyp}"
            assert o.dur == 34560000
        assert r.audio.cut_bytes + r.cutter.skipped_bytes == 6 * 512, f"stream type {strmtyp}"
        assert r.audio.review == 0


def test_eac3_track_leaves_out_ac3_syncframes():
    stream = b"".join(ac3(k) + eac3(0, 0, 3, 512, 0x80 | k) for k in range(6))
    r = run("eac3", [(stream, 0)])
    assert len(r.outs) == 6
    assert all(len(o.bytes) == 512 and o.bytes[2] & 0xC0 == 0 for o in r.outs)
    assert r.cutter.skipped == 6
    # the payload's time goes to the first UNIT starting in it: the AC-3 frame
    # before it is not a unit of this track
    assert r.outs[0].time == 0
    assert all(o.time == AV_NOPTS_VALUE for o in r.outs[1:])


def test_a_frame_with_another_length_is_timed_by_its_own_header_and_reviewed():
    frames = [eac3(0, 0, 3, 768, k) for k in range(4)] + [eac3(0, 0, 2, 512, k) for k in range(4, 8)]
    r = run("eac3", timed(frames))
    durs = [o.dur for o in r.outs]
    assert durs[:4] == [dur(1536, 48000)] * 4
    assert durs[4:] == [dur(768, 48000)] * 4
    assert r.audio.header.samples == 1536, "the stream's values stay those of its first frame"
    assert r.audio.review == 4


def test_a_marker_is_an_empty_frame_that_keeps_its_time():
    r = run("ac3", timed([ac3(k) for k in range(2)]), marker=777)
    m = r.outs[-1]
    assert (len(m.bytes), m.time, m.dur, m.flags) == (0, 777, 0, F_KEY | F_MARKER)
    assert r.audio.markers == 1
