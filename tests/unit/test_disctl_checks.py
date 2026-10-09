"""The title checks of the master video's segments and joins
(disctl_title.c, with the marks of disctl_mpv.c and the counts of
disctl_video.c): how decoding starts at each segment's first key frame and
the pictures shown before it, pictures repeated at a join, end of sequence
codes inside a segment, what the video stage takes out and the scan it
reads. Nothing is changed by them: only events and statistics.

The test stream (FFmpeg's MPEG-2 encoder, tests/data/README.md) starts with
a closed GOP; its second GOP is open: its I picture is followed by two B
pictures shown before it (ISO/IEC 13818-2 6.3.8)."""

from helpers import lib
from helpers.disctl import codec_id, pictures
from test_disctl_title import AC3_DUR, BASE, M2V, ac3, interleave, run, tracks, video
from test_disctl_video import pes, run as run_video

VIDEO_ONLY = [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0]]
SEQ = b"\0\0\1\xb3"
GOP = b"\0\0\1\xb8"


def gop_starts(s):
    """Where each GOP begins: its sequence header (the stream puts one in
    front of every GOP)."""
    out, i = [], s.find(SEQ)
    while i >= 0:
        out.append(i)
        i = s.find(SEQ, i + 4)
    return out


def with_gop_flags(s, closed, broken):
    """The stream with its first GOP header's closed_gop / broken_link bits
    set as asked."""
    b = bytearray(s)
    g = b.find(GOP)
    b[g + 7] = (b[g + 7] & ~0x60) | (closed << 6) | (broken << 5)
    return bytes(b)


def join_starts(events):
    return [(e["dur"], e["skew"], e["count"]) for e in events if e["kind"] == lib.DR_EV_JOIN_START]


def of_kind(events, kind):
    return [e for e in events if e["kind"] == kind]


def title(*streams):
    """A video-only title: one segment per stream, each timed from its own
    clock. The outputs and the events."""
    outs, events, _, _ = run(VIDEO_ONLY, False, [video(BASE, s) for s in streams])
    return outs, events


def from_second_gop():
    return M2V[gop_starts(M2V)[1]:]


def test_a_segment_that_starts_with_a_closed_gop_is_a_clean_start():
    outs, events = title(M2V, M2V[:gop_starts(M2V)[1]])
    assert join_starts(events) == [(1, lib.DR_START_CLEAN, 0), (2, lib.DR_START_CLEAN, 0)]
    assert len(outs) == len(pictures(M2V)) + len(pictures(M2V[:gop_starts(M2V)[1]]))


def test_a_segment_that_starts_with_an_open_gop_says_its_leading_pictures_may_need_the_join():
    rest = from_second_gop()
    outs, events = title(M2V[:gop_starts(M2V)[1]], rest)
    assert join_starts(events) == [(1, lib.DR_START_CLEAN, 0), (2, lib.DR_START_OPEN, 2)]
    assert len(outs) == len(pictures(M2V[:gop_starts(M2V)[1]])) + len(pictures(rest)), "nothing is left out"


def test_an_open_gop_at_the_title_start_is_reported_too():
    _, events = title(from_second_gop())
    assert join_starts(events) == [(1, lib.DR_START_OPEN, 2)]


def test_a_broken_link_says_its_leading_pictures_cannot_be_decoded():
    _, events = title(M2V[:gop_starts(M2V)[1]], with_gop_flags(from_second_gop(), 0, 1))
    assert join_starts(events)[1] == (2, lib.DR_START_BROKEN, 2)


def test_closed_gop_makes_the_same_leading_pictures_a_clean_start():
    _, events = title(M2V[:gop_starts(M2V)[1]], with_gop_flags(from_second_gop(), 1, 0))
    assert join_starts(events)[1] == (2, lib.DR_START_CLEAN, 2)


def test_a_gop_on_both_sides_of_a_join_is_one_repeat_finding():
    g = gop_starts(M2V)
    first, second = M2V[:g[3]], M2V[g[2]:g[5]]     # GOP 3 at the end of the first and the start of the second
    _, events = title(first, second)
    rep = of_kind(events, lib.DR_EV_JOIN_REPEAT)
    assert len(rep) == 1
    assert rep[0]["count"] == len(pictures(M2V[g[2]:g[3]])), "every picture of the repeated GOP"
    assert rep[0]["skew"] == 2, "segment 2"


def test_equal_skipped_pictures_at_a_join_are_no_repeat():
    # the stream's B pictures of a still part code to the same bytes in
    # every GOP: equal single pictures across a join are no finding
    g = gop_starts(M2V)
    a, b = M2V[:g[3]], M2V[g[3]:g[5]]
    ends = {a[x:y] for x, y, _, _ in pictures(a)[-32:]}
    assert any(b[x:y] in ends for x, y, _, _ in pictures(b)[:32]), "the stream has such pictures"
    _, events = title(a, b)
    assert not of_kind(events, lib.DR_EV_JOIN_REPEAT)


def test_an_end_of_sequence_code_with_pictures_after_it_is_reported_and_kept():
    g = gop_starts(M2V)
    s = M2V[:g[2]] + b"\0\0\1\xb7" + M2V[g[2]:g[4]]
    _, events = title(s)
    assert len(of_kind(events, lib.DR_EV_SEQ_END_INSIDE)) == 1
    _, st = run_video(lambda d, k: pes(d), s)
    assert st.seq_end == 1 and st.seq_end_inside == 1


def test_an_end_of_sequence_code_at_the_end_is_no_finding():
    s = M2V + b"\0\0\1\xb7"
    _, events = title(s)
    assert not of_kind(events, lib.DR_EV_SEQ_END_INSIDE)
    _, st = run_video(lambda d, k: pes(d), s)
    assert st.seq_end == 1 and st.seq_end_inside == 0


def test_the_video_stage_counts_zero_stuffing_after_a_gop_header():
    b = bytearray(M2V)
    g = b.find(GOP)
    b[g + 8:g + 8] = bytes(100)                    # 100 zero bytes after the GOP header
    _, st = run_video(lambda d, k: pes(d), bytes(b))
    assert st.stuffing == 100
    _, st = run_video(lambda d, k: pes(d))
    assert st.stuffing == 0


def test_the_scan_of_every_picture_is_read():
    _, st = run_video(lambda d, k: pes(d))
    assert st.scan_known == st.out == len(pictures(M2V))
    # FFmpeg's encoder made a progressive stream of frame pictures
    assert st.progressive == st.out and st.rff == 0
    assert st.pictures == st.out, "every picture read is given out"


def test_audio_in_sync_with_its_disc_times_is_no_sync_finding():
    n = 1200 * 36036 // 34560
    seg = interleave(video(BASE), ac3(BASE, AC3_DUR, n))
    _, events, _, _ = run(tracks("ac3"), True, [seg])
    assert not of_kind(events, lib.DR_EV_SYNC)
