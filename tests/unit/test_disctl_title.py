"""One title through every stage of the title timeline (disctl_title.c): payloads
of an MPEG-2 video track and an AC-3 (or LPCM) track in one or two segments
in, joined, junctioned and checked frames out. Audio frames leave only once
the video has been handed on past their time."""

import dataclasses

from helpers import DATA, DIST, ffi, lib
from helpers.disctl import Handles, codec_id, events_into, pictures, spu
from test_helper import EDM, EOC, load, parity

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
FRAME = 36036000  # 1001/30000 s
AC3_DUR = 34560000  # 1536 samples at 48 kHz
STEP = 12288000  # one sub-picture delay step: 1024 / 90000 s
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
    data: bytes = b""


def video(base, stream=M2V):
    """The video payloads of one segment: every picture of the test stream
    with its PES time on the grid from base."""
    return [Payload(0, stream[a:b], base + disp * FRAME, False, base + (k - 1) * FRAME)
            for k, (a, b, disp, _) in enumerate(pictures(stream))]


def ac3(start, step, n):
    """AC-3 payloads: n frames (the test stream's frames over and over), one
    per payload, the first at start, each step after the one before."""
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    return [Payload(1, frames[k % len(frames)], start + k * step, False, start + k * step) for k in range(n)]


def interleave(a, b):
    """Payloads interleaved by decode time (the way they lie in a program
    stream); each track keeps its own order."""
    return sorted(a + b, key=lambda p: p.order)


def run(tracks, tolerance, lpcm_hd, segments, spu=None, cc_helper=None):
    """(outputs, events, duration, review); spu: a list that gets track 1's
    sub-picture result (no stop time, outside the canvas)."""
    events = []
    outs = []
    vmax = [-(1 << 63)]
    keep = Handles()
    tr = ffi.new("DRTitleTrack[]", tracks)
    helper = ffi.new("char[]", cc_helper.encode()) if cc_helper else ffi.NULL
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": len(tracks), "tracks": tr, "tolerance": tolerance,
                                      "lpcm_hd": int(lpcm_hd), "event": lib.tb_py_event,
                                      "event_opaque": keep(events_into(events)), "cc_helper": helper})
    t = ffi.new("DRTitle **")

    def drain():
        while True:
            f = ffi.new("DRFrame *")
            k = ffi.new("int *")
            r = lib.ff_disctl_title_frame(t[0], k, f)
            if r != 0:
                return r
            outs.append(Out(k[0], f.time, f.dur, f.flags, vmax[0],
                            bytes(ffi.buffer(f.data, f.size)) if k[0] and f.size else b""))
            if k[0] == 0:
                vmax[0] = max(vmax[0], f.time)
            lib.ff_disctl_frame_unref(f)
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    for seg in segments:
        assert lib.ff_disctl_title_segment(t[0]) == 0
        for p in seg:
            assert lib.ff_disctl_title_payload(t[0], p.track, p.data, len(p.data), p.time, int(p.tail)) == 0
            assert drain() == EAGAIN, "EAGAIN while the title goes on"
    assert lib.ff_disctl_title_finish(t[0]) == 0
    assert drain() == EOF_, "AVERROR_EOF after the last frame"
    duration, review = lib.ff_disctl_title_duration(t[0]), lib.ff_disctl_title_review(t[0])
    if spu is not None:
        n, o = ffi.new("int64_t *"), ffi.new("int64_t *")
        assert lib.ff_disctl_title_spu_result(t[0], 1, n, o) == 0
        assert lib.ff_disctl_title_spu_result(t[0], 0, n, o) < 0, "not a sub-picture track"
        spu += [n[0], o[0]]
    if cc_helper:
        b, c = ffi.new("int64_t *"), ffi.new("int64_t *")
        assert lib.ff_disctl_title_cc_result(t[0], len(tracks) - 1, b, c) == 0
        events.append({"kind": "cc_result", "blocks": b[0], "cues": c[0]})
    lib.ff_disctl_title_close(t)
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


def subs(units):
    """Sub-picture payloads of track 1: (unit, display position in frames)."""
    return [Payload(1, u, BASE + n * FRAME, False, BASE + n * FRAME) for u, n in units]


def sub_tracks():
    return [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0], [codec_id("dvd_subtitle"), lib.DR_KIND_SUBTITLE, 0]]


def test_a_sub_picture_without_a_stop_time_lasts_until_the_next_one():
    # stop times are kept; a unit that shows a picture without one stays until
    # the next unit starts, across segments too; the title's last one until
    # the video's end (untested on real discs: a review)
    s1 = subs([(spu(100, True, [], 1), 10), (spu(None, True, [], 2), 20), (spu(None, True, [], 3), 30),
               (spu(50, True, [], 4), 40), (spu(None, True, [], 5), 1190)])
    base2 = 500 * 1080000000
    s2 = [Payload(1, spu(None, True, [], 6), base2 + 5 * FRAME, False, base2 + 5 * FRAME)]
    res = []
    outs, events, duration, review = run(sub_tracks(), 110160000, True,
                                         [interleave(video(BASE), s1), interleave(video(base2), s2)], res)
    s = [(o.time, o.dur) for o in outs if o.track == 1]
    assert s == [
        (10 * FRAME, 100 * STEP),
        (20 * FRAME, 10 * FRAME),
        (30 * FRAME, 10 * FRAME),
        (40 * FRAME, 50 * STEP),
        (1190 * FRAME, 15 * FRAME),                  # until the next one, in segment 2 (at 1205)
        (1205 * FRAME, duration - 1205 * FRAME),     # the last: until the video's end
    ]
    assert duration == 2400 * FRAME
    assert review == 1
    assert res == [4, 0], "four units without a stop time, none outside (no canvas given)"


def test_a_sub_picture_followed_by_one_at_the_same_time_keeps_its_step():
    outs, _, _, review = run(sub_tracks(), 110160000, True,
                             [interleave(video(BASE), subs([(spu(None, True, [], 1), 10), (spu(20, True, [], 2), 10)]))])
    assert [(o.time, o.dur) for o in outs if o.track == 1] == [(10 * FRAME, STEP), (10 * FRAME, 20 * STEP)]
    assert review == 0


def cc_block(pairs):
    """A DVD caption block (GOP user data): odd field first, each field-1
    byte pair followed by a field-2 null pair."""
    head = bytes([0, 0, 1, 0xB2, 0x43, 0x43, 0x01, 0xF8, 0x80 | 2 * len(pairs)])
    return head + b"".join(bytes([0xFF, parity(a), parity(b), 0xFE, 0x80, 0x80]) for a, b in pairs)


def with_captions(stream, per_gop):
    """The stream with a caption block after each GOP header: per_gop[g]
    (default one null pair) for GOP g. Returns it and each GOP's first
    displayed picture (its display index)."""
    out, g, i = bytearray(), 0, 0
    while True:
        j = stream.find(b"\x00\x00\x01\xb8", i)
        if j < 0:
            out += stream[i:]
            break
        out += stream[i:j + 8] + cc_block(per_gop.get(g, [(0x00, 0x00)]))
        i = j + 8
        g += 1
    s = bytes(out)
    gops = [p for p in range(len(s)) if s.startswith(b"\x00\x00\x01\xb8", p)]
    first = []
    for k, start in enumerate(gops):
        end = gops[k + 1] if k + 1 < len(gops) else len(s)
        first.append(min(d for a, b, d, _ in pictures(s) if start <= a < end or (a < start < b)))
    return s, first


def test_captions_in_the_gop_user_data_become_a_text_track():
    # GOP 2 loads a caption, GOP 3 shows it, GOP 10 erases it; GOP 90 loads
    # one more, GOP 91 shows it and nothing erases it: it lasts until the
    # end of the video. Each block is timed on its GOP's first picture.
    per_gop = {2: load(b"HELLO"), 3: EOC, 10: EDM, 90: load(b"LAST"), 91: EOC}
    stream, first = with_captions(M2V, per_gop)
    tracks = [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0], [codec_id("subrip"), lib.DR_KIND_CAPTION, 0]]
    outs, events, duration, review = run(tracks, 110160000, True, [video(BASE, stream)],
                                         cc_helper=str(DIST / "ccextractor"))
    ms = lambda d: d * FRAME // MS                      # noqa: E731  (a display index in ms, truncated)
    cues = [(o.time // MS, (o.time + o.dur) // MS, o.data.decode()) for o in outs if o.track == 1]
    assert cues == [
        (ms(first[3]) + 1, ms(first[10]) - 1, "HELLO" + " " * 27),
        (ms(first[91]) + 1, duration // MS - 1, "LAST" + " " * 28),
    ]
    assert len([o for o in outs if o.track == 0]) == 1200, "the video keeps every picture"
    res = [e for e in events if e.get("kind") == "cc_result"][0]
    assert (res["blocks"], res["cues"]) == (len(first), 2)
    assert review == 0


def dropped(stream):
    """(video frames out, caption blocks left out, their entries with caption
    bytes) of a title with only a video track."""
    tr = ffi.new("DRTitleTrack[]", [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0]])
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": 1, "tracks": tr, "tolerance": 110160000, "lpcm_hd": 1})
    t, f, k = ffi.new("DRTitle **"), ffi.new("DRFrame *"), ffi.new("int *")
    b, x = ffi.new("int64_t *"), ffi.new("int64_t *")
    n = 0
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    assert lib.ff_disctl_title_segment(t[0]) == 0
    for p in video(BASE, stream):
        assert lib.ff_disctl_title_payload(t[0], 0, p.data, len(p.data), p.time, 0) == 0
    assert lib.ff_disctl_title_finish(t[0]) == 0
    while lib.ff_disctl_title_frame(t[0], k, f) == 0:
        n += 1
        lib.ff_disctl_frame_unref(f)
    lib.ff_disctl_title_cc_dropped(t[0], b, x)
    lib.ff_disctl_title_close(t)
    return n, b[0], x[0]


def test_caption_blocks_without_a_caption_track_are_counted_and_left_out():
    # caption text: counted (load: 6 control + 3 text pairs, EOC 2, EDM 2); the null code only
    # (the HD DVD corpus): blocks counted, no text; the video keeps every picture
    stream, first = with_captions(M2V, {2: load(b"HELLO"), 3: EOC, 4: EDM})
    assert dropped(stream) == (1200, len(first), 6 + 3 + 2 + 2)
    stream, first = with_captions(M2V, {})
    assert dropped(stream) == (1200, len(first), 0)


def test_a_failed_title_stays_failed():
    # the caption converter cannot start: the payload that brings the first
    # caption block fails, and every later call returns the same error (a
    # frame step left with half a payload must not go on)
    stream, _ = with_captions(M2V, {2: load(b"HELLO"), 3: EOC})
    tr = ffi.new("DRTitleTrack[]", [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0],
                                    [codec_id("subrip"), lib.DR_KIND_CAPTION, 0]])
    helper = ffi.new("char[]", b"/nonexistent/ccextractor")
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": 2, "tracks": tr, "tolerance": 110160000, "lpcm_hd": 1,
                                      "cc_helper": helper})
    t = ffi.new("DRTitle **")
    f, k = ffi.new("DRFrame *"), ffi.new("int *")
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    assert lib.ff_disctl_title_segment(t[0]) == 0
    err = 0
    for p in video(BASE, stream):
        err = lib.ff_disctl_title_payload(t[0], 0, p.data, len(p.data), p.time, 0)
        if err:
            break
        while lib.ff_disctl_title_frame(t[0], k, f) == 0:
            lib.ff_disctl_frame_unref(f)
    assert err < 0
    p = video(BASE, stream)[-1]
    assert lib.ff_disctl_title_payload(t[0], 0, p.data, len(p.data), p.time, 0) == err
    assert lib.ff_disctl_title_segment(t[0]) == err
    assert lib.ff_disctl_title_finish(t[0]) == err
    assert lib.ff_disctl_title_frame(t[0], k, f) == err
    lib.ff_disctl_title_close(t)


# ---- a second layer (the dependent view of a 3-D video) ----

LAYER = 2


def mvc_picture(k, extra=b""):
    """A synthetic dependent-view picture: MVC delimiter (24), a filler data
    unit (12, taken out as from the master video), a slice extension (20)."""
    return (b"\0\0\0\1\x18\x80" + b"\0\0\1\x0c" + b"\xff" * 8 + b"\x80" +
            b"\0\0\1\x14" + bytes([0x80 | (k & 0x3f), k >> 6 & 0xff, 0x11]) + extra)


def mvc_clean(k, extra=b""):
    return b"\0\0\0\1\x18\x80" + b"\0\0\1\x14" + bytes([0x80 | (k & 0x3f), k >> 6 & 0xff, 0x11]) + extra


def layer(base, skip=(), extra=(), split=False, stream=M2V):
    """The layer's payloads: one picture per master picture with the same
    PES time (as on a disc), in the same (decoding) order; skip: master
    pictures without one; extra: (after picture k, time) pictures with no
    master frame; split: each picture in two payloads, the second without a
    time (as TS packets bring it)."""
    out = []
    for k, (a, b, disp, _) in enumerate(pictures(stream)):
        order = base + (k - 1) * FRAME
        if k not in skip:
            pic = mvc_picture(k)
            if split:
                out += [Payload(LAYER, pic[:7], base + disp * FRAME, False, order),
                        Payload(LAYER, pic[7:], AV_NOPTS, False, order)]
            else:
                out.append(Payload(LAYER, pic, base + disp * FRAME, False, order))
        for after, t in extra:
            if after == k:
                out.append(Payload(LAYER, mvc_picture(900 + k), t, False, order))
    return out


AV_NOPTS = -(1 << 63)


def layer_tracks():
    return [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0], [codec_id("ac3"), lib.DR_KIND_AUDIO, 0],
            [codec_id("h264"), lib.DR_KIND_LAYER, 0]]


def by_track(outs, k):
    return [o for o in outs if o.track == k]


def test_a_second_layer_picture_leaves_with_its_video_frame():
    v = video(BASE)
    seg = interleave(v + ac3(BASE, AC3_DUR, 10), layer(BASE, split=True))
    outs, _, _, _ = run(layer_tracks(), MS, False, [seg])
    vid, lay = by_track(outs, 0), by_track(outs, LAYER)
    assert len(lay) == len(vid) == len(v), "one picture per video frame"
    assert [(o.time, o.dur, o.flags & lib.DR_F_KEY) for o in lay] == \
           [(o.time, o.dur, o.flags & lib.DR_F_KEY) for o in vid], "the video frame's time, duration and key flag"
    assert [o.data for o in lay] == [mvc_clean(k) for k in range(len(v))], \
        "pictures cut at the MVC delimiters, in decoding order, the filler unit taken out"
    for i, o in enumerate(outs):
        if o.track == LAYER:
            assert any(p.track == 0 and p.time == o.time for p in outs[:i]), "after its video frame"


def test_a_second_layer_picture_without_a_video_frame_is_left_out():
    v = video(BASE)
    seg = interleave(v, layer(BASE, extra=[(3, BASE + 5000 * FRAME)]))
    outs, _, _, _ = run(layer_tracks()[:1] + [layer_tracks()[2]], MS, False,
                        [[p if p.track != LAYER else dataclasses.replace(p, track=1) for p in seg]])
    assert [o.data for o in by_track(outs, 1)] == [mvc_clean(k) for k in range(len(v))], \
        "every video frame's picture, the stray one left out"


def test_a_video_frame_without_its_second_layer_picture_fails_the_title():
    tracks = [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0], [codec_id("h264"), lib.DR_KIND_LAYER, 0]]
    v = video(BASE)
    seg = interleave(v, [dataclasses.replace(p, track=1) for p in layer(BASE, skip={5})])
    keep = Handles()
    tr = ffi.new("DRTitleTrack[]", tracks)
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": 2, "tracks": tr, "tolerance": MS, "event": lib.tb_py_event,
                                      "event_opaque": keep(events_into([]))})
    t = ffi.new("DRTitle **")
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    assert lib.ff_disctl_title_segment(t[0]) == 0
    for p in seg:
        lib.ff_disctl_title_payload(t[0], p.track, p.data, len(p.data), p.time, 0)
    assert lib.ff_disctl_title_finish(t[0]) < 0, "the layer's stream would miss a picture"
    lib.ff_disctl_title_close(t)


def test_a_second_layer_follows_the_video_into_the_next_segment():
    v1, v2 = video(BASE), video(BASE)
    s1 = interleave(v1, [dataclasses.replace(p, track=1) for p in layer(BASE)])
    s2 = interleave(v2, [dataclasses.replace(p, track=1) for p in layer(BASE)])
    tracks = [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0], [codec_id("h264"), lib.DR_KIND_LAYER, 0]]
    outs, _, _, _ = run(tracks, MS, False, [s1, s2])
    vid, lay = by_track(outs, 0), by_track(outs, 1)
    assert len(lay) == len(vid) == 2 * len(v1)
    assert [o.time for o in lay] == [o.time for o in vid], "the second segment's pictures on its place"


def test_the_h264_clean_up_leaves_a_unit_with_nothing_to_take_as_it_is():
    dropped = ffi.new("int64_t[32]")
    stuffing = ffi.new("int64_t *")
    changed = ffi.new("int *")
    unit = mvc_clean(1)
    out = ffi.new("uint8_t[]", len(unit) + 16)
    n = lib.ff_disctl_h264_clean(unit, len(unit), out, dropped, stuffing, changed)
    assert (n, changed[0]) == (len(unit), 0)
    # zero stuffing in front of a kept unit is cut to one byte; in front of
    # a unit taken out it goes with that unit
    unit = b"\0\0\0\0" + mvc_picture(1) + b"\0\0\0" + b"\0\0\1\x0b"
    n = lib.ff_disctl_h264_clean(unit, len(unit), out, dropped, stuffing, changed)
    assert bytes(ffi.buffer(out, n)) == mvc_clean(1) and changed[0] == 1
    assert (dropped[12], dropped[11], stuffing[0]) == (1, 1, 4)


def test_a_chapter_position_starts_the_chapter_at_the_first_picture_from_there():
    # DVD-Video chapters are positions: the mark of each open chapter is
    # the time of the first video frame from the bytes given when its
    # position was reached; the key-frame rule then places it
    pics = pictures(M2V)
    gops = [k for k, (a, b, _, _) in enumerate(pics) if b"\x00\x00\x01\xb8" in M2V[a:b]]
    at = [gops[2], gops[5]]
    marks = ffi.new("int64_t[]", [(1 << 63) - 1, (1 << 63) - 1])
    tr = ffi.new("DRTitleTrack[]", [[codec_id("mpeg2video"), lib.DR_KIND_VIDEO, 0]])
    cfg = ffi.new("DRTitleConfig *", {"nb_tracks": 1, "tracks": tr, "tolerance": MS, "marks": marks, "nb_marks": 2,
                                      "open_marks": marks})
    t = ffi.new("DRTitle **")
    assert lib.ff_disctl_title_open(t, ffi.NULL, cfg) == 0
    assert lib.ff_disctl_title_segment(t[0]) == 0
    outs = []

    def drain():
        while True:
            f, k = ffi.new("DRFrame *"), ffi.new("int *")
            if lib.ff_disctl_title_frame(t[0], k, f):
                return
            outs.append((f.time, f.flags))
            lib.ff_disctl_frame_unref(f)
    for k, p in enumerate(video(BASE)):
        if k in at:
            assert lib.ff_disctl_title_chapter_position(t[0]) == 0
        assert lib.ff_disctl_title_payload(t[0], 0, p.data, len(p.data), p.time, 0) == 0
        drain()
    assert lib.ff_disctl_title_finish(t[0]) == 0
    drain()
    lib.ff_disctl_title_close(t)
    marked = [time for time, flags in outs if flags & lib.DR_F_CHAPTER]
    # the I pictures of those GOPs, on the title timeline (the first video frame at 0)
    first = min(BASE + disp * FRAME for _, _, disp, _ in pics)
    assert marked == [BASE + pics[k][2] * FRAME - first for k in at]
    assert list(marks) == marked
