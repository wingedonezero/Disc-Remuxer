"""libavformat/discrip_join.c: the rip core's joiner. Segment 0 starts the
title timeline at its first video time (the earliest of the video timing's
first batch); segment k is placed where the video of segment k-1 really
ended. Audio without a time takes where its track is; audio early by more
than its duration is moved there; subtitles and video keep their times;
chapter marks go on the video key frame at or up to 0.4 s before them."""

from helpers import AV_NOPTS_VALUE, ffi, lib
from helpers.discrip import Handles, events_into

FRAME = 36036000  # 1001/30000 s
AC3 = 34560000
F_KEY, F_BATCH, F_CHAPTER = lib.DR_F_KEY, lib.DR_F_BATCH, lib.DR_F_CHAPTER
VIDEO, AUDIO, SUBTITLE = lib.DR_KIND_VIDEO, lib.DR_KIND_AUDIO, lib.DR_KIND_SUBTITLE


class Sink:
    def __init__(self):
        self.out = []  # track, time, flags
        self.events = []


def fr(time, dur, flags):
    return [time, dur, flags]


def join(kinds, marks, segments):
    """Segments of (track, frame) in input order: (sink, stats), or the
    negative error code."""
    sink = Sink()

    def on_out(track, f):
        sink.out.append((track, f.time, f.flags))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    kinds_c = ffi.new("int[]", kinds)
    marks_c = ffi.new("int64_t[]", marks or [0])
    cfg = ffi.new("DRJoinConfig *", {
        "nb_tracks": len(kinds), "kinds": kinds_c, "marks": marks_c, "nb_marks": len(marks),
        "out": lib.tb_py_join_out, "out_opaque": keep(on_out),
        "event": lib.tb_py_event, "event_opaque": keep(events_into(sink.events))})
    j = ffi.new("DRJoin **")
    st = ffi.new("DRJoinStats *")
    assert lib.ff_discrip_join_open(j, ffi.NULL, cfg) == 0
    ret = 0
    held = []
    for seg in segments:
        ret = lib.ff_discrip_join_segment(j[0])
        if ret < 0:
            break
        for t, (time, dur, flags) in seg:
            held.append(ffi.new("DRFrame *", {"size": 1, "time": time, "dur": dur, "flags": flags, "src": time}))
            ret = lib.ff_discrip_join_push(j[0], t, held[-1])
            if ret < 0:
                break
        if ret < 0:
            break
    if ret >= 0:
        ret = lib.ff_discrip_join_finish(j[0])
    lib.ff_discrip_join_stats(j[0], st)
    lib.ff_discrip_join_close(j)
    return ret if ret < 0 else (sink, st)


def gop(base, first, n):
    """Video frames of a GOP in decode order I P B B ... from display position
    first (presentation times base + position x FRAME), the last one ending
    the batch."""
    v = [(0, fr(base + (first + 1) * FRAME, FRAME, F_KEY))]
    for k in range(n - 1):
        v.append((0, fr(base + (first + k + (1 if k != 0 else 0)) * FRAME, FRAME, 0)))
    v[-1][1][2] |= F_BATCH
    return v


def track_times(s, track):
    return [o[1] for o in s.out if o[0] == track]


def test_segments_follow_where_the_previous_video_really_ended():
    # segment 0: video times 1000 s.. (decode order: the I frame is displayed
    # after the first B frame, so the start is the batch's earliest time);
    # segment 1 restarts its own clock at 5 s
    b0 = 1000 * 1080000000
    b1 = 5 * 1080000000
    s0 = gop(b0, 0, 12) + [(1, fr(b0 + k * AC3, AC3, F_KEY)) for k in range(10)]
    s1 = gop(b1, 0, 12) + [(1, fr(b1 + k * AC3, AC3, F_KEY)) for k in range(10)]
    s, st = join([VIDEO, AUDIO], [], [s0, s1])
    v = track_times(s, 0)
    assert v[1] == 0, "segment 0 starts at its earliest video time"
    assert min(v) == 0
    end0 = 12 * FRAME  # the video of segment 0 ended here on the title timeline
    assert st.offset == end0
    assert min(v[12:]) == end0
    a = track_times(s, 1)
    assert a[0] == 0
    assert a[10] == end0, "segment 1's audio follows its own video"
    assert st.segments == 2


def test_audio_without_a_time_takes_where_the_track_is():
    s0 = gop(0, 0, 4) + [
        (1, fr(AV_NOPTS_VALUE, AC3, F_KEY)),
        (1, fr(AV_NOPTS_VALUE, AC3, F_KEY)),
        (1, fr(5 * AC3, AC3, F_KEY)),  # first timed frame: the two before it end there
        (1, fr(AV_NOPTS_VALUE, AC3, F_KEY)),
    ]
    s, _ = join([VIDEO, AUDIO], [], [s0])
    assert track_times(s, 1) == [3 * AC3, 4 * AC3, 5 * AC3, 6 * AC3]


def test_audio_early_by_more_than_its_duration_is_moved_and_reported_once():
    s0 = gop(0, 0, 4) + [(1, fr(k * AC3, AC3, F_KEY)) for k in range(5)] + [
        (1, fr(5 * AC3 - 2 * AC3, AC3, F_KEY)),  # 2 frames early: moved to 5 x AC3
        (1, fr(6 * AC3 - 2 * AC3, AC3, F_KEY)),  # same value again: moved, not reported
        (1, fr(7 * AC3 - AC3 // 2, AC3, F_KEY)),  # half a frame early: passes unchanged
    ]
    s, st = join([VIDEO, AUDIO], [], [s0])
    assert track_times(s, 1)[5:] == [5 * AC3, 6 * AC3, 7 * AC3 - AC3 // 2]
    assert len(s.events) == 1
    e = s.events[0]
    assert (e["kind"], e["pos"], e["skew"]) == (lib.DR_EV_RETIME, 5 * AC3, -2 * AC3)
    assert st.retimed == 2


def test_subtitles_keep_their_times():
    # the second starts 5 frames before the first ends (more than its own
    # 1 frame): it keeps its time (a sub-picture replaces the one shown);
    # the third overlaps the second by less than its duration: no event
    s0 = gop(0, 0, 4) + [
        (1, fr(10 * FRAME, 3 * FRAME, F_KEY)),
        (1, fr(8 * FRAME, FRAME, F_KEY)),
        (1, fr(8 * FRAME + FRAME // 2, 2 * FRAME, F_KEY)),
    ]
    s, st = join([VIDEO, SUBTITLE], [], [s0])
    assert track_times(s, 1) == [10 * FRAME, 8 * FRAME, 8 * FRAME + FRAME // 2]
    assert st.retimed == 0
    assert len(s.events) == 1
    e = s.events[0]
    assert (e["kind"], e["track"], e["pos"], e["dur"]) == (lib.DR_EV_SUB_OVERLAP, 1, 8 * FRAME, 5 * FRAME)


def test_chapter_marks_go_on_the_key_frame_at_or_just_before_them():
    # key frames every 12 frames; marks at 0, at 1 s, at 2.1 s
    s0 = []
    for g in range(8):
        s0 += gop(0, g * 12, 12)
    s, st = join([VIDEO], [0, 1080000000, 2268000000], [s0])
    # key frames at display positions 1, 13, 25, ... (x FRAME): the first one
    # >= mark - 0.4 s
    assert [o[1] for o in s.out if o[2] & F_CHAPTER] == [FRAME, 25 * FRAME, 61 * FRAME]
    assert st.chapters == 3


def test_a_segment_without_video_fails():
    assert isinstance(join([VIDEO, AUDIO], [], [gop(0, 0, 4), [(1, fr(0, AC3, F_KEY))]]), int)
