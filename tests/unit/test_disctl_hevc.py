"""HEVC in the title timeline: FFmpeg's HEVC parser in disc mode
(hevc/parser.c) cuts the disc reader's access units (the units in front of a
picture's first slice segment, the picture's slice segments, the trailing
suffix SEI / type 62 / 63 units) and gives each picture's type, random
access point, picture order count and field periods (pic_struct); the HEVC
rules (disctl_hevc.c) take out end, filler and reserved units, units of
another layer and zero stuffing and keep every other NAL unit (delimiters
too) as on the disc. Streams built bit by bit here (synth/hevc.py)."""

from helpers import AV_NOPTS_VALUE, ffi, lib
from helpers.disctl import Handles, codec_id, events_into, fn
from synth import hevc as H

FIELD = 22522500  # 1001/48000 s
BASE = 5 * 1080000000
F_KEY, F_DISCARD = lib.DR_F_KEY, lib.DR_F_DISCARD


class Sink:
    def __init__(self):
        self.out = []
        self.events = []


def run(units, chunk=0):
    """Units (bytes, PES time) through the frame step and the video stage:
    the frames (bytes, time, duration, flags), the stats, the frame step's
    return code. chunk: write each unit in pieces of that many bytes."""
    cid = codec_id("hevc")
    sink = Sink()

    def on_frame(f):
        sink.out.append((bytes(ffi.buffer(f.data, f.size)), f.time, f.dur, f.flags))
        lib.ff_disctl_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    c = ffi.new("DRFrames **")
    st = ffi.new("DRVideoStats *")
    assert lib.ff_disctl_video_open(v, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame), lib.tb_py_event,
                                     keep(events_into(sink.events))) == 0
    assert lib.ff_disctl_frames_open(c, ffi.NULL, cid, fn("ff_disctl_video_unit"), v[0]) == 0
    ret = 0
    for data, t in units:
        pieces = [data[i:i + chunk] for i in range(0, len(data), chunk)] if chunk else [data]
        for k, p in enumerate(pieces):
            ret = lib.ff_disctl_frames_write(c[0], p, len(p), t if k == 0 else AV_NOPTS_VALUE)
            if ret < 0:
                break
        if ret < 0:
            break
    if ret >= 0:
        ret = lib.ff_disctl_frames_flush(c[0])
        assert lib.ff_disctl_video_flush(v[0]) == 0
    lib.ff_disctl_video_stats(v[0], st)
    lib.ff_disctl_frames_close(c)
    lib.ff_disctl_video_close(v)
    return sink, st, ret


N = 10  # pictures of a GOP: I0 P3 B1 B2 P6 B4 B5 P9 B7 B8


def gop(g, n=N, head_type=H.IDR_N_LP, pic_struct=0, recovery=False, sps=None, timing_sei=True):
    """One GOP in decode order (I P B B P B B ...) as access units with
    their PES times and display positions: [(bytes, time, display)].
    head_type: the I picture's nal_unit_type; an IDR picture counts its
    GOP's display order from 0, others go on from the GOPs before."""
    out = []
    order = [(H.SLICE_I, 0)]
    k = 1
    while k + 2 <= n - 1:
        order += [(H.SLICE_P, k + 2), (H.SLICE_B, k), (H.SLICE_B, k + 1)]
        k += 3
    idr = head_type in (H.IDR_W_RADL, H.IDR_N_LP)
    for i, (kind, disp) in enumerate(order):
        au = H.aud()
        msgs = [H.pic_timing(pic_struct)] if timing_sei else []
        if i == 0:
            au += H.vps() + (sps or H.sps()) + H.pps()
            if recovery:
                msgs.append(H.recovery_point())
        if msgs:
            au += H.sei(msgs)
        if i == 0:
            t = head_type
        else:
            t = H.TRAIL_R if kind != H.SLICE_B else H.TRAIL_N
        poc = disp if idr else g * n + disp
        au += H.slice_(t, kind, poc % 256)
        out.append((au, BASE + (g * n + disp) * 2 * FIELD, g * n + disp))
    return out


def stream(gops=12, **kw):
    units = []
    for g in range(gops):
        units += gop(g, **kw)
    return units


def without(data, drop=lambda t, layer: False):
    """The frame the HEVC rules make of data: the NAL units drop() keeps,
    each with its start code (one zero byte in front when the data has zero
    bytes there), trailing zero bytes cut."""
    starts, i = [], data.find(b"\0\0\1")
    while i >= 0:
        lo = starts[-1][1] + 4 if starts else 0
        z = i
        while z > lo and data[z - 1] == 0:
            z -= 1
        starts.append((z, i))
        i = data.find(b"\0\0\1", i + 3)
    out = bytearray()
    for k, (z, s) in enumerate(starts):
        e = starts[k + 1][0] if k + 1 < len(starts) else len(data)
        while e > s + 3 and data[e - 1] == 0:
            e -= 1
        if drop((data[s + 3] >> 1) & 0x3f, (data[s + 3] & 1) << 5 | data[s + 4] >> 3):
            continue
        out += (b"\0" if s > z else b"") + data[s:e]
    return bytes(out)


def test_access_units_become_frames_on_the_grid():
    units = stream()
    s, st, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert (st.num, st.den) == (24000, 1001), "frame rate from the VUI timing info"
    assert len(s.out) == len(units)
    for o, (u, t, _) in zip(s.out, units):
        assert o[1] == t, "grid time = PES time"
        assert o[2] == 2 * FIELD
        assert o[0] == without(u), "every NAL unit as on the disc"
    assert s.events == [], s.events
    assert all(o[3] & F_KEY for o in s.out[::N]), "IDR pictures are key frames"
    assert not any(o[3] & F_KEY for i, o in enumerate(s.out) if i % N), "only those"
    assert all(o[3] & F_DISCARD for i, o in enumerate(s.out) if i % N % 3 != 1 and i % N), "B pictures"


def test_key_frames_are_irap_pictures_and_recovery_points_not_every_i_picture():
    # GOP 0: IDR; 1: CRA; 2: an I picture (TRAIL_R) with a recovery point
    # SEI; 3: an I picture without one (pictures after it may refer to
    # earlier ones); 4: IDR
    units = (gop(0) + gop(1, head_type=H.CRA) + gop(2, head_type=H.TRAIL_R, recovery=True) +
             gop(3, head_type=H.TRAIL_R) + gop(4))
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    keys = [i for i, o in enumerate(s.out) if o[3] & F_KEY]
    assert keys == [0, N, 2 * N, 4 * N]
    assert [o[1] for o in s.out] == [t for _, t, _ in units], "display order across the GOPs"


def test_the_cut_does_not_depend_on_how_the_payloads_arrive():
    units = [(u, t) for u, t, _ in stream(gops=4)]
    whole, _, _ = run(units)
    pieces, _, ret = run(units, chunk=184)
    assert ret == 0
    assert [o[:3] for o in pieces.out] == [o[:3] for o in whole.out]


def test_end_filler_reserved_and_other_layer_units_and_zero_stuffing_are_taken_out():
    units = stream(gops=3)
    dirty = []
    for i, (u, t, _) in enumerate(units):
        if i == 5:
            u = u + H.filler()
        if i == 6:
            u = u + H.nal(45, b"\x40" * 8)             # a reserved type
        if i == 7:
            u = u + b"\0\0\0\0"                         # trailing_zero_8bits
        if i == 8:
            u = u + H.slice_(H.TRAIL_R, H.SLICE_P, 0, layer=1)  # a second layer's unit
        if i == len(units) - 1:
            u = u + H.end_of_seq()
        dirty.append((u, t))
    s, _, ret = run(dirty)
    assert ret == 0
    assert len(s.out) == len(units)
    for o, (u, _, _) in zip(s.out, units):
        assert o[0] == without(u)
        assert not {H.FD, H.EOS, 45} & set(H.nal_types(o[0]))
        assert H.nal_types(o[0])[0] == H.AUD, "the delimiter stays"


def test_a_picture_before_its_parameter_sets_is_left_out():
    units = stream(gops=3)
    # the stream starts with two pictures of a GOP whose VPS / SPS / PPS came before
    head = [(H.aud() + H.slice_(H.TRAIL_R, H.SLICE_P, 3), BASE - 4 * FIELD),
            (H.aud() + H.slice_(H.TRAIL_N, H.SLICE_B, 4), BASE - 2 * FIELD)]
    s, st, ret = run(head + [(u, t) for u, t, _ in units])
    assert ret == 0
    assert len(s.out) == len(units), "the two pictures without parameter sets are no frames"
    assert s.out[0][0] == without(units[0][0])


def test_slice_segments_of_one_picture_stay_together_and_a_first_slice_segment_starts_a_picture():
    # four slice segments per picture, pictures not separated by any other unit
    units = []
    for i in range(48):
        head = i % 12 == 0
        au = H.vps() + H.sps() + H.pps() + H.sei([H.pic_timing(0)]) if head else b""
        for m in range(4):
            au += H.slice_(H.IDR_N_LP if head else H.TRAIL_R, H.SLICE_I if head else H.SLICE_P, i % 12,
                           first=m == 0, address=m * 500)
        units.append((au, BASE + i * 2 * FIELD))
    s, _, ret = run(units)
    assert ret == 0
    assert len(s.out) == len(units)
    assert [o[0] for o in s.out] == [u for u, _ in units]


def test_suffix_units_after_the_slices_go_with_the_picture_and_prefix_sei_with_the_next():
    units = stream(gops=2)
    suffix = H.sei([(4, b"\x01\x02\x03")], suffix=True) + H.nal(63, b"\x11" * 20) + H.nal(62, b"\x19" * 30)
    built = []
    for i, (u, t, _) in enumerate(units):
        if i == 4:
            u = u + suffix                               # trailing: same frame
        built.append((u, t))
    s, _, ret = run(built)
    assert ret == 0
    assert len(s.out) == len(units)
    assert s.out[4][0] == without(built[4][0])
    assert H.nal_types(s.out[4][0])[-3:] == [H.SEI_SUFFIX, 63, 62]
    assert H.nal_types(s.out[5][0])[:2] == [H.AUD, H.SEI_PREFIX]


def test_frame_doubling_lasts_four_field_periods():
    units = stream(gops=3, pic_struct=7)
    s, _, ret = run([(u, BASE + d * 4 * FIELD) for u, _, d in units])
    assert ret == 0
    assert all(o[2] == 4 * FIELD for o in s.out)


def test_without_frame_field_info_every_picture_is_a_frame():
    units = stream(gops=3, pic_struct=7, sps=H.sps(frame_field_info=False))
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert all(o[2] == 2 * FIELD for o in s.out), "the picture timing SEI is not read without it"


def test_the_vps_timing_gives_the_rate_when_the_vui_has_none():
    units = []
    for u, t, _ in stream(gops=2):
        u = u.replace(H.vps(), H.vps(timing=(1001, 30000))).replace(H.sps(), H.sps(timing=None))
        units.append((u, t))
    s, st, ret = run(units)
    assert ret == 0
    assert (st.num, st.den) == (30000, 1001)


def test_field_pictures_without_their_picture_timing_stop_the_track():
    units = stream(gops=1, sps=H.sps(field_seq=True), timing_sei=False)
    _, _, ret = run([(u, t) for u, t, _ in units])
    assert ret < 0
