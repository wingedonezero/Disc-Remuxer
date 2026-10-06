"""libavformat/discrip_mlp.c: MLP / Dolby TrueHD access units (AUs) in the
rip core. An AU lasts the stream's samples per AU; only major-sync AUs are
sync units and key frames; an AU that ends the stream (marker D2 34 at the
end of its substreams) lasts the samples it decodes to. Streams made by
FFmpeg's encoders (tests/data/README.md)."""

from helpers import AV_NOPTS_VALUE, DATA, lib
from helpers.discrip import dur, run

THD = (DATA / "sine_4813.thd").read_bytes()
MLP = (DATA / "sine_4813.mlp").read_bytes()
F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def aus(stream):
    """The AUs of a stream (length field of each AU header)."""
    out, i = [], 0
    while i < len(stream):
        n = (((stream[i] & 0x0F) << 8) | stream[i + 1]) * 2
        out.append(stream[i:i + n])
        i += n
    return out


def is_major(au):
    return au[4:7] == bytes([0xF8, 0x72, 0x6F])


def payloads(stream):
    """The stream in 2016-byte payloads, each with a time."""
    return [(stream[i:i + 2016], (i // 2016) * 1000 + 1) for i in range(0, len(stream), 2016)]


def check_units(outs, units):
    assert len(outs) == len(units)
    for o, au in zip(outs, units):
        assert o.bytes == au
        sync = F_KEY | F_SYNC if is_major(au) else 0
        assert o.flags == sync, "only major-sync AUs are sync units and key frames"


def test_truehd_aus_last_40_samples_and_the_stream_end_au_its_decoded_13():
    units = aus(THD)
    assert len(units) == 121
    r = run("truehd", payloads(THD))
    check_units(r.outs, units)
    assert (r.audio.header.rate, r.audio.header.samples) == (48000, 40)
    for o in r.outs[:120]:
        assert o.dur == dur(40, 48000)
    assert r.outs[120].dur == dur(13, 48000), "4813 samples = 120 x 40 + 13"
    assert sum(o.dur for o in r.outs) == 120 * dur(40, 48000) + dur(13, 48000)
    assert r.outs[0].time == 1, "the first AU starts the first payload"
    assert r.audio.review == 0


def test_mlp_aus_without_a_stream_end_marker_all_last_40_samples():
    r = run("mlp", payloads(MLP))
    check_units(r.outs, aus(MLP))
    assert all(o.dur == dur(40, 48000) for o in r.outs)


def test_a_major_sync_au_with_a_bad_checksum_is_not_a_unit():
    units = aus(THD)
    second_major = [k for k, a in enumerate(units) if is_major(a)][1]
    offset = sum(len(a) for a in units[:second_major])
    stream = bytearray(THD)
    stream[offset + 12] ^= 0x01  # inside the major sync block, covered by its checksum
    stream = bytes(stream)
    r = run("truehd", [(stream, 5)])
    assert r.cutter.skipped == 1, "the damaged AU is left out"
    assert len(r.outs) == len(units) - 1
    assert all(o.bytes != stream[offset:offset + len(units[second_major])] for o in r.outs)
    assert r.outs[1].time == AV_NOPTS_VALUE


def test_junk_between_aus_is_skipped_up_to_the_next_major_sync():
    # 100 bytes of junk after AU 10: the AUs after it up to the next major
    # sync cannot be found by length any more and are skipped with the junk
    units = aus(THD)
    at = sum(len(a) for a in units[:10])
    stream = THD[:at] + bytes(100) + THD[at:]
    next_major = next(k for k in range(10, len(units)) if is_major(units[k]))
    r = run("truehd", [(stream, 5)])
    lost = sum(len(a) for a in units[10:next_major])
    assert r.cutter.skipped_bytes == 100 + lost
    assert len(r.outs) == 10 + len(units) - next_major
    assert r.outs[10].bytes == units[next_major]


def test_a_cut_off_last_au_is_not_a_unit():
    units = aus(THD)
    r = run("truehd", [(THD[:-5], 5)])
    assert len(r.outs) == len(units) - 1
    assert r.cutter.skipped_bytes == len(units[120]) - 5
