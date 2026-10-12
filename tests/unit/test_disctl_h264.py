"""H.264 in the title timeline: FFmpeg's H.264 parser in disc mode
(h264_parser.c) cuts the disc reader's access units (the units in front of a
picture's first slice, the picture's slices, the trailing slice extensions)
and gives each picture's type, recovery point, picture order count and field
periods (pic_struct); the H.264 rules (disctl_h264.c) take out filler, end
units and zero stuffing and keep every other NAL unit (delimiters too) as on
the disc. Streams built bit by bit here (synth/h264.py)."""

from helpers import AV_NOPTS_VALUE, LOG, ffi, install_log, lib
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


def gop(g, n=N, idr=True, extra_head=b"", pic_struct=0, recovery=True, sps=None, field=None):
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
            au += (sps or H.sps()) + H.pps()
            au += H.sei([H.pic_timing(pic_struct), H.recovery_point()] if not idr and recovery else
                        [H.pic_timing(pic_struct)])
        else:
            au += H.sei([H.pic_timing(pic_struct)])
        au += extra_head
        ref = kind != H.SLICE_B
        au += H.slice_(kind, frame_num % 16, (2 * disp) % 64, idr=idr and i == 0, ref=ref, field=field)
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


def test_access_units_become_frames_on_the_grid():
    units = stream()
    s, st, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert (st.num, st.den) == (24000, 1001), "frame rate from the SPS timing info"
    assert len(s.out) == len(units)
    for o, (u, t, _) in zip(s.out, units):
        assert o[1] == t, "grid time = PES time"
        assert o[2] == 2 * FIELD
        assert o[0] == without(u, set()), "every NAL unit as on the disc"
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
    s, st, ret = run(dirty)
    assert ret == 0
    assert len(s.out) == len(units)
    # counted for the title's summary line
    assert list(st.removed) == [1, 0, 1], list(st.removed)
    assert st.stuffing == 4, "the trailing zero bytes"
    for o, (u, _, _) in zip(s.out, units):
        assert o[0] == without(u, set())
        assert 12 not in H.nal_types(o[0]) and 10 not in H.nal_types(o[0])
        assert H.nal_types(o[0])[0] == 9, "the delimiter stays"


def test_a_picture_before_its_parameter_sets_is_left_out():
    units = stream(gops=3)
    # the stream starts with two pictures of a GOP whose SPS / PPS came before
    head = [(H.aud() + H.slice_(H.SLICE_P, 3, 10), BASE - 4 * FIELD), (H.aud() + H.slice_(H.SLICE_B, 4, 8, ref=False),
                                                                        BASE - 2 * FIELD)]
    install_log()
    s, st, ret = run(head + [(u, t) for u, t, _ in units])
    assert ret == 0
    assert len(s.out) == len(units), "the two pictures without parameter sets are no frames"
    assert any("an H.264 picture without its parameter sets (its SPS / PPS not seen): left out with the units in "
               "front of it" in l for l in LOG), LOG
    assert s.out[0][0] == without(units[0][0], set())


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


def repair_units(nslices):
    """Pictures of nslices slices, the slices after the first with other
    frame_num / pic_order_cnt_lsb values, a delimiter in front of each."""
    units = []
    for i in range(48):
        kind = H.SLICE_I if i % 12 == 0 else H.SLICE_P
        au = H.aud() + (H.sps() + H.pps() + H.sei([H.pic_timing(0)]) if i % 12 == 0 else b"")
        for m in range(nslices):
            au += H.slice_(kind, (i + 3 * m) % 16, (2 * i + 6 * m) % 64, idr=i % 12 == 0, first_mb=m * 2040,
                           idr_id=i // 12 % 2)
        units.append((au, BASE + i * 2 * FIELD))
    return units


def test_four_slices_that_agree_on_their_numbers_need_no_repair_and_say_nothing():
    units = []
    for i in range(24):
        kind = H.SLICE_I if i % 12 == 0 else H.SLICE_P
        au = H.aud() + (H.sps() + H.pps() + H.sei([H.pic_timing(0)]) if i % 12 == 0 else b"")
        for m in range(4):
            au += H.slice_(kind, i % 16, (2 * i) % 64, idr=i % 12 == 0, first_mb=m * 2040, idr_id=i // 12 % 2)
        units.append((au, BASE + i * 2 * FIELD))
    install_log()
    s, _, ret = run(units)
    assert ret == 0 and [o[0] for o in s.out] == [u for u, _ in units]
    assert not any("taken as one picture" in line for line in LOG)


def test_four_slices_that_disagree_on_their_numbers_before_a_delimiter_are_one_picture():
    # the reference program's repair: exactly 3 slices after the first, then a delimiter
    units = repair_units(4)
    install_log()
    s, _, ret = run(units)
    assert ret == 0
    assert [o[0] for o in s.out[:47]] == [u for u, _ in units[:47]], "one frame per access unit"
    # the last one has no delimiter after it: its slices go by their numbers
    assert len(s.out) == 47 + 4
    assert sum("taken as one picture" in line and "[untested on real discs]" in line for line in LOG) == 47
    # five such slices: the first is a picture of its own (the 4th unit after
    # it is no delimiter), the 4 after it are the repair's (the rule applies
    # to each picture as it starts); the last access unit has no delimiter
    # after it: 5 pictures
    install_log()
    s, _, ret = run(repair_units(5))
    pictures = [o for o in s.out if o[0]]       # not the grid's placeholders (two pictures share a PES time here)
    assert ret == 0 and len(pictures) == 47 * 2 + 5
    assert sum("taken as one picture" in line for line in LOG) == 47


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
    assert s.out[4][0] == without(built[4][0], set())
    assert H.nal_types(s.out[4][0])[-1] == 20


def test_a_reset_of_the_picture_order_count_keeps_the_display_order():
    # I P P P* P P ... in display order; P* has memory_management_control_operation
    # 5 and the encoder counts from it again (H.264 8.2.1): the pictures after
    # it are shown after it
    units, lsb = [], 0
    for i in range(30):
        head = H.aud() + (H.sps() + H.pps() + H.sei([H.pic_timing(0)]) if i == 0 else H.sei([H.pic_timing(0)]))
        if i == 0:
            sl = H.slice_(H.SLICE_I, 0, 0, idr=True)
        else:
            if i == 13:
                lsb = 0                          # the count after P* starts at its own 0
            lsb += 2
            sl = H.slice_(H.SLICE_P, i % 16, lsb % 64, mmco5=(i == 12))
        units.append((head + sl, BASE + i * 2 * FIELD))
    s, _, ret = run(units)
    assert ret == 0
    assert [o[1] for o in s.out] == [t for _, t in units], "every picture at its own time"
    assert s.events == [], s.events


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


def test_an_interlaced_frame_without_its_pic_struct_takes_it_from_the_pictures_before_and_says_so():
    units = stream(gops=2, pic_struct=3, sps=H.sps(frame_mbs_only=0), field=False)
    k = 15                                       # after more than 9 frames with pic_struct 3
    units[k] = (units[k][0].replace(H.sei([H.pic_timing(3)]), b""),) + units[k][1:]
    install_log()
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert all(o[2] == 2 * FIELD for o in s.out), "pic_struct 3: two fields"
    notes = [l for l in LOG if "without its pic_struct: 3 taken from the pictures before" in l]
    assert len(notes) == 1 and "[untested on real discs]" in notes[0], LOG
    assert any("1 frame(s) without their pic_struct" in l for l in LOG), LOG


def test_pic_struct_that_does_not_fit_the_picture_is_said_once_and_counted():
    units = stream(gops=2, pic_struct=1)         # a field pic_struct on frame pictures
    install_log()
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert len([l for l in LOG if "pic_struct 1 on a frame picture (H.264 Table D-1" in l]) == 1, LOG
    assert any("%d picture(s) whose pic_struct does not fit their field_pic_flag" % len(units) in l for l in LOG), LOG


def test_frame_doubling_without_fixed_frame_rate_is_said_once_and_counted():
    units = stream(gops=2, pic_struct=7, sps=H.sps(fixed_frame_rate=0))
    install_log()
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert len([l for l in LOG if "pic_struct 7 (frame doubling / tripling) without fixed_frame_rate_flag" in l]) == 1
    assert any("%d picture(s) with pic_struct 7 / 8 without fixed_frame_rate_flag" % len(units) in l for l in LOG), LOG
