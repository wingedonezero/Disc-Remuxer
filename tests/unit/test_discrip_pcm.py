"""libavformat/discrip_pcm.c: the PCM strategy of the rip core. Input frames
carry 16-bit stereo 48 kHz samples whose values are their own sample
numbers, so the output shows exactly which samples were written, where
silence went, and what was cut. Output frames are 1600 samples (1/30 s),
timed by a sample counter from 0."""

import struct

from helpers import ffi, lib
from helpers.discrip import Frame, Handles, events_into, video_ref

RATE = 48000
T = 22500  # ticks per sample at 48 kHz
MS = 1080000
N = 80  # samples per input frame (DVD-Video LPCM at 48 kHz)
F_KEY = lib.DR_F_KEY
I64_MAX = (1 << 63) - 1


class Video:
    def __init__(self, end):
        self.end = end

    def max_time(self):
        return self.end

    def advance(self, target):
        return 0, self.end < target


class Sink:
    def __init__(self):
        self.samples = []  # left channel of every sample written
        self.frames = []
        self.events = []


def val(k):
    return k % 65000 + 1


def frame(time, first, n):
    """An input frame at time holding samples first .. first + n
    (little-endian, both channels the sample number; 0 is kept for silence)."""
    d = b"".join(struct.pack("<HH", val(k), val(k)) for k in range(first, first + n))
    return [d, time, n * T]


def run(frames, video_end):
    sink = Sink()

    def on_out(f):
        b = bytes(ffi.buffer(f.data, f.size))
        sink.samples += [struct.unpack_from("<H", b, i)[0] for i in range(0, len(b), 4)]
        sink.frames.append((f.time, f.dur, f.flags))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    cfg = ffi.new("DRPcmConfig *", {
        "track": 1, "rate": RATE, "bits": 16, "bytes_per_sample_frame": 4,
        "video": video_ref(Video(video_end), keep),
        "out": lib.tb_py_frame, "out_opaque": keep(on_out),
        "event": lib.tb_py_event, "event_opaque": keep(events_into(sink.events))})
    p = ffi.new("DRPcm **")
    st = ffi.new("DRPcmStats *")
    assert lib.ff_discrip_pcm_open(p, ffi.NULL, cfg) == 0
    held = []  # the stage may keep pointers to the bytes until it is closed
    for d, time, dur in frames:
        held.append(Frame(d, time, dur, F_KEY))
        assert lib.ff_discrip_pcm_push(p[0], held[-1].ptr) == 0
    assert lib.ff_discrip_pcm_finish(p[0]) == 0
    lib.ff_discrip_pcm_stats(p[0], st)
    lib.ff_discrip_pcm_close(p)
    return sink, st


def run_of(time, first, count):
    """count frames from time, samples from first, back to back."""
    return [frame(time + k * N * T, first + k * N, N) for k in range(count)]


def kinds(s):
    return [e["kind"] for e in s.events]


def test_back_to_back_frames_leave_in_frames_of_1600_samples():
    s, st = run(run_of(0, 0, 50), I64_MAX)
    assert s.samples == [val(k) for k in range(50 * N)]
    assert len(s.frames) == 3
    assert s.frames[0] == (0, 1600 * T, F_KEY)
    assert s.frames[1][0] == 1600 * T
    assert s.frames[2] == (3200 * T, 800 * T, F_KEY), "the last one as it is"
    assert s.events == [], s.events
    assert (getattr(st, "in"), st.out) == (50, 3)


def test_a_lead_in_within_1_ms_is_kept_and_its_track_starts_at_0():
    # 0.8 ms before the video: all samples, from 0
    s, _ = run(run_of(-864000, 0, 30), I64_MAX)
    assert s.samples == [val(k) for k in range(30 * N)]
    assert s.frames[0][0] == 0
    assert kinds(s) == [lib.DR_EV_START_SHIFT]


def test_frames_more_than_1_ms_early_are_dropped():
    # frames at -9.4, -7.73, -6.07, -4.4, -2.73, -1.07 ms ... (1.667 ms each):
    # those before -1 ms dropped; the first left is 1.07 ms early, inside 1 ms?
    # no: 1.067 ms > 1 ms is dropped too; the one at +0.6 ms starts the track
    # after 0.6 ms of silence
    start = -9 * MS - 400 * 1080
    s, st = run(run_of(start, 0, 40), I64_MAX)
    first_kept = next(k for k in range(40) if start + k * N * T >= -MS)
    assert st.dropped == first_kept
    t0 = start + first_kept * N * T
    lead = t0 * RATE // 1080000000
    assert t0 >= 0 and lead > 0
    assert s.samples[:lead] == [0] * lead
    assert s.samples[lead] == val(first_kept * N)
    assert (lib.DR_EV_START_DROP in kinds(s)) == (abs(t0) > 3 * MS)


def test_a_late_start_below_5_s_is_filled_with_silence_from_5_s_on_skipped():
    s, _ = run(run_of(100 * MS, 0, 10), I64_MAX)
    assert s.samples[:4800] == [0] * 4800, "100 ms of silence"
    assert s.samples[4800] == val(0)
    assert lib.DR_EV_PCM_SILENCE in kinds(s)
    s, _ = run(run_of(6000 * MS, 0, 30), I64_MAX)
    assert s.samples[0] == val(0), "no silence written"
    assert s.frames[0][0] == 6 * 1080000000, "the first frame at 6 s"
    assert kinds(s) == [lib.DR_EV_PCM_SKIP]


def test_a_gap_is_filled_with_silence():
    # 30 frames, a 50 ms gap, 30 frames
    f = run_of(0, 0, 30) + run_of(30 * N * T + 50 * MS, 30 * N, 30)
    s, st = run(f, I64_MAX)
    gap = 50 * MS // T
    assert st.silence == gap
    assert s.samples == [val(k) for k in range(30 * N)] + [0] * gap + [val(k) for k in range(30 * N, 60 * N)]


def test_frames_with_a_broken_time_are_appended_where_they_follow_on():
    # frames 10..12 carry times 50 ms late; frame 13 goes on from where 9
    # ended without them: they were broken, not a gap
    f = run_of(0, 0, 20)
    for g in f[10:13]:
        g[1] += 50 * MS
    s, st = run(f, I64_MAX)
    assert s.samples == [val(k) for k in range(20 * N)]
    assert st.silence == 0
    assert st.broken == 3
    e = [x for x in s.events if x["kind"] == lib.DR_EV_PCM_TIMECODE]
    assert (len(e), e[0]["count"]) == (1, 3)


def test_a_frame_running_into_the_next_is_cut_at_its_start():
    # frame 10 starts 10 samples before frame 9 ends: frame 9 is cut
    f = run_of(0, 0, 20)
    for k in range(10, 20):
        f[k] = frame(k * N * T - 10 * T, k * N, N)
    s, st = run(f, I64_MAX)
    assert s.samples == [val(k) for k in range(10 * N - 10)] + [val(k) for k in range(10 * N, 20 * N)]
    assert st.overlap == 1


def test_audio_after_the_end_of_the_video_is_left_out():
    # a gap at 40 frames; the video ends before the frames after it
    f = run_of(0, 0, 40) + run_of(40 * N * T + 500 * MS, 40 * N, 20)
    s, _ = run(f, 40 * N * T + 100 * MS)
    assert s.samples == [val(k) for k in range(40 * N)]
    assert lib.DR_EV_VIDEO_ENDED in kinds(s)
