"""libavformat/disctl_junction.c: the audio junction of the title timeline. Every
case is built from an event the reference program logged on a real disc,
with the numbers in ticks (1/1,080,000,000 s): the title start (every frame
kept at its own time, also before the video: the reference program dropped
and shifted such a lead-in, which put the track out of sync by the part it
moved), overlaps growing the skew, short gaps absorbed, real gaps (skew
reset, nothing inserted), whole-frame drops only up to a sync unit."""

from helpers import ffi, lib
from helpers.disctl import Handles, events_into, video_ref

MS = 1080000
AC3 = 34560000  # 1536 samples at 48 kHz = 32 ms
I64_MAX = (1 << 63) - 1
EV = lib


class Video:
    """Video seen by the junction: handed on up to max, ends at end."""

    def __init__(self, end=I64_MAX):
        self.max = 0
        self.end = end

    def max_time(self):
        return self.max

    def advance(self, target):
        self.max = max(self.max, min(target, self.end))
        return 0, self.max < target


class Sink:
    def __init__(self):
        self.out = []  # time, dur, flags
        self.events = []


def fr(time, dur, sync):
    """Input frame: time, duration, sync unit (no bytes, size 1)."""
    return (time, dur, lib.DR_F_KEY | lib.DR_F_SYNC if sync else 0)


def run_of(start, dur, n, sync):
    """Contiguous frames of dur from start."""
    return [fr(start + k * dur, dur, sync) for k in range(n)]


def junction(frames, frame_dur, video):
    sink = Sink()

    def on_out(f):
        sink.out.append((f.time, f.dur, f.flags))
        lib.ff_disctl_frame_unref(f)
        return 0
    keep = Handles()
    cfg = ffi.new("DRJunctionConfig *", {
        "track": 1, "frame_dur": frame_dur, "video": video_ref(video, keep),
        "out": lib.tb_py_frame, "out_opaque": keep(on_out),
        "event": lib.tb_py_event, "event_opaque": keep(events_into(sink.events)), "codec": 0, "rate": 0})
    j = ffi.new("DRJunction **")
    st = ffi.new("DRJunctionStats *")
    assert lib.ff_disctl_junction_open(j, ffi.NULL, cfg) == 0
    held = []
    for time, dur, flags in frames:
        held.append(ffi.new("DRFrame *", {"size": 1, "time": time, "dur": dur, "flags": flags, "src": time}))
        assert lib.ff_disctl_junction_push(j[0], held[-1]) == 0
    assert lib.ff_disctl_junction_finish(j[0]) == 0
    lib.ff_disctl_junction_stats(j[0], st)
    lib.ff_disctl_junction_close(j)
    return sink, st


def kinds(s):
    return [e["kind"] for e in s.events]


def test_a_lead_in_keeps_every_frame_at_its_time():
    # Patch Adams t00 (HD DVD): first audio frame 1.266 ms before the video
    lead = 1367280
    s, st = junction(run_of(-lead, AC3, 20, True), AC3, Video())
    assert len(s.out) == 20
    assert s.out[0][0] == -lead, "the first frame keeps its time: a negative delay"
    assert all(o[0] == -lead + k * AC3 for k, o in enumerate(s.out))
    assert s.events == [{"kind": EV.DR_EV_START_EARLY, "track": 1, "pos": 0, "dur": lead, "skew": 0, "count": 0}]
    assert (st.skew, st.base) == (0, 0)


def test_overlap_grows_the_skew_and_short_gaps_are_absorbed():
    # Patch Adams t00: lead-in 1.266 ms; at 22.66 s a frame 26.666 ms early
    # (skew 27.933 ms); later gaps of 1.166 ms and 4.166 ms are absorbed
    lead = 1367280
    ov = 28799280  # 26.666 ms
    g1 = 1259280  # 1.166 ms
    g2 = 4499280  # 4.166 ms
    frames = run_of(-lead, AC3, 10, True)
    t = -lead + 10 * AC3 - ov
    frames += run_of(t, AC3, 10, True)
    t += 10 * AC3 + g1
    frames += run_of(t, AC3, 10, True)
    t += 10 * AC3 + g2
    frames += run_of(t, AC3, 10, True)
    s, st = junction(frames, AC3, Video())
    assert kinds(s) == [EV.DR_EV_START_EARLY, EV.DR_EV_OVERLAP, EV.DR_EV_GAP_ABSORBED, EV.DR_EV_GAP_ABSORBED]
    assert s.events[1]["dur"] == ov
    assert s.events[1]["skew"] == ov, "26.666 ms: the lead-in is no skew"
    assert s.events[2]["skew"] == ov - g1, "25.5 ms"
    assert s.events[3]["skew"] == ov - g1 - g2, "21.333 ms"
    assert len(s.out) == 40, "no frame dropped: 32 ms > 26.666 ms"
    assert st.dropped == 0
    # frames leave back to back except where the skew changed
    assert all(b[0] >= a[0] for a, b in zip(s.out, s.out[1:]))


def test_a_real_gap_resets_the_skew_and_inserts_nothing():
    # Patch Adams t01: E-AC-3 frames of 256 samples (5.333 ms); a 56.033 ms
    # gap -> "audio gap - 10.506 missing frame(s)"
    eac3 = 5760000
    gap = 60515640  # 56.033 ms
    frames = run_of(0, eac3, 30, True) + run_of(30 * eac3 + gap, eac3, 30, True)
    s, st = junction(frames, eac3, Video())
    assert kinds(s) == [EV.DR_EV_GAP]
    e = s.events[0]
    assert (e["pos"], e["dur"], e["skew"]) == (30 * eac3, gap, 0)
    assert e["count"] == 10506, "10.506 missing frames (x1000)"
    assert len(s.out) == 60
    assert s.out[30][0] == 30 * eac3 + gap, "the gap stays a gap"
    assert st.skew == 0


def test_a_large_overlap_drops_whole_frames_up_to_a_sync_unit():
    # DVDDEMYSTIFIED2 (DVD): a 2942.633 ms overlap on 32 ms AC-3 frames ->
    # "92 frame(s) dropped to reduce audio skew to -1.366ms"
    ov = 3178043640  # 2942.633 ms
    frames = run_of(0, AC3, 200, True) + run_of(200 * AC3 - ov, AC3, 200, True)
    s, st = junction(frames, AC3, Video())
    assert kinds(s) == [EV.DR_EV_OVERLAP, EV.DR_EV_DROP]
    d = s.events[1]
    assert d["count"] == 92, "92 x 32 ms = 2944 ms <= 2942.633 + 2 ms"
    assert d["dur"] == 92 * AC3
    assert d["skew"] == ov - 92 * AC3, "-1.367 ms"
    assert st.dropped == 92
    assert len(s.out) == 400 - 92


def test_frames_are_never_dropped_when_no_sync_unit_follows():
    ov = 10 * AC3
    frames = run_of(0, AC3, 20, False) + run_of(20 * AC3 - ov, AC3, 20, False)
    s, st = junction(frames, AC3, Video())
    assert kinds(s) == [EV.DR_EV_OVERLAP]
    assert st.dropped == 0
    assert st.skew == ov


def test_a_long_lead_in_keeps_every_frame_at_its_time():
    # SAINTBEAST #3 (DVD): the audio starts 216.133 ms before the video. The
    # reference program dropped 6 frames and moved the 7th (24.133 ms before
    # the video) to 0: the whole track 24.133 ms late. Kept as it is: a
    # stream file of every frame with a -216.133 ms delay.
    first = -(6 * AC3 + 26063640)
    s, st = junction(run_of(first, AC3, 20, True), AC3, Video())
    assert kinds(s) == [EV.DR_EV_START_EARLY]
    assert s.events[0]["dur"] == -first
    assert len(s.out) == 20
    assert [o[0] for o in s.out] == [first + k * AC3 for k in range(20)], "every frame at its own time"
    assert (st.dropped, st.skew, st.base) == (0, 0, 0)


def test_a_lead_in_inside_one_frame_is_kept_too():
    # UHD Resident Evil: the audio 8 ms before the video (the reference
    # program moved it to 0: 8 ms late)
    lead = 8 * MS
    s, _ = junction(run_of(-lead, AC3, 5, True), AC3, Video())
    assert kinds(s) == [EV.DR_EV_START_EARLY]
    assert [o[0] for o in s.out] == [-lead + k * AC3 for k in range(5)]


def test_a_track_starting_up_to_3_ms_after_the_video_keeps_its_delay():
    late = 2 * MS
    s, st = junction(run_of(late, AC3, 5, True), AC3, Video())
    assert kinds(s) == [EV.DR_EV_START_GAP]
    assert s.out[0][0] == late
    assert st.skew == 0
    s, _ = junction(run_of(5 * MS, AC3, 5, True), AC3, Video())
    assert s.events == [], "later than 3 ms: kept, no event"


def test_a_long_gap_while_the_video_goes_on_gets_markers_every_second():
    # ANGEL_S5D1 (DVD): 11-12 s gaps -> markers, then the gap line
    gap = 11 * 1080000000
    frames = run_of(0, AC3, 10, True) + run_of(10 * AC3 + gap, AC3, 10, True)
    s, _ = junction(frames, AC3, Video())
    markers = sum(1 for e in s.events if e["kind"] == EV.DR_EV_GAP_MARKER)
    assert markers >= 6, f"{markers} markers"
    assert kinds(s)[-1] == EV.DR_EV_GAP
    assert sum(1 for o in s.out if o[2] & lib.DR_F_MARKER) == markers
    assert sum(1 for o in s.out if not o[2] & lib.DR_F_MARKER) == 20, "markers carry no audio"


def test_audio_after_the_end_of_the_video_is_left_out():
    gap = 2 * 1080000000
    frames = run_of(0, AC3, 10, True) + run_of(10 * AC3 + gap, AC3, 10, True)
    s, st = junction(frames, AC3, Video(end=10 * AC3))
    assert kinds(s) == [EV.DR_EV_VIDEO_ENDED]
    assert len(s.out) == 10
    assert st.ended == 1
