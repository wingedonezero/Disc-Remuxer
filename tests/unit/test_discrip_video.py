"""libavformat/discrip_video.c with the MPEG-2 rules of discrip_mpv.c:
pictures timed on a fixed grid (base + display position x field duration).
The PES times fed in are computed here from the bitstream (each GOP's
display start + temporal_reference), independently of the code. Stream made
by FFmpeg's MPEG-2 encoder (tests/data/README.md)."""

from helpers import AV_NOPTS_VALUE, DATA, ffi, lib
from helpers.discrip import Handles, codec_id, events_into, fn, pictures

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
FRAME = 36036000  # 1001/30000 s
BASE = 10 * 1080000000  # the first displayed picture at 10 s
F_KEY, F_DISCARD, F_BATCH, F_MARKER = lib.DR_F_KEY, lib.DR_F_DISCARD, lib.DR_F_BATCH, lib.DR_F_MARKER


class Sink:
    def __init__(self):
        self.out = []  # time, dur, flags
        self.events = []


def run(time, m2v=M2V):
    """The stream through the cutter and the video timing, each picture's
    payload with the PES time time(display index, decode index)."""
    cid = codec_id("mpeg2video")
    sink = Sink()

    def on_frame(f):
        sink.out.append((f.time, f.dur, f.flags))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    c = ffi.new("DRCutter **")
    st = ffi.new("DRVideoStats *")
    assert lib.ff_discrip_video_open(v, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame), lib.tb_py_event,
                                     keep(events_into(sink.events))) == 0
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_video_unit"), v[0]) == 0
    for k, (a, b, disp, _) in enumerate(pictures(m2v)):
        t = time(disp, k)
        assert lib.ff_discrip_cutter_write(c[0], m2v[a:b], b - a, AV_NOPTS_VALUE if t is None else t) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    assert lib.ff_discrip_video_flush(v[0]) == 0
    lib.ff_discrip_video_stats(v[0], st)
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_video_close(v)
    return sink, st


def pes(disp):
    return BASE + disp * FRAME


def of_kind(s, kind):
    return [e for e in s.events if e["kind"] == kind]


def test_pictures_on_the_grid_keep_their_times_in_decode_order():
    pics = pictures(M2V)
    s, st = run(lambda d, k: pes(d))
    assert (st.num, st.den) == (30000, 1001)
    assert st.base == BASE
    assert len(s.out) == len(pics)
    for o, p in zip(s.out, pics):
        assert o[0] == pes(p[2]), "grid time = PES time"
        assert o[1] == FRAME, "two fields"
        assert bool(o[2] & F_KEY) == (p[3] == 1), "I pictures are key frames"
        assert bool(o[2] & F_DISCARD) == (p[3] == 3), "B pictures are discardable"
    assert s.events == [], s.events
    assert any(o[2] & F_BATCH for o in s.out)
    assert s.out[-1][2] & F_BATCH == F_BATCH, "the last picture ends a batch"


def test_only_some_pictures_with_pes_times_still_give_the_grid():
    # PES times on every 5th picture only (as when PES packets span pictures)
    pics = pictures(M2V)
    s, st = run(lambda d, k: pes(d) if k % 5 == 0 else None)
    assert st.base == BASE
    for o, p in zip(s.out, pics):
        assert o[0] == pes(p[2])


def test_a_picture_off_the_grid_is_reported_and_keeps_its_grid_time():
    # like the 300 Combo bonus titles: one picture's PES time 2 frames late
    pics = pictures(M2V)
    bad = 67
    s, st = run(lambda d, k: pes(d) + (2 * FRAME if k == bad else 0))
    tc = of_kind(s, lib.DR_EV_VIDEO_TIMECODE)
    assert len(tc) == 1
    assert (tc[0]["pos"], tc[0]["dur"]) == (pes(pics[bad][2]), 2 * FRAME), "+66.733 ms"
    assert s.out[bad][0] == pes(pics[bad][2]), "the grid time is kept"
    inv = of_kind(s, lib.DR_EV_VIDEO_INVALID)
    assert len(inv) == 1
    assert inv[0]["count"] == 1
    assert st.invalid == 1


def test_a_forward_jump_of_the_pes_times_moves_the_grid_with_placeholders():
    # from the first I picture at decode index 600 or later, the PES times are
    # 60 frames (about 2 s) later
    pics = pictures(M2V)
    jump = 60 * FRAME
    cut = next(k for k in range(600, len(pics)) if pics[k][3] == 1)
    s, st = run(lambda d, k: pes(d) + (jump if k >= cut else 0))
    assert of_kind(s, lib.DR_EV_VIDEO_REPAIR), s.events
    assert st.placeholders > 0
    real = [o for o in s.out if not o[2] & F_MARKER]
    assert len(real) == len(pics)
    # after the jump the pictures are on the new grid again
    last = len(pics) - 1
    assert real[last][0] == pes(pics[last][2]) + jump
    assert real[cut + 30][0] == pes(pics[cut + 30][2]) + jump
    # the I picture at the jump keeps the old grid (+3/16 s) and so do the B
    # pictures of its (open) GOP shown before it: those, and only those, are
    # off the grid
    leading_b = 0
    for p in pics[cut + 1:]:
        if p[3] != 3:
            break
        leading_b += 1
    assert st.invalid == 1 + leading_b
    lo = 11 * 18018000  # 3/16 s of fields at 29.97 fps
    assert [e["dur"] for e in of_kind(s, lib.DR_EV_VIDEO_TIMECODE)] == [jump - lo, jump], \
        "the I picture, then its leading B pictures"


def test_a_jump_of_a_fraction_of_a_field_leaves_the_rest_off_the_grid_by_that_fraction():
    # 2 s = 119.88 fields: the grid moves 120 fields, the pictures after the
    # jump are 2.16 ms early against it (one difference, many pictures)
    pics = pictures(M2V)
    jump = 2 * 1080000000
    cut = next(k for k in range(600, len(pics)) if pics[k][3] == 1)
    s, st = run(lambda d, k: pes(d) + (jump if k >= cut else 0))
    tc = of_kind(s, lib.DR_EV_VIDEO_TIMECODE)
    assert tc
    assert all(e["dur"] == -2160000 or abs(e["dur"]) > FRAME for e in tc), tc
    assert st.invalid > 500


def test_a_later_header_with_another_frame_rate_is_a_warning_and_the_first_rate_stays():
    # every sequence header after the 50th says 25 fps (frame_rate_code 3)
    m = bytearray(M2V)
    heads = [i for i in range(len(m) - 8) if m[i:i + 4] == b"\x00\x00\x01\xb3"]
    for i in heads[50:]:
        m[i + 7] = (m[i + 7] & 0xF0) | 3
    m = bytes(m)
    pics = pictures(m)
    s, st = run(lambda d, k: pes(d), m)
    assert (st.num, st.den) == (30000, 1001)
    rc = of_kind(s, lib.DR_EV_VIDEO_RATE_CHANGE)
    assert len(rc) == 1, "one warning for the new rate"
    assert (rc[0]["count"], rc[0]["dur"]) == (25, 1)
    for o, p in zip(s.out, pics):
        assert o[0] == pes(p[2]), "still timed at 29.97"
