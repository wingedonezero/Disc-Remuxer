"""HEVC in the title timeline: FFmpeg's HEVC parser in disc mode
(hevc/parser.c) cuts the disc reader's access units (the units in front of a
picture's first slice segment, the picture's slice segments, the trailing
suffix SEI / type 62 / 63 units) and gives each picture's type, random
access point, picture order count and field periods (pic_struct); the HEVC
rules (disctl_hevc.c) take out end, filler and reserved units, units of
another layer and zero stuffing and keep every other NAL unit (delimiters
too) as on the disc. Streams built bit by bit here (synth/hevc.py)."""

from helpers import AV_NOPTS_VALUE, LOG, ffi, install_log, lib
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


def gop(g, n=N, head_type=H.IDR_N_LP, pic_struct=0, recovery=False, sps=None, timing_sei=True, slice_size=300):
    """One GOP in decode order (I P B B P B B ...) as access units with
    their PES times and display positions: [(bytes, time, display)].
    head_type: the I picture's nal_unit_type; an IDR picture counts its
    GOP's display order from 0, others go on from the GOPs before.
    slice_size: the bytes of picture data (other bytes: other pictures)."""
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
        au += H.slice_(t, kind, poc % 256, size=slice_size)
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


def test_filler_reserved_and_other_layer_units_and_zero_stuffing_are_taken_out():
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
        dirty.append((u, t))
    s, _, ret = run(dirty)
    assert ret == 0
    assert len(s.out) == len(units)
    for o, (u, _, _) in zip(s.out, units):
        assert o[0] == without(u)
        assert not {H.FD, 45} & set(H.nal_types(o[0]))
        assert H.nal_types(o[0])[0] == H.AUD, "the delimiter stays"


def test_unspecified_units_stay_and_an_end_of_sequence_unit_marks_its_frame():
    units = stream(gops=2)
    built = []
    for i, (u, t, _) in enumerate(units):
        if i == 3:
            u = u + H.nal(57, b"\x21" * 6)             # unspecified (application data)
        if i == N - 1:
            u = u + H.end_of_seq()
        built.append((u, t))
    s, _, ret = run(built)
    assert ret == 0
    assert s.out[3][0] == built[3][0], "kept as on the disc"
    end = s.out[N - 1]
    assert end[0] == built[N - 1][0], "the end unit stays for the title timeline to decide"
    assert end[3] & lib.DR_F_SEQ_END
    assert not any(o[3] & lib.DR_F_SEQ_END for i, o in enumerate(s.out) if i != N - 1)


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


def test_a_slice_segment_of_another_pps_stays_in_its_picture():
    # damaged data: the second slice segment of picture 4 names PPS 1; only
    # first_slice_segment_in_pic_flag starts a picture (H.265 7.4.7.1), so the
    # picture keeps all its slice segments (a decoder skips the bad one)
    units = []
    for i in range(24):
        head = i % 12 == 0
        au = H.aud()
        if head:
            au += H.vps() + H.sps() + H.pps() + H.pps(pps_id=1)
        for m in range(3):
            au += H.slice_(H.IDR_N_LP if head else H.TRAIL_R, H.SLICE_I if head else H.SLICE_P, i % 12,
                           first=m == 0, address=m * 500, pps_id=1 if (i == 4 and m == 1) else 0)
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


def test_a_stream_of_field_pictures_is_cut_as_the_reference_program_cuts_it():
    # Ultra HD Blu-ray video is progressive, but a field stream is ripped:
    # each field picture lasts one field period, and that is said
    units = stream(gops=2, sps=H.sps(field_seq=True, timing=(1001, 48000)), pic_struct=1)
    install_log()
    s, _, ret = run([(u, BASE + d * FIELD) for u, _, d in units])
    assert ret == 0
    # the video stage pairs field pictures into frames, as for H.264 and MPEG-2
    assert b"".join(o[0] for o in s.out) == b"".join(u for u, _, _ in units), "every picture, in order"
    assert sum(o[2] for o in s.out) == len(units) * FIELD, "one field period each"
    assert any("coded as fields" in line and "[untested on real discs]" in line for line in LOG)


def test_a_field_picture_without_its_timing_sei_takes_the_pic_struct_of_the_pictures_before():
    head = stream(gops=1, sps=H.sps(field_seq=True, timing=(1001, 48000)), pic_struct=2)
    tail = [(u, t, d + N) for u, t, d in gop(1, sps=H.sps(field_seq=True, timing=(1001, 48000)), timing_sei=False)]
    install_log()
    s, _, ret = run([(u, BASE + d * FIELD) for u, _, d in head + tail])
    assert ret == 0
    assert sum(o[2] for o in s.out) == len(head + tail) * FIELD, "pic_struct 2 guessed: one field each"
    assert any("taken from the pictures before" in line for line in LOG)
    # nothing before to guess from: the stream cannot be cut (the reference program fails too)
    _, _, ret = run([(u, t) for u, t, _ in stream(gops=1, sps=H.sps(field_seq=True, timing=(1001, 48000)), timing_sei=False)])
    assert ret < 0


def test_a_field_paired_with_another_picture_stops_the_track():
    # pic_struct 9-12 (the reference program refuses them too)
    units = stream(gops=1, pic_struct=9)
    _, _, ret = run([(u, t) for u, t, _ in units])
    assert ret < 0


def test_without_vui_pic_struct_is_read_when_the_profile_says_progressive_and_interlaced():
    # H.265 E.3.1: frame_field_info_present_flag is then inferred as 1 (the
    # timing comes from the VPS)
    def units(interlaced, step):
        return [(u.replace(H.vps(), H.vps(timing=(1001, 24000))), BASE + d * step * FIELD)
                for u, _, d in stream(gops=2, pic_struct=7, sps=H.sps(vui=False, interlaced=interlaced))]
    s, st, ret = run(units(True, 4))
    assert ret == 0
    assert (st.num, st.den) == (24000, 1001)
    assert all(o[2] == 4 * FIELD for o in s.out)
    s, _, ret = run(units(False, 2))
    assert ret == 0
    assert all(o[2] == 2 * FIELD for o in s.out), "otherwise inferred 0: frames"


def test_a_missing_picture_timing_sei_is_taken_as_a_frame():
    units = stream(gops=2, timing_sei=False)
    s, _, ret = run([(u, t) for u, t, _ in units])
    assert ret == 0
    assert all(o[2] == 2 * FIELD for o in s.out)


# ---- the title timeline: end of sequence units between segments ----

def run_title(segments):
    """Video-only title: each segment's units (bytes, PES time) through the
    title timeline; the video frames' bytes in output order."""
    tr = ffi.new("DRTitleTrack[]", [[codec_id("hevc"), lib.DR_KIND_VIDEO, 0]])
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": 1, "tracks": tr, "lpcm_hd": 0})
    t = ffi.new("DRTitle **")
    out = []

    def drain():
        while True:
            f = ffi.new("DRFrame *")
            k = ffi.new("int *")
            r = lib.ff_disctl_title_frame(t[0], k, f)
            if r:
                return r
            out.append(bytes(ffi.buffer(f.data, f.size)) if f.size else b"")
            lib.ff_disctl_frame_unref(f)
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    for seg in segments:
        assert lib.ff_disctl_title_segment(t[0]) == 0
        for u, time in seg:
            assert lib.ff_disctl_title_payload(t[0], 0, u, len(u), time) == 0
            drain()
    assert lib.ff_disctl_title_finish(t[0]) == 0
    drain()
    w = ffi.new("int64_t *")
    assert lib.ff_disctl_title_track_result(t[0], 0, ffi.new("int64_t *"), w, ffi.new("int64_t *")) == 0
    lib.ff_disctl_title_close(t)
    return out, w[0]


def ending(units):
    """The segment's units with an end of sequence unit after the last picture."""
    return units[:-1] + [(units[-1][0] + H.end_of_seq(), units[-1][1])]


def test_an_end_of_sequence_unit_before_an_idr_picture_and_at_the_end_is_taken_out():
    s1 = ending([(u, t) for u, t, _ in stream(gops=2)])
    s2 = ending([(u, t) for u, t, _ in stream(gops=2, slice_size=301)])   # other pictures than s1's
    out, warnings = run_title([s1, s2])
    assert len(out) == 4 * N
    assert all(H.EOS not in H.nal_types(o) for o in out), "nothing depends on them"
    assert out[2 * N - 1] == without(s1[-1][0], lambda t, l: t == H.EOS)
    assert warnings == 0


def test_an_end_of_sequence_unit_before_a_cra_picture_is_kept():
    s1 = ending([(u, t) for u, t, _ in stream(gops=2)])
    s2 = [(u, t) for u, t, _ in gop(0, head_type=H.CRA, slice_size=301) + gop(1, slice_size=301)]
    out, warnings = run_title([s1, s2])
    assert len(out) == 4 * N
    assert out[2 * N - 1] == s1[-1][0], "the CRA picture's leading pictures depend on it"
    assert warnings == 1


def test_an_end_of_sequence_unit_before_a_picture_that_is_no_random_access_point_is_kept():
    s1 = [(u, t) for u, t, _ in stream(gops=2)]
    s1[5] = (s1[5][0] + H.end_of_seq(), s1[5][1])     # a stream fault: a P picture follows
    out, warnings = run_title([s1])
    assert out[5] == s1[5][0]
    assert warnings == 1


def test_a_delimiter_muxed_into_the_packet_before_keeps_the_frame_at_its_picture_time():
    # a badly muxed stream: picture 5's delimiter and SEI ride at the end of
    # picture 4's packet; picture 5 keeps the time of the packet with its
    # picture (the anchor rule), and the frames' bytes are unchanged
    units = stream(gops=2)
    head = H.aud() + H.sei([H.pic_timing(0)])
    payloads = [(u, t) for u, t, _ in units]
    assert payloads[5][0].startswith(head)
    payloads[4] = (payloads[4][0] + head, payloads[4][1])
    payloads[5] = (payloads[5][0][len(head):], payloads[5][1])
    s, _, ret = run(payloads)
    assert ret == 0
    assert [o[1] for o in s.out] == [t for _, t, _ in units]
    assert [o[0] for o in s.out] == [without(u) for u, _, _ in units]


# ---- Dolby Vision: the enhancement layer merged into the video's frames ----

def rpu():
    """A stand-in for a Dolby Vision RPU unit (nal_unit_type 62): its bytes
    pass as they are."""
    return H.nal(62, b"\x19\x08\x09\x55" * 6 + b"\x80", sc=b"\0\0\1")


def el_units(units):
    """The enhancement layer of a stream: its own access units (here the
    same pictures) each closed by its RPU."""
    return [(u + rpu(), t) for u, t, _ in units]


def wrapped(el):
    """The units of an enhancement picture as they follow the base picture:
    7E 01 in front of each unit's header, the RPU as it is."""
    out = bytearray()
    starts, i = [], el.find(b"\0\0\1")
    while i >= 0:
        starts.append(i)
        i = el.find(b"\0\0\1", i + 3)
    pos = 0
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else len(el)
        out += el[pos:s + 3]
        if (el[s + 3] >> 1) & 63 not in (62, 63):
            out += b"\x7e\x01"
        out += el[s + 3:e]
        pos = e
    return bytes(out)


def test_wrapping_puts_7e01_in_front_of_every_unit_but_the_rpu():
    el = H.aud() + H.vps() + H.slice_(H.IDR_N_LP, H.SLICE_I) + rpu()
    out = ffi.new("uint8_t[]", lib.ff_disctl_hevc_wrap_size(el, len(el)))
    n = lib.ff_disctl_hevc_wrap_layer(el, len(el), out)
    got = bytes(ffi.buffer(out, n))
    assert got == wrapped(el)
    assert got.count(b"\0\0\1\x7e\x01") == 3, "delimiter, VPS, slice wrapped"
    assert got.endswith(rpu()), "the RPU as it is"


def run_dv(base, el, merged=True):
    """A title of an HEVC video and its Dolby Vision enhancement layer (one
    segment): the frames per track (bytes, time) and the video's warnings."""
    tr = ffi.new("DRTitleTrack[]", [[codec_id("hevc"), lib.DR_KIND_VIDEO, 0, 0, 0, 0, 0],
                                    [codec_id("hevc"), lib.DR_KIND_LAYER, 0, 0, 0, 0,
                                     lib.DR_LAYER_MERGED if merged else 0]])
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": 2, "tracks": tr, "lpcm_hd": 0})
    t = ffi.new("DRTitle **")
    out = {0: [], 1: []}

    def drain():
        while True:
            f = ffi.new("DRFrame *")
            k = ffi.new("int *")
            if lib.ff_disctl_title_frame(t[0], k, f):
                return
            out[k[0]].append((bytes(ffi.buffer(f.data, f.size)) if f.size else b"", f.time))
            lib.ff_disctl_frame_unref(f)
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    assert lib.ff_disctl_title_segment(t[0]) == 0
    for (b, tb), (e, te) in zip(base, el + [(None, None)] * (len(base) - len(el))):
        assert lib.ff_disctl_title_payload(t[0], 0, b, len(b), tb) == 0
        if e is not None:
            assert lib.ff_disctl_title_payload(t[0], 1, e, len(e), te) == 0
        drain()
    for e, te in el[len(base):]:
        assert lib.ff_disctl_title_payload(t[0], 1, e, len(e), te) == 0
    assert lib.ff_disctl_title_finish(t[0]) == 0
    drain()
    w = ffi.new("int64_t *")
    assert lib.ff_disctl_title_track_result(t[0], 0, ffi.new("int64_t *"), w, ffi.new("int64_t *")) == 0
    lib.ff_disctl_title_close(t)
    return out, w[0]


def test_each_video_frame_carries_its_enhancement_picture():
    units = stream(gops=2)
    base = [(u, t) for u, t, _ in units]
    el = el_units(units)
    out, warnings = run_dv(base, el)
    assert out[1] == [], "no track of its own"
    assert len(out[0]) == len(units)
    for (b, t), (e, _), o in zip(base, el, out[0]):
        assert o == (without(b) + wrapped(without(e)), t - BASE), "title times start at the first picture's"
    assert warnings == 0


def test_separate_the_enhancement_layer_is_a_track_with_the_video_times():
    units = stream(gops=2)
    base = [(u, t) for u, t, _ in units]
    el = el_units(units)
    out, warnings = run_dv(base, el, merged=False)
    assert [o for o in out[0]] == [(without(b), t - BASE) for b, t in base]
    assert [o for o in out[1]] == [(without(e), t - BASE) for e, t in el]
    assert warnings == 0


def test_a_video_frame_without_its_enhancement_picture_goes_on_alone_with_a_warning():
    units = stream(gops=2)
    base = [(u, t) for u, t, _ in units]
    el = el_units(units)
    del el[2 * N - 3]                                 # a damaged disc: one enhancement picture missing
    out, warnings = run_dv(base, el)
    assert len(out[0]) == len(units)
    assert out[0][2 * N - 3] == (without(base[2 * N - 3][0]), base[2 * N - 3][1] - BASE), "the base picture alone"
    assert warnings == 1
