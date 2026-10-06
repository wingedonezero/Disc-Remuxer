"""libavformat/discrip_units.c: the rip core's stage 1. Payloads are cut
into units by FFmpeg's parser; a payload's time goes to the first unit whose
anchor (audio: the frame start; MPEG-2 video: the picture start code) lies
in that payload's bytes (ISO/IEC 13818-1 2.4.3.7). Every expectation is
computed here from the built stream's layout, independently of the code."""

import dataclasses

from helpers import AV_NOPTS_VALUE, ffi, lib
from helpers.discrip import Handles, codec_id

TICKS_PER_PTS = 12000


@dataclasses.dataclass
class Out:
    pos: int
    size: int
    time: int
    head: bytes


def run(codec, payloads):
    """Runs the cutter for codec over the payloads (bytes, time):
    (units, stats)."""
    cid = codec_id(codec)
    assert cid is not None, "codec name"
    outs = []

    def on_unit(u):
        data = bytes(ffi.buffer(u.data, u.size))
        outs.append(Out(u.pos, u.size, u.time, data[:4]))
        return 0
    keep = Handles()
    c = ffi.new("DRCutter **")
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, lib.tb_py_unit, keep(on_unit)) == 0
    for data, time in payloads:
        assert lib.ff_discrip_cutter_write(c[0], bytes(data), len(data),
                                           AV_NOPTS_VALUE if time is None else time) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    st = ffi.new("DRCutterStats *")
    lib.ff_discrip_cutter_stats(c[0], st)
    lib.ff_discrip_cutter_close(c)
    return outs, st


def expected_time(payloads, anchors, a):
    """The expected time of an anchor at stream offset a: payload k's time
    goes to the first anchor in [start_k, end_k)."""
    start = 0
    for data, time in payloads:
        end = start + len(data)
        if time is not None:
            first = next((x for x in anchors if start <= x < end), None)
            if first == a:
                return time
        start = end
    return AV_NOPTS_VALUE


def ac3_frame():
    """One AC-3 syncframe: 48 kHz, 192 kbit/s (768 bytes), bsid 8, 2/0; the
    rest is zero (the parser reads only the header)."""
    return bytes([0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]) + bytes(768 - 7)


def chunks(data, n):
    return [data[i:i + n] for i in range(0, len(data), n)]


def test_ac3_times_go_to_the_first_frame_starting_in_each_payload():
    # 92 junk bytes, then 60 frames; DVD-sized payloads (2016 bytes) so that
    # frame starts fall at every position against payload boundaries,
    # including headers split over two payloads. Every 3rd payload has no time.
    lead = 92
    stream = b"\x11" * lead + ac3_frame() * 60
    payloads = [(p, k * 1000 * TICKS_PER_PTS if k % 3 != 2 else None) for k, p in enumerate(chunks(stream, 2016))]
    anchors = [lead + n * 768 for n in range(60)]

    outs, st = run("ac3", payloads)

    assert len(outs) == 60, "every frame is a unit, the junk is not"
    assert st.skipped == 1
    assert st.skipped_bytes == 92
    for o, a in zip(outs, anchors):
        assert o.pos == a
        assert o.size == 768
        assert o.time == expected_time(payloads, anchors, a), f"frame at {a}"
    # payloads with a time but no frame start in them give their time to no unit
    timed_with_start, s = 0, 0
    for d, t in payloads:
        e = s + len(d)
        if t is not None and any(s <= a < e for a in anchors):
            timed_with_start += 1
        s = e
    assert st.timed == timed_with_start
    assert st.records_unused == st.records - st.timed


def test_a_time_is_never_given_to_a_frame_that_started_before_the_payload():
    # Frames larger than a payload: payload 1 holds only the middle of frame 0,
    # so its time belongs to no unit; payload 2 holds the start of frame 1.
    stream = ac3_frame() * 3
    payloads = [(stream[:300], 100), (stream[300:700], 200), (stream[700:], 300)]
    outs, st = run("ac3", payloads)
    assert [o.time for o in outs] == [100, 300, AV_NOPTS_VALUE]
    assert st.records_unused == 1


def mpeg2_picture(seq, tr, kind):
    """MPEG-2 video: sequence header (720x480, 29.97 fps), GOP header,
    picture header (temporal reference tr, type kind), one slice of filler."""
    v = bytearray()
    if seq:
        v += bytes([0, 0, 1, 0xB3, 0x2D, 0x01, 0xE0, 0x24, 0xFF, 0xFF, 0xE0, 0x18])
        v += bytes([0, 0, 1, 0xB8, 0x00, 0x08, 0x00, 0x00])
    v += bytes([0, 0, 1, 0x00, (tr >> 2) & 0xFF, (kind & 7) << 3, 0xFF, 0xF8])
    v += bytes([0, 0, 1, 0x01])
    v += b"\x55" * 600
    return bytes(v)


def test_mpeg2_times_go_to_the_picture_start_code_not_the_sequence_header():
    # 12 pictures, a sequence header + GOP every 4th (pictures of 632 bytes
    # with the headers, 612 without). Payloads of 2470 bytes: the boundary at
    # 2470 falls between picture 4's sequence header (2468) and its picture
    # start code (2488).
    stream = bytearray()
    anchors = []
    for n in range(12):
        pic = mpeg2_picture(n % 4 == 0, n, 1 if n % 4 == 0 else 2)
        anchors.append(len(stream) + pic.find(bytes([0, 0, 1, 0])))
        stream += pic
    stream = bytes(stream)
    payloads = [(p, k * 7 + 1) for k, p in enumerate(chunks(stream, 2470))]

    outs, st = run("mpeg2video", payloads)

    assert len(outs) == 12, outs
    assert st.skipped == 0
    seq_split = 0
    for o, a in zip(outs, anchors):
        assert o.pos <= a < o.pos + o.size, f"unit {o} holds anchor {a}"
        assert o.time == expected_time(payloads, anchors, a), f"picture at {a}"
        if o.head == bytes([0, 0, 1, 0xB3]) and o.pos // 2470 != a // 2470:
            seq_split += 1
    assert seq_split > 0, "layout has a sequence header in an earlier payload than its picture"


def test_a_codec_without_a_table_entry_is_refused():
    keep = Handles()
    c = ffi.new("DRCutter **")
    ret = lib.ff_discrip_cutter_open(c, ffi.NULL, codec_id("hevc"), lib.tb_py_unit, keep(lambda u: 0))
    lib.ff_discrip_cutter_close(c)
    assert ret < 0
    assert c[0] == ffi.NULL
