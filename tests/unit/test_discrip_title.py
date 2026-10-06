"""One title through every stage of the rip core (discrip_title.c): payloads
of an MPEG-2 video track and an AC-3 (or LPCM) track in one or two segments
in, joined, junctioned and checked frames out. Audio frames leave only once
the video has been handed on past their time."""

import dataclasses

from helpers import DATA, ffi, lib
from helpers.discrip import Handles, codec_id, events_into, pictures

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
FRAME = 36036000  # 1001/30000 s
AC3_DUR = 34560000  # 1536 samples at 48 kHz
MS = 1080000
BASE = 10 * 1080000000
EAGAIN = -11
EOF_ = -0x20464F45


@dataclasses.dataclass
class Payload:
    """A payload of a track: bytes, PES time, from the source's tail; order
    places it among the other track's payloads (video: its decode time)."""
    track: int
    data: bytes
    time: int
    tail: bool
    order: int


@dataclasses.dataclass
class Out:
    """An output frame: track, time, duration, flags, the largest video time
    given out before it."""
    track: int
    time: int
    dur: int
    flags: int
    vmax_before: int


def video(base):
    """The video payloads of one segment: every picture of the test stream
    with its PES time on the grid from base."""
    return [Payload(0, M2V[a:b], base + disp * FRAME, False, base + (k - 1) * FRAME)
            for k, (a, b, disp, _) in enumerate(pictures(M2V))]


def ac3(start, step, n):
    """AC-3 payloads: n frames (the test stream's frames over and over), one
    per payload, the first at start, each step after the one before."""
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    return [Payload(1, frames[k % len(frames)], start + k * step, False, start + k * step) for k in range(n)]


def interleave(a, b):
    """Payloads interleaved by decode time (the way they lie in a program
    stream); each track keeps its own order."""
    return sorted(a + b, key=lambda p: p.order)


def run(tracks, tolerance, lpcm_hd, segments):
    """(outputs, events, duration, review)."""
    events = []
    outs = []
    vmax = [-(1 << 63)]
    keep = Handles()
    tr = ffi.new("DRTitleTrack[]", tracks)
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": len(tracks), "tracks": tr, "tolerance": tolerance,
                                      "lpcm_hd": int(lpcm_hd), "event": lib.tb_py_event,
                                      "event_opaque": keep(events_into(events))})
    t = ffi.new("DRTitle **")

    def drain():
        while True:
            f = ffi.new("DRFrame *")
            k = ffi.new("int *")
            r = lib.ff_discrip_title_frame(t[0], k, f)
            if r != 0:
                return r
            outs.append(Out(k[0], f.time, f.dur, f.flags, vmax[0]))
            if k[0] == 0:
                vmax[0] = max(vmax[0], f.time)
            lib.ff_discrip_frame_unref(f)
    assert lib.ff_discrip_title_open(t, ffi.NULL, cfg) == 0
    for seg in segments:
        assert lib.ff_discrip_title_segment(t[0]) == 0
        for p in seg:
            assert lib.ff_discrip_title_payload(t[0], p.track, p.data, len(p.data), p.time, int(p.tail)) == 0
            assert drain() == EAGAIN, "EAGAIN while the title goes on"
    assert lib.ff_discrip_title_finish(t[0]) == 0
    assert drain() == EOF_, "AVERROR_EOF after the last frame"
    duration, review = lib.ff_discrip_title_duration(t[0]), lib.ff_discrip_title_review(t[0])
    lib.ff_discrip_title_close(t)
    return outs, events, duration, review


def tracks(audio):
    return [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0], [codec_id(audio), lib.DR_KIND_AUDIO, 0]]


def kinds(events):
    return [e["kind"] for e in events if e["track"] == 1]


def test_one_segment_starts_at_its_video_and_audio_waits_for_the_video():
    # audio starts 5 ms before the video: within HD DVD's 102 ms it is kept
    # and the whole track shifted by it
    n = 1200 * 36036 // 34560
    seg = interleave(video(BASE), ac3(BASE - 5 * MS, AC3_DUR, n))
    outs, events, duration, review = run(tracks("ac3"), 110160000, True, [seg])
    v = [o for o in outs if o.track == 0]
    a = [o for o in outs if o.track == 1]
    assert len(v) == 1200
    assert min(o.time for o in v) == 0, "the title starts at its first video time"
    assert duration == 1200 * FRAME
    assert review == 0, "nothing untested"
    assert len(a) == n
    assert a[0].time == 0, "the lead-in became a shift"
    assert kinds(events) == [lib.DR_EV_START_SHIFT]
    assert events[0]["dur"] == 5 * MS
    for k, o in enumerate(a):
        assert o.time == k * AC3_DUR
        assert o.dur == AC3_DUR
    # every audio frame left after the video reached its time on the title
    # timeline (its output time less the 5 ms shift), except those after the
    # last video frame (they leave when the video has ended)
    last_video = max(o.time for o in v)
    for o in a:
        t = o.time - 5 * MS
        assert o.vmax_before >= t or t > last_video, f"audio at {t} left at video {o.vmax_before}"


def test_the_second_segment_follows_the_video_of_the_first():
    # segment 2 has its own clock; its audio starts 20 ms before its video,
    # segment 1's audio runs on 15 ms past its video: an overlap at the join
    n1 = 1200 * 36036 // 34560 + 1
    s1 = interleave(video(BASE), ac3(BASE, AC3_DUR, n1))
    base2 = 500 * 1080000000
    s2 = interleave(video(base2), ac3(base2 - 20 * MS, AC3_DUR, 100))
    outs, events, duration, review = run(tracks("ac3"), 110160000, True, [s1, s2])
    assert len([o for o in outs if o.track == 0]) == 2400
    assert duration == 2400 * FRAME, "segment 2 is placed where segment 1's video ended"
    assert review == 0
    ev = kinds(events)
    assert lib.DR_EV_OVERLAP in ev, events
    assert lib.DR_EV_GAP not in ev, events
    a = [o for o in outs if o.track == 1]
    assert all(b.time >= x.time for x, b in zip(a, a[1:])), "audio leaves in time order"


def test_payloads_from_the_source_tail_mark_their_frames():
    n = 1200 * 36036 // 34560
    a = ac3(BASE, AC3_DUR, n)
    for p in a[n - 10:]:
        p.tail = True
    outs, _, _, _ = run(tracks("ac3"), 110160000, True, [interleave(video(BASE), a)])
    a = [o for o in outs if o.track == 1]
    assert sum(1 for o in a if o.flags & lib.DR_F_TAIL) == 10
    assert all(o.flags & lib.DR_F_TAIL for o in a[n - 10:])


def test_lpcm_payloads_carry_their_header_and_go_through_the_pcm_strategy():
    # DVD-Video LPCM, 16-bit stereo 48 kHz: each payload = 3-byte header +
    # 6 frames of 320 bytes (1/100 s); output frames of 1/30 s (1600 samples)
    pay = [Payload(1, bytes([k % 32, 0x01, 0x80]) + bytes(6 * 320), BASE + k * 10 * MS, False, BASE + k * 10 * MS)
           for k in range(4000)]
    outs, events, _, _ = run(tracks("pcm_dvd"), 1080000, False, [interleave(video(BASE), pay)])
    a = [o for o in outs if o.track == 1]
    assert a
    assert a[0].time == 0
    assert all(o.dur == 36000000 for o in a), "1600 samples at 48 kHz"
    # the video ends at 40.04 s: the PCM track ends with it
    end = a[-1].time + a[-1].dur
    assert end <= 1200 * FRAME + 36000000, end
    assert all(e["kind"] != lib.DR_EV_GAP for e in events)


def test_untested_features_are_counted_for_the_caller():
    # E-AC-3 with dependent frames (extra channels) is implemented but not met
    # on a real disc: every such unit is a review, which the demuxer reports
    # so that the job is marked failed
    def eac3(strmtyp, tag):
        f = bytearray([tag]) * 768
        f[0:2] = b"\x0b\x77"
        f[2:4] = ((strmtyp << 14) | (768 // 2 - 1)).to_bytes(2, "big")
        f[4] = (3 << 4) | (2 << 1)
        f[5] = 16 << 3
        f[6] = 0
        f[7] = 0
        return bytes(f)
    n = 1200 * 36036 // 34560
    pay = [Payload(1, eac3(0, k % 200) + eac3(1, 0x55), BASE + k * AC3_DUR, False, BASE + k * AC3_DUR)
           for k in range(n)]
    outs, _, _, review = run(tracks("eac3"), 110160000, True, [interleave(video(BASE), pay)])
    assert sum(1 for o in outs if o.track == 1) == n
    assert review > 0
