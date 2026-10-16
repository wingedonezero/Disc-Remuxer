"""src/timeline/disctl_frames.c: the title timeline's stage 1 (the frame step). Payloads are cut
into units by the codec's rule; a payload's time goes to the first unit whose
anchor (audio: the frame start; MPEG-2 video: the picture start code) lies
in that payload's bytes (ISO/IEC 13818-1 2.4.3.7). Every expectation is
computed here from the built stream's layout, independently of the code."""

import dataclasses

from synth.a52 import with_crcs
from helpers import AV_NOPTS_VALUE, DATA, ffi, install_log, lib, logged
from helpers.disctl import Handles, codec_id  # noqa: F401 (tb_py_place lives there)

TICKS_PER_PTS = 12000


@dataclasses.dataclass
class Out:
    pos: int
    size: int
    time: int
    head: bytes


def run(codec, payloads, report=None):
    """Runs the frame step for codec over the payloads (bytes, time) or
    (bytes, time, block): (units, stats). report: (label, part, place) with
    place(seg, block) -> str, how left-out bytes are told."""
    cid = codec_id(codec)
    assert cid is not None, "codec name"
    outs = []

    def on_unit(u):
        data = bytes(ffi.buffer(u.data, u.size))
        outs.append(Out(u.pos, u.size, u.time, data[:4]))
        return 0
    keep = Handles()
    c = ffi.new("DRFrames **")
    assert lib.ff_disctl_frames_open(c, ffi.NULL, cid, lib.tb_py_unit, keep(on_unit)) == 0
    if report:
        label, part, place = report
        lib.ff_disctl_frames_set_report(c[0], label.encode(), part.encode(), 0, lib.tb_py_place, keep(place))
    for data, time, *block in payloads:
        assert lib.ff_disctl_frames_write_at(c[0], bytes(data), len(data), AV_NOPTS_VALUE if time is None else time,
                                              block[0] if block else -1) == 0
    assert lib.ff_disctl_frames_flush(c[0]) == 0
    st = ffi.new("DRFramesStats *")
    lib.ff_disctl_frames_stats(c[0], st)
    lib.ff_disctl_frames_close(c)
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
    rest is zero, with valid CRCs (the disc frame rules check them after
    bytes that are no frame)."""
    return with_crcs(bytes([0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]) + bytes(768 - 7))


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


def test_bytes_before_the_first_frame_are_the_end_of_a_cut_frame_later_ones_a_damaged_stream():
    # a title that starts inside a frame (a DVD cell or clip boundary cuts
    # it): its first payload begins with that frame's end; bytes that are no
    # frame further on are damage
    install_log()
    run("ac3", [(b"\x11" * 92 + ac3_frame() * 3 + b"\x22" * 100 + ac3_frame() * 3, 0)])
    assert logged("ac3: the first 92 bytes of the title come before its first frame start: left out; the track "
                  "starts with its first whole frame"), "info"
    assert logged("ac3: 100 bytes at stream offset 2396 do not start a frame (damaged or cut data): left out; the "
                  "track goes on at the next frame start"), "warning"


def place(seg, block):
    return f"in the sector at VTS_01_1.VOB byte {(100 + block) * 2048}, byte {block * 2048} of the title"


def test_left_out_bytes_are_told_with_the_track_and_the_disc_place_of_their_first_byte():
    # payloads of 2016 bytes from sectors 6, 7, 8, ...: the cut frame's end
    # lies in sector 6, the damaged bytes (stream offset 2396) in sector 7
    install_log()
    stream = b"\x11" * 92 + ac3_frame() * 3 + b"\x22" * 100 + ac3_frame() * 3
    payloads = [(p, None, 6 + k) for k, p in enumerate(chunks(stream, 2016))]
    run("ac3", payloads, ("Track 1", "title", place))
    assert logged("Track 1: the first 92 bytes of the title come before its first frame start: left out (in the "
                  "sector at VTS_01_1.VOB byte 217088, byte 12288 of the title); the track starts with its first "
                  "whole frame")
    assert logged("Track 1: 100 bytes at stream offset 2396 do not start a frame (damaged or cut data): left out (in "
                  "the sector at VTS_01_1.VOB byte 219136, byte 14336 of the title); the track goes on at the next "
                  "frame start")


def test_a_frame_the_segment_does_not_finish_is_told_as_the_end_of_the_part():
    install_log()
    outs, st = run("ac3", [(ac3_frame() * 2 + ac3_frame()[:300], None, 0)], ("Track 2", "segment 2", place))
    assert len(outs) == 2 and (st.skipped, st.skipped_bytes) == (1, 300)
    assert logged("Track 2: the segment 2 ends inside a frame: its last 300 bytes are the start of a frame it does "
                  "not finish: left out (in the sector at VTS_01_1.VOB byte 204800, byte 0 of the title); the track "
                  "ends with the frame before")


def test_bytes_with_no_frame_start_are_told_as_damage():
    install_log()
    outs, st = run("ac3", [(b"\x33" * 500, None)], ("Track 1", "title", place))
    assert not outs and (st.skipped, st.skipped_bytes) == (1, 500)
    assert logged("Track 1: 500 bytes at stream offset 0 with no frame start in them (damaged or cut data): left out")


def test_payloads_one_after_another_with_no_frame_start_are_told_in_one_line():
    # a stream this cutter finds no frame in: one line for the whole run
    # (with the places of its first and last payload), not one per payload;
    # a frame between two runs ends the first (DTS: its rule gives up each
    # payload's bytes when no frame starts in them)
    install_log()
    dts = (DATA / "sine_5120.dts").read_bytes()[:1884]
    payloads = [(b"\x33" * 500, None, k) for k in range(4)] + [(dts, None, 4), (b"\x33" * 300, None, 5)]
    outs, st = run("dts", payloads, ("Track 1", "title", place))
    assert len(outs) == 1 and (st.skipped, st.skipped_bytes) == (5, 2300)
    assert logged("Track 1: 2000 bytes from stream offset 0 on, given in 4 pieces, with no frame start in them "
                  "(damaged or cut data, or a stream this cutter does not know): left out (the first in the sector "
                  "at VTS_01_1.VOB byte 204800, byte 0 of the title; the last in the sector at VTS_01_1.VOB byte "
                  "210944, byte 6144 of the title)"), "warning"
    assert logged("Track 1: 300 bytes at stream offset 3884 with no frame start in them (damaged or cut "
                  "data): left out (in the sector at VTS_01_1.VOB byte 215040, byte 10240 of the title)"), "warning"


def test_a_place_that_is_not_known_is_left_out_of_the_line():
    install_log()
    run("ac3", [(b"\x11" * 92 + ac3_frame() * 2, None)], ("Track 1", "title", lambda seg, block: None))
    assert logged("Track 1: the first 92 bytes of the title come before its first frame start: left out; the track "
                  "starts with its first whole frame")


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
    c = ffi.new("DRFrames **")
    ret = lib.ff_disctl_frames_open(c, ffi.NULL, codec_id("av1"), lib.tb_py_unit, keep(lambda u: 0))
    lib.ff_disctl_frames_close(c)
    assert ret < 0
    assert c[0] == ffi.NULL
