"""src/timeline/disctl_junction.c: the audio junction of the title timeline. Every
case is built from an event the reference program logged on a real disc,
with the numbers in ticks (1/1,080,000,000 s): the title start (every frame
kept at its own time, also before the video: the reference program dropped
and shifted such a lead-in, which put the track out of sync by the part it
moved), overlaps growing the skew, short gaps absorbed, real gaps (skew
reset, nothing inserted), whole-frame drops only up to a sync unit."""

from helpers import ffi, lib
from helpers.disctl import Handles, codec_id, events_into, video_ref

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
        self.bytes = []  # the bytes of each frame handed on
        self.events = []


def fr(time, dur, sync):
    """Input frame: time, duration, sync unit (no bytes, size 1)."""
    return (time, dur, lib.DR_F_KEY | lib.DR_F_SYNC if sync else 0)


def run_of(start, dur, n, sync):
    """Contiguous frames of dur from start."""
    return [fr(start + k * dur, dur, sync) for k in range(n)]


def junction(frames, frame_dur, video, codec=0, rate=0):
    """frames: (time, dur, flags), (time, dur, flags, segment, bytes) or
    (time, dur, flags, segment, bytes, sample number)."""
    sink = Sink()

    def on_out(f):
        sink.out.append((f.time, f.dur, f.flags))
        sink.bytes.append(bytes(ffi.buffer(f.data, f.size)) if f.data != ffi.NULL else None)
        lib.ff_disctl_frame_unref(f)
        return 0
    keep = Handles()
    cfg = ffi.new("DRJunctionConfig *", {
        "track": 1, "frame_dur": frame_dur, "video": video_ref(video, keep),
        "out": lib.tb_py_frame, "out_opaque": keep(on_out),
        "event": lib.tb_py_event, "event_opaque": keep(events_into(sink.events)), "codec": codec, "rate": rate})
    j = ffi.new("DRJunction **")
    st = ffi.new("DRJunctionStats *")
    assert lib.ff_disctl_junction_open(j, ffi.NULL, cfg) == 0
    held = []
    for time, dur, flags, *more in frames:
        f = {"size": 1, "time": time, "dur": dur, "flags": flags, "src": time}
        if more:
            seg, payload = more[:2]
            data = ffi.new("uint8_t[]", payload)
            held.append(data)
            f.update({"seg": seg, "data": data, "size": len(payload)})
            if len(more) > 2:
                f.update({"ctr": more[2] & 0xFFFF, "flags": flags | lib.DR_F_CTR})
        held.append(ffi.new("DRFrame *", f))
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
    assert st.skew == 0


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
    assert (st.gaps, st.gap_dur) == (1, gap), "counted as a gap left open (the stream file closes it)"


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
    assert (st.dropped, st.skew) == (0, 0)


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


# ---- the copy check at a join ----
# Resident Evil UHD 00879 (Blu-ray, 8 seamless joins): clip B's first frame is
# a byte copy of clip A's last one on French / Spanish AC-3 (stamps overlap by
# one frame), English AC-3 core (0.29-0.86 frame) and DTS-HD MA (0.58-0.88
# frame). The copy is left out (B's frame), A's end stays.

TICK = 12000  # one 90 kHz tick


def seg_run(seg, start, dur, n, tag):
    """n contiguous AC-3-like frames of a segment, each with its own bytes."""
    return [(start + k * dur, dur, lib.DR_F_KEY | lib.DR_F_SYNC, seg, b"%s%03d" % (tag, k)) for k in range(n)]


def join(a, b_start, b_first=None, b_n=10):
    """A = 10 frames of segment 0 from 0; B = segment 1 from b_start; B's
    first frames replaced by b_first (bytes) when given."""
    b = seg_run(1, b_start, AC3, b_n, b"B")
    for k, payload in enumerate(b_first or []):
        b[k] = b[k][:4] + (payload,)
    return a + b


A = seg_run(0, 0, AC3, 10, b"A")
A_END = 10 * AC3


def test_a_copy_at_a_one_frame_overlap_is_left_out():
    s, st = junction(join(A, A_END - AC3, [b"A009"]), AC3, Video())
    assert kinds(s) == [EV.DR_EV_AUDIO_COPY]
    e = s.events[0]
    assert (e["pos"], e["dur"], e["count"]) == (A_END - AC3, AC3, AC3)
    assert s.bytes == [b"A%03d" % k for k in range(10)] + [b"B%03d" % k for k in range(1, 10)]
    assert s.out[10][0] == A_END, "B's second frame follows A's end"
    assert (st.dropped, st.copies, st.skew) == (1, 1, 0)


def test_a_copy_at_a_partial_overlap_is_left_out_too():
    # English AC-3 core, join 4: 9.178 ms of overlap
    ov = 9912240
    s, st = junction(join(A, A_END - ov, [b"A009"]), AC3, Video())
    assert EV.DR_EV_AUDIO_COPY in kinds(s)
    assert s.bytes[10] == b"B001"
    assert (st.dropped, st.copies) == (1, 1)
    assert s.out[10][0] == A_END, "a short gap after the copy is taken from the skew"


def test_the_same_bytes_without_an_overlap_are_no_copy():
    # ANGEL S2D1 (DVD): byte-equal digital silence on both sides, 0.000 ms overlap
    s, st = junction(join(A, A_END, [b"A009"]), AC3, Video())
    assert s.events == []
    assert len(s.out) == 20 and s.bytes[10] == b"A009", "kept"
    assert st.dropped == 0
    s, st = junction(join(A, A_END - TICK, [b"A009"]), AC3, Video())
    assert EV.DR_EV_AUDIO_COPY not in kinds(s), "one tick is no overlap"
    assert st.copies == 0


def test_different_bytes_at_an_overlap_are_kept():
    ov = AC3 // 2
    s, st = junction(join(A, A_END - ov), AC3, Video())
    assert kinds(s) == [EV.DR_EV_OVERLAP], "no copy: the overlap only grows the skew"
    assert s.bytes[10] == b"B000"
    assert (st.copies, st.dropped, st.skew) == (0, 0, ov)


def test_at_most_one_frame_per_join_is_left_out():
    # B starts with A's last frame twice: only the first is a copy at the join
    s, st = junction(join(A, A_END - 2 * AC3, [b"A009", b"A009"]), AC3, Video())
    assert kinds(s).count(EV.DR_EV_AUDIO_COPY) == 1
    assert st.copies == 1


def test_a_copy_of_two_frames_is_found_and_nothing_left_out():
    s, st = junction(join(A, A_END - 2 * AC3, [b"A008", b"A009"]), AC3, Video())
    assert EV.DR_EV_AUDIO_COPY not in kinds(s)
    e = [e for e in s.events if e["kind"] == EV.DR_EV_AUDIO_COPY_LONG]
    assert len(e) == 1
    assert (e[0]["pos"], e[0]["dur"], e[0]["count"]) == (A_END - 2 * AC3, 2 * AC3, 2)
    assert st.copies == 0, "found only: the copy check leaves nothing out"
    # the skew rule then goes on as before: two frames of skew, two dropped
    assert st.dropped == 2 and s.bytes[10] == b"B002"


def test_an_overlap_one_tick_short_of_a_frame_counts_as_one_frame():
    # Resident Evil French, join 3: the stamps overlap by 32 ms less half a
    # 90 kHz tick (the log shows 31.994 ms): the join is placed where the last
    # picture ends, its stamp in whole ticks plus 3753.75 ticks (23.976 fps).
    # B differs from A here, so the skew decides: one frame dropped, not
    # carried on to the next join
    ov = AC3 - TICK // 2
    s, st = junction(join(A, A_END - ov), AC3, Video())
    assert EV.DR_EV_DROP in kinds(s)
    assert st.dropped == 1
    assert abs(st.skew) <= TICK


def test_a_gap_of_one_frame_within_one_tick_is_a_missing_frame():
    # 32 ms less half a tick between two AC-3 frames (32 ms): one frame missing, left open
    # (not closed by moving the rest of the track one frame earlier)
    gap = AC3 - TICK // 2
    frames = run_of(0, AC3, 10, True) + run_of(10 * AC3 + gap, AC3, 10, True)
    s, st = junction(frames, AC3, Video())
    assert kinds(s) == [EV.DR_EV_GAP]
    assert s.out[10][0] == 10 * AC3 + gap, "the gap stays"
    assert st.skew == 0
    # a gap short of one frame by more than a tick is still closed
    gap = AC3 - TICK - 1
    frames = run_of(0, AC3, 10, True) + run_of(10 * AC3 + gap, AC3, 10, True)
    s, st = junction(frames, AC3, Video())
    assert kinds(s) == [EV.DR_EV_GAP_ABSORBED]


# ---- TrueHD joins by the streams' sample numbers ----
# Each clip's TrueHD is a stream of its own; every restart header carries the
# number of its first sample, the units after it count on by 40 (48 kHz).
# Aliens / Resident Evil UHD: every clip starts with two major syncs, and its
# first unit is stamped on its first picture, so the stamps show an overlap of
# 0..0.96 unit at every join whether a unit repeats or not.

THD = 900000  # 40 samples at 48 kHz
MAJOR = lib.DR_F_KEY | lib.DR_F_SYNC


def thd_seg(seg, start, n, first):
    """n units from time start whose first sample number is first; the first
    two and every 16th are major syncs."""
    return [(start + k * THD, THD, MAJOR if k < 2 or k % 16 == 0 else 0, seg, b"%c%04d" % (65 + seg, k), first + 40 * k)
            for k in range(n)]


def thd_junction(frames):
    return junction(frames, THD, Video(), codec_id("truehd"), 48000)


def joins(s):
    return [e["count"] for e in s.events if e["kind"] == EV.DR_EV_SAMPLE_JOIN]


def test_a_segment_that_continues_the_sample_numbers_keeps_everything_whatever_the_stamps():
    # Aliens SE: three joins in a row whose stamps overlap by 0.6 unit each;
    # the old skew rule would have dropped a real unit at the second
    ov = THD * 6 // 10
    a = thd_seg(0, 0, 40, 1000)
    b = thd_seg(1, 40 * THD - ov, 40, 1000 + 40 * 40)
    c = thd_seg(2, 80 * THD - 2 * ov, 40, 1000 + 80 * 40)
    s, st = thd_junction(a + b + c)
    assert joins(s) == [lib.DR_JOIN_CONTINUES] * 2
    assert len(s.out) == 120 and st.dropped == 0
    assert s.out[40][0] == 40 * THD and s.out[80][0] == 80 * THD, "each segment follows on"
    assert list(st.joins)[lib.DR_JOIN_CONTINUES] == 2


def test_a_first_unit_holding_the_last_samples_again_is_left_out():
    # Resident Evil 00879 join 2: B's second unit starts where A ends
    a = thd_seg(0, 0, 40, 65000)               # the counter wraps inside A
    b = thd_seg(1, 40 * THD - THD * 63 // 100, 40, (65000 + 40 * 40 - 40) & 0xFFFF)
    s, st = thd_junction(a + b)
    assert joins(s) == [lib.DR_JOIN_SAME]
    e = [e for e in s.events if e["kind"] == EV.DR_EV_SAMPLE_JOIN][0]
    assert e["dur"] == 40
    assert len(s.out) == 79 and st.dropped == 1
    assert s.bytes[40] == b"B0001", "B's second unit follows A"
    assert s.out[40][0] == 40 * THD
    assert all(b[0] > a[0] for a, b in zip(s.out, s.out[1:]))


def test_a_repeated_first_unit_stays_when_no_major_sync_follows_it():
    a = thd_seg(0, 0, 40, 0)
    b = thd_seg(1, 40 * THD, 40, 40 * 40 - 40)
    b[1] = b[1][:2] + (0,) + b[1][3:]          # B's second unit a minor sync
    s, st = thd_junction(a + b)
    assert joins(s) == [lib.DR_JOIN_SAME_KEPT]
    assert len(s.out) == 80 and st.dropped == 0


def test_sample_numbers_that_do_not_connect_leave_the_join_to_the_skew_rule():
    # a piece of another cut: its own timeline; the stamps overlap by a unit
    a = thd_seg(0, 0, 40, 0)
    b = thd_seg(1, 39 * THD, 40, 30000)
    s, st = thd_junction(a + b)
    assert joins(s) == [lib.DR_JOIN_APART]
    e = [e for e in s.events if e["kind"] == EV.DR_EV_SAMPLE_JOIN][0]
    assert e["dur"] == ((1600 - 30000 + 0x8000) & 0xFFFF) - 0x8000
    assert EV.DR_EV_DROP in kinds(s) and st.dropped == 1, "a whole unit of overlap: the skew rule drops it"


def test_a_side_without_sample_numbers_leaves_the_join_to_the_skew_rule():
    a = thd_seg(0, 0, 40, 0)
    b = [f[:5] for f in thd_seg(1, 40 * THD, 40, 1600)]
    s, st = thd_junction(a + b)
    assert joins(s) == [lib.DR_JOIN_NO_COUNT]
    assert len(s.out) == 80
