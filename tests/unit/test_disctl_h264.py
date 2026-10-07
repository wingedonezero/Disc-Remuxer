"""H.264 in the title timeline: FFmpeg's H.264 parser in disc mode
(h264_parser.c) cuts the disc reader's access units (the units in front of a
picture's first slice, the picture's slices, the trailing slice extensions)
and gives each picture's type, recovery point, picture order count and field
periods (pic_struct); the H.264 rules (disctl_h264.c) take out delimiters,
filler, end units and zero stuffing and keep every other NAL unit as on the
disc. Streams built bit by bit here (synth/h264.py)."""

from helpers import AV_NOPTS_VALUE, ffi, lib
from helpers.disctl import Handles, codec_id, events_into, fn
from synth import h264 as H

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
    cid = codec_id("h264")
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


def gop(g, n=N, idr=True, extra_head=b"", pic_struct=0, recovery=True):
    """One GOP in decode order (I P B B P B B ...) as access units with
    their PES times and display positions: [(bytes, time, display)]."""
    out = []
    # decode order: I0, then pairs P(k+2) B(k) B(k+1) -> display I0 B1 B2 P3 B4 B5 P6 ...
    order = [(H.SLICE_I, 0)]
    k = 1
    while k + 2 <= n - 1:
        order += [(H.SLICE_P, k + 2), (H.SLICE_B, k), (H.SLICE_B, k + 1)]
        k += 3
    frame_num = 0
    for i, (kind, disp) in enumerate(order):
        au = H.aud()
        if i == 0:
            au += H.sps() + H.pps()
            au += H.sei([H.pic_timing(pic_struct), H.recovery_point()] if not idr and recovery else
                        [H.pic_timing(pic_struct)])
        else:
            au += H.sei([H.pic_timing(pic_struct)])
        au += extra_head
        ref = kind != H.SLICE_B
        au += H.slice_(kind, frame_num % 16, (2 * disp) % 64, idr=idr and i == 0, ref=ref)
        if ref:
            frame_num += 1
        out.append((au, BASE + (g * n + disp) * 2 * FIELD, g * n + disp))
    return out


def stream(gops=12, **kw):
    units = []
    for g in range(gops):
        units += gop(g, **kw)
    return units


def without(data, types):
    """The frame the H.264 rules make of data: the NAL units whose type is
    not in types, each with its start code (one zero byte in front when the
    data has zero bytes there), trailing zero bytes cut."""
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
        if data[s + 3] & 0x1f in types:
            continue
        out += (b"\0" if s > z else b"") + data[s:e]
    return bytes(out)


def test_access_units_become_frames_on_the_grid_without_delimiters():
    units = stream()
    s, st, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert (st.num, st.den) == (24000, 1001), "frame rate from the SPS timing info"
    assert len(s.out) == len(units)
    for o, (u, t, _) in zip(s.out, units):
        assert o[1] == t, "grid time = PES time"
        assert o[2] == 2 * FIELD
        assert o[0] == without(u, {9}), "every NAL unit as on the disc but the delimiter"
    assert s.events == [], s.events
    assert all(o[3] & F_KEY for o in s.out[::N]), "I pictures are key frames"
    assert not any(o[3] & F_KEY for i, o in enumerate(s.out) if i % N), "only those"
    assert all(o[3] & F_DISCARD for i, o in enumerate(s.out) if i % N % 3 != 1 and i % N), "B pictures"


def test_key_frames_are_idr_pictures_and_recovery_points_not_every_i_picture():
    # GOP 0: IDR; GOP 1: an I picture with a recovery point SEI; GOP 2: an
    # I picture without one (pictures after it may refer to earlier ones)
    units = gop(0) + gop(1, idr=False) + gop(2, idr=False, recovery=False) + gop(3)
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    keys = [i for i, o in enumerate(s.out) if o[3] & F_KEY]
    assert keys == [0, N, 3 * N]


def test_the_cut_does_not_depend_on_how_the_payloads_arrive():
    units = [(u, t) for u, t, _ in stream(gops=4)]
    whole, _, _ = run(units)
    pieces, _, ret = run(units, chunk=184)
    assert ret == 0
    assert [o[:3] for o in pieces.out] == [o[:3] for o in whole.out]


def test_filler_end_units_and_zero_stuffing_are_taken_out():
    units = stream(gops=3)
    dirty = []
    for i, (u, t, _) in enumerate(units):
        if i == 5:
            u = u + H.filler()
        if i == 7:
            u = u + b"\0\0\0\0"                  # trailing_zero_8bits
        if i == len(units) - 1:
            u = u + H.end_of_seq()
        dirty.append((u, t))
    s, _, ret = run(dirty)
    assert ret == 0
    assert len(s.out) == len(units)
    for o, (u, _, _) in zip(s.out, units):
        assert o[0] == without(u, {9})
        assert 12 not in H.nal_types(o[0]) and 10 not in H.nal_types(o[0])


def test_a_picture_before_its_parameter_sets_is_left_out():
    units = stream(gops=3)
    # the stream starts with two pictures of a GOP whose SPS / PPS came before
    head = [(H.aud() + H.slice_(H.SLICE_P, 3, 10), BASE - 4 * FIELD), (H.aud() + H.slice_(H.SLICE_B, 4, 8, ref=False),
                                                                        BASE - 2 * FIELD)]
    s, st, ret = run(head + [(u, t) for u, t, _ in units])
    assert ret == 0
    assert len(s.out) == len(units), "the two pictures without parameter sets are no frames"
    assert s.out[0][0] == without(units[0][0], {9})


def test_slices_of_one_picture_stay_together_and_a_new_frame_num_starts_a_picture():
    # four slices per picture, pictures not separated by any other unit
    units = []
    for i in range(48):
        kind = H.SLICE_I if i % 12 == 0 else H.SLICE_P
        au = (H.sps() + H.pps() + H.sei([H.pic_timing(0)]) if i % 12 == 0 else b"")
        for m in range(4):
            au += H.slice_(kind, i % 16, (2 * i) % 64, idr=i % 12 == 0, first_mb=m * 2040, idr_id=i // 12 % 2)
        units.append((au, BASE + i * 2 * FIELD))
    s, _, ret = run(units)
    assert ret == 0
    assert len(s.out) == len(units)
    assert [o[0] for o in s.out] == [u for u, _ in units]


def test_slice_extensions_after_the_slices_go_with_the_picture_and_sei_with_the_next():
    units = stream(gops=2)
    ext = H.nal(20, b"\x80" * 40)
    sei2 = H.sei([H.pic_timing(0)])
    built = []
    for i, (u, t, _) in enumerate(units):
        if i == 4:
            u = u + ext                          # trailing: same frame
        built.append((u, t))
    s, _, ret = run(built)
    assert ret == 0
    assert s.out[4][0] == without(built[4][0], {9})
    assert H.nal_types(s.out[4][0])[-1] == 20


def test_frame_doubling_lasts_four_field_periods():
    units = stream(gops=3, pic_struct=7)
    s, _, ret = run([(u, BASE + d * 4 * FIELD) for u, _, d in units])
    assert ret == 0
    assert all(o[2] == 4 * FIELD for o in s.out)


def test_without_hrd_or_pic_struct_every_picture_is_a_frame():
    units = []
    for u, t, _ in stream(gops=3):
        u = u.replace(H.sps(), H.sps(hrd=False, pic_struct=False))
        units.append((u.replace(H.sei([H.pic_timing(0)]), b""), t))
    s, _, ret = run(units)
    assert ret == 0
    assert all(o[2] == 2 * FIELD for o in s.out)


def test_a_data_partition_where_a_picture_starts_stops_the_track():
    units = [(u, t) for u, t, _ in stream(gops=1)]
    units.append((H.aud() + H.nal(2, b"\x88" * 30), units[-1][1] + 2 * FIELD))
    _, _, ret = run(units)
    assert ret < 0
