"""Blu-ray PGS in the title timeline: the PGS rule (src/parse/pgs_rule.c)
(hdmv_pgs_parser.c) cuts display sets (a presentation composition segment to
the end segment) and leaves out what is none; the subtitle stage
(disctl_spu.c, disctl_pgs.c) times each set at the video grid point nearest
to its composition segment's time, lasting one field period; a forced-only
track cuts each set's composition objects to the forced ones (what a player
shows with subtitles off) and keeps the sets that then show forced objects,
the first set after them that shows none (it clears them), and the sets of
an epoch a kept set inside that epoch may use. The video grid comes from the
MPEG-2 test stream (tests/data/README.md); the sets are built byte by byte."""

import struct

from helpers import AV_NOPTS_VALUE, DATA, ffi, lib
from helpers.disctl import Handles, codec_id, events_into, fn, pictures

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
FRAME = 36036000  # 1001/30000 s
FIELD = FRAME // 2
BASE = 10 * 1080000000


# ---- display sets ----

def seg(t, body):
    return bytes([t]) + struct.pack(">H", len(body)) + body


def obj(oid, forced=False, cropped=False):
    """A composition object: object id, window id, flags, x, y (+ cropping)."""
    o = struct.pack(">HBBHH", oid, 0, (0x40 if forced else 0) | (0x80 if cropped else 0), 100, 900)
    return o + (struct.pack(">HHHH", 0, 0, 50, 20) if cropped else b"")


def pcs(objects, number=1, state=0x80):
    body = struct.pack(">HHBHBBBB", 1920, 1080, 0x10, number, state, 0, 0, len(objects))
    return seg(0x16, body + b"".join(objects))


def display_set(objects, number=1, state=0x80):
    """PCS, window, palette, one object definition per object, end."""
    out = pcs(objects, number, state)
    out += seg(0x17, bytes([1, 0, 0, 0, 100, 3, 132, 0, 50, 0, 20]))
    out += seg(0x14, bytes([0, 0]) + bytes([1, 16, 128, 128, 255]))
    for k in range(len(objects)):
        out += seg(0x15, struct.pack(">HBB", k, 0, 0xC0) + b"\x00\x00\x10" + struct.pack(">HH", 50, 20) + b"\x01" * 12)
    return out + seg(0x80, b"")


def clear_set(number=2, state=0x80):
    return pcs([], number, state) + seg(0x17, bytes([0])) + seg(0x80, b"")


def update_set(objects, number):
    """A set inside its epoch (composition state normal) that shows objects
    an earlier set defined: PCS and end only."""
    return pcs(objects, number, 0x00) + seg(0x80, b"")


def run(payloads, forced_only=False, showing=0):
    """The video (every picture on its grid from BASE) and PGS payloads
    (bytes, time): the sets (bytes, time, duration), the stats, the cut."""
    vid = codec_id("mpeg2video")
    pid = codec_id("hdmv_pgs_subtitle")
    out, events = [], []

    def on_frame(f):
        out.append((bytes(ffi.buffer(f.data, f.size)), f.time, f.dur))
        lib.ff_disctl_frame_unref(f)
        return 0

    def drop(f):
        lib.ff_disctl_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    vc = ffi.new("DRFrames **")
    s = ffi.new("DRSpu **")
    sc = ffi.new("DRFrames **")
    st = ffi.new("DRSpuStats *")
    cut = ffi.new("DRFramesStats *")
    assert lib.ff_disctl_video_open(v, ffi.NULL, vid, 0, lib.tb_py_frame, keep(drop), ffi.NULL, ffi.NULL) == 0
    assert lib.ff_disctl_frames_open(vc, ffi.NULL, vid, fn("ff_disctl_video_unit"), v[0]) == 0
    assert lib.ff_disctl_spu_open(s, ffi.NULL, 1, v[0], 1920, 1080, lib.tb_py_frame, keep(on_frame), lib.tb_py_event,
                                   keep(events_into(events))) == 0
    on = ffi.new("int *", showing)
    lib.ff_disctl_spu_set_pgs(s[0], int(forced_only), on)
    assert lib.ff_disctl_frames_open(sc, ffi.NULL, pid, fn("ff_disctl_spu_unit"), s[0]) == 0
    for a, b, disp, _ in pictures(M2V):
        assert lib.ff_disctl_frames_write(vc[0], M2V[a:b], b - a, BASE + disp * FRAME) == 0
    for data, time in payloads:
        assert lib.ff_disctl_frames_write(sc[0], data, len(data), AV_NOPTS_VALUE if time is None else time) == 0
    assert lib.ff_disctl_frames_flush(vc[0]) == 0
    assert lib.ff_disctl_video_flush(v[0]) == 0
    assert lib.ff_disctl_frames_flush(sc[0]) == 0
    assert lib.ff_disctl_spu_flush(s[0]) == 0
    lib.ff_disctl_spu_stats(s[0], st)
    lib.ff_disctl_frames_stats(sc[0], cut)
    lib.ff_disctl_frames_close(sc)
    lib.ff_disctl_spu_close(s)
    lib.ff_disctl_frames_close(vc)
    lib.ff_disctl_video_close(v)
    return out, st, cut


def segments(data):
    """The segments of the payload as (type, its bytes), each one PES packet
    on a disc."""
    out, i = [], 0
    while i < len(data):
        n = 3 + struct.unpack(">H", data[i + 1:i + 3])[0]
        out.append(data[i:i + n])
        i += n
    return out


def as_pes(sets_and_times):
    """Each segment as its own payload; the composition segment carries the
    set's time, the others an earlier one (as on a disc)."""
    out = []
    for data, t in sets_and_times:
        for k, sg in enumerate(segments(data)):
            out.append((sg, t if k == 0 else t - 6 * FIELD))
    return out


# ---- tests ----

def test_a_display_set_is_one_unit_at_its_composition_time_lasting_one_field():
    s1, s2 = display_set([obj(0)]), clear_set()
    out, st, _ = run(as_pes([(s1, BASE + 10 * FRAME), (s2, BASE + 40 * FRAME + 1000)]))
    assert [o[0] for o in out] == [s1, s2], "the bytes as on the disc"
    assert [o[1] for o in out] == [BASE + 10 * FRAME, BASE + 40 * FRAME], "snapped to the video grid"
    assert all(o[2] == FIELD for o in out)
    assert st.forced_objects == 0


def test_what_is_no_display_set_is_left_out():
    good = display_set([obj(0)])
    stray = seg(0x15, b"\x00" * 20)                      # an object segment without a composition
    cut_short = pcs([obj(0)]) + seg(0x17, bytes([0]))   # no end segment before the next set
    tail = pcs([obj(1)], 9)                             # the stream ends inside a set
    out, _, cut = run([(stray, BASE + 2 * FRAME), (cut_short, BASE + 4 * FRAME), (good, BASE + 8 * FRAME),
                       (tail, BASE + 20 * FRAME)])
    assert [o[0] for o in out] == [good]
    assert cut.skipped == 3


def test_a_forced_only_track_shows_only_the_forced_objects():
    mixed = display_set([obj(0, forced=True), obj(1)])
    normal = display_set([obj(2)], 2)                   # replaces the forced subtitle: nothing is shown
    cropped = display_set([obj(3, cropped=True), obj(4, forced=True)], 3)
    out, st, _ = run(as_pes([(mixed, BASE + 5 * FRAME), (normal, BASE + 30 * FRAME), (cropped, BASE + 60 * FRAME)]),
                     forced_only=True)
    assert len(out) == 3, "forced, the set that clears it, forced"
    assert segments(out[0][0])[0] == pcs([obj(0, forced=True)])
    assert segments(out[1][0])[0] == pcs([], 2), "no forced object: the set clears the screen"
    assert segments(out[2][0])[0] == pcs([obj(4, forced=True)], 3), "the object after a cropped one is read right"
    for o, s in zip(out, (mixed, normal, cropped)):
        assert segments(o[0])[1:] == segments(s)[1:], "the other segments as on the disc"
    assert st.forced_objects == 2
    full, st_full, _ = run(as_pes([(mixed, BASE + 5 * FRAME)]))
    assert full[0][0] == mixed and st_full.forced_objects == 1, "the full track keeps the set as it is"


def test_a_forced_only_track_keeps_the_forced_sets_and_the_sets_that_clear_them():
    # the way discs carry forced subtitles: each one a set of its own (an
    # epoch start with its own window, palette and object), cleared by the
    # next set; the sets without forced objects are left out
    sets = [display_set([obj(0)], 1), clear_set(2, 0x00),
            display_set([obj(0, forced=True)], 3), clear_set(4, 0x00),
            display_set([obj(0)], 5), clear_set(6, 0x00), display_set([obj(0)], 7), clear_set(8, 0x00),
            display_set([obj(0, forced=True)], 9), display_set([obj(0, forced=True)], 10), clear_set(11, 0x00),
            clear_set(12, 0x00)]
    times = [BASE + (5 + 10 * k) * FRAME for k in range(len(sets))]
    out, st, _ = run(as_pes(list(zip(sets, times))), forced_only=True)
    assert [o[0] for o in out] == [sets[k] for k in (2, 3, 8, 9, 10)], "the forced sets and their clears, as on the disc"
    assert [o[1] for o in out] == [times[k] for k in (2, 3, 8, 9, 10)]
    assert (st.forced_sets, st.forced_clears, st.forced_reused, st.forced_left_out) == (3, 2, 0, 7)
    assert st.forced_objects == 3


def test_a_forced_set_inside_its_epoch_keeps_the_sets_it_may_use():
    # the epoch's first set defines object 0 (not forced there); a later set
    # of the epoch shows it forced without defining it again: the first set
    # stays too, showing nothing
    start = display_set([obj(0)], 1)
    later = update_set([obj(0, forced=True)], 2)
    out, st, _ = run(as_pes([(start, BASE + 5 * FRAME), (later, BASE + 20 * FRAME), (clear_set(3, 0x00),
                                                                                   BASE + 40 * FRAME)]),
                     forced_only=True)
    assert len(out) == 3
    assert segments(out[0][0])[0] == pcs([], 1) and segments(out[0][0])[1:] == segments(start)[1:]
    assert out[1][0] == later and out[0][1] < out[1][1]
    assert (st.forced_sets, st.forced_clears, st.forced_reused, st.forced_left_out) == (1, 1, 1, 0)
    # a new epoch: what the one before defined is not needed
    out, st, _ = run(as_pes([(start, BASE + 5 * FRAME), (display_set([obj(0, forced=True)], 2), BASE + 20 * FRAME)]),
                     forced_only=True)
    assert len(out) == 1 and (st.forced_reused, st.forced_left_out) == (0, 1)


def test_a_forced_subtitle_on_screen_at_a_segment_start_is_cleared_there():
    # the track's state goes across segments: a segment that starts while a
    # forced subtitle is shown keeps its first set without one
    sets = [(clear_set(1, 0x00), BASE + 5 * FRAME), (display_set([obj(0)], 2), BASE + 20 * FRAME)]
    out, st, _ = run(as_pes(sets), forced_only=True, showing=1)
    assert [o[0] for o in out] == [sets[0][0]] and st.forced_clears == 1
    out, _, _ = run(as_pes(sets), forced_only=True, showing=0)
    assert out == []


def test_the_set_check():
    assert lib.ff_disctl_pgs_check(display_set([obj(0)]), len(display_set([obj(0)]))) == 1
    bad = display_set([obj(0)])[:-3]
    assert lib.ff_disctl_pgs_check(bad, len(bad)) == 0
