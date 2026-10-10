"""libavformat/disctl_mlp.c: MLP / Dolby TrueHD access units (AUs) in the
title timeline. An AU lasts the stream's samples per AU; only major-sync AUs are
sync units and key frames; an AU that ends the stream (marker D2 34 at the
end of its substreams) lasts the samples it decodes to; every AU carries the
stream's own number of its first sample (a major sync's restart header, then
counted on). Streams made by FFmpeg's encoders (tests/data/README.md)."""

from helpers import AV_NOPTS_VALUE, DATA, lib
from helpers.disctl import dur, run

THD = (DATA / "sine_4813.thd").read_bytes()
MLP = (DATA / "sine_4813.mlp").read_bytes()
F_KEY, F_SYNC, F_CTR = lib.DR_F_KEY, lib.DR_F_SYNC, lib.DR_F_CTR


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
        assert o.flags & ~F_CTR == sync, "only major-sync AUs are sync units and key frames"


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
    assert r.frames.skipped == 1, "the damaged AU is left out"
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
    assert r.frames.skipped_bytes == 100 + lost
    assert len(r.outs) == 10 + len(units) - next_major
    assert r.outs[10].bytes == units[next_major]


def test_a_cut_off_last_au_is_not_a_unit():
    units = aus(THD)
    r = run("truehd", [(THD[:-5], 5)])
    assert len(r.outs) == len(units) - 1
    assert r.frames.skipped_bytes == len(units[120]) - 5


def output_timing(au):
    """The output timing of a major-sync AU, read as the Dolby TrueHD bitstream
    description lays it out: AU header (4 bytes), major sync (28 bytes, more
    with extended substream info), one directory word per substream (+2 with
    its extra word), then substream 0's block: 2 flag bits, the 13-bit restart
    sync 0x18F5, 1 bit, the 16-bit output_timing."""
    nsub = au[0x14] >> 4
    d = ((au[0x1E] >> 3) & 0x1E) + 0x22 if au[7] == 0xBA and au[0x1D] & 1 else 0x20
    for _ in range(nsub):
        d += 2 + 2 * (au[d] >> 7)
    w = int.from_bytes(au[d:d + 2], "big")
    assert w >> 14 == 3 and (w >> 1) & 0x1FFF == 0x18F5, "a restart header"
    return int.from_bytes(au[d + 2:d + 4], "big")


def test_every_au_carries_its_first_sample_number_counted_from_the_restart_headers():
    for codec, stream in (("truehd", THD), ("mlp", MLP)):
        units = aus(stream)
        r = run(codec, payloads(stream))
        assert len(r.outs) == len(units)
        ctr = None
        for o, au in zip(r.outs, units):
            assert o.flags & F_CTR, codec
            if is_major(au):
                assert o.ctr == output_timing(au), codec
            else:
                assert o.ctr == (ctr + 40) & 0xFFFF, "a minor sync counts on by one AU"
            ctr = o.ctr
        majors = [o.ctr for o, au in zip(r.outs, units) if is_major(au)]
        assert majors[1] - majors[0] == 40 * [k for k, a in enumerate(units) if is_major(a)][1], codec


def test_aus_before_the_first_major_sync_count_back_from_its_restart_header():
    # a segment that starts between two major syncs (an HD DVD title's second
    # EVOB): its first AUs wait for the stream values, then take the sample
    # numbers that lead up to the first major sync's output timing
    for codec, stream in (("truehd", THD), ("mlp", MLP)):
        units = aus(stream)
        k = [i for i, a in enumerate(units) if is_major(a)][1]
        cut = units[1:]                               # starts with minor syncs, k - 1 of them
        r = run(codec, payloads(b"".join(cut)))
        assert len(r.outs) == len(cut), codec
        assert all(o.flags & F_CTR for o in r.outs), "every AU has its sample number"
        full = run(codec, payloads(stream))
        assert [o.ctr for o in r.outs] == [o.ctr for o in full.outs[1:]], "the same numbers as in the whole stream"
        assert r.outs[k - 1].ctr == output_timing(units[k]), codec
