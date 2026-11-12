"""The cut stage of the title timeline (src/timeline/disctl_cut.c): of a
title's frames it keeps one cut part's, on the part's clock. The cuts come
from the same pictures through the cut rule (src/disc-remuxer/cuts.c), so
the two are checked against each other: every picture lands in exactly one
part (but the leading pictures left out at an open key frame), each part
starts at 0, and its pictures are the title's from its key frame on."""

from helpers import AV_NOPTS_VALUE, ffi, install_log, lib, logged

F = 45_045_000                      # one picture at 23.976 (ticks)
CLEAN, BROKEN, OPEN = lib.DR_START_CLEAN, lib.DR_START_BROKEN, lib.DR_START_OPEN
OTHER, H264, HEVC = lib.DR_CUT_OTHER, lib.DR_CUT_H264, lib.DR_CUT_HEVC
START_FLAG = {CLEAN: lib.DR_F_START_CLEAN, BROKEN: lib.DR_F_START_BROKEN, OPEN: lib.DR_F_START_OPEN}


def picture_list(specs):
    """(time, key, start, rasl, ref) in decoding order: (n, start) key frames, ('r', n) an HEVC RASL
    picture, ('R', n) an H.264 reference picture, n others."""
    out = []
    for s in specs:
        if isinstance(s, tuple) and s[0] in ('r', 'R'):
            out.append((s[1] * F, 0, 0, s[0] == 'r', s[0] == 'R'))
        elif isinstance(s, tuple):
            out.append((s[0] * F, 1, s[1], 0, 1))
        else:
            out.append((s * F, 0, 0, 0, 0))
    return out


def places(pics, codec):
    a = ffi.new("CutPicture[]", len(pics))
    for i, (t, key, start, rasl, ref) in enumerate(pics):
        a[i].time, a[i].duration, a[i].key, a[i].start, a[i].rasl, a[i].ref = t, F, key, start, rasl, ref
    p, n = ffi.new("CutPoint **"), ffi.new("int *")
    assert lib.cut_points(a, len(pics), codec, p, n) == 0
    out = [(p[0][i].zero, p[0][i].key_time) for i in range(n[0])]
    lib.free(p[0])
    return out


def part(pics, cuts, k, codec=H264, others=()):
    """Part k of the title cut at cuts [(zero, key_time)]: the pictures given
    (title time, part time), finish's return, the result."""
    pl = ffi.new("DRCutPlace[]", len(cuts))
    for i, (z, kt) in enumerate(cuts):
        pl[i].zero, pl[i].key_time = z, kt
    c = ffi.new("DRCut **")
    kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO, lib.DR_CUT_TRACK_NONE])
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, len(cuts), k, codec, kinds, 2, ffi.NULL) == 0
    given = []
    for i, (t, key, start, rasl, ref) in enumerate(pics):
        f = ffi.new("DRFrame *")
        f.time = t
        f.flags = ((lib.DR_F_KEY | START_FLAG.get(start, 0) if key else 0) | (lib.DR_F_RASL if rasl else 0) |
                   (lib.DR_F_REF if ref else 0))
        if lib.ff_disctl_cut_frame(c[0], 0, f):
            given.append((t, f.time))
        if i in others:
            o = ffi.new("DRFrame *")
            o.time = t
            assert lib.ff_disctl_cut_frame(c[0], 1, o) == 0, "a track a cut part does not carry: left out"
    ret = lib.ff_disctl_cut_finish(c[0])
    r = ffi.new("DRCutResult *")
    lib.ff_disctl_cut_result(c[0], r)
    lib.ff_disctl_cut_close(c)
    return given, ret, (r.pictures, r.left_out, r.first, r.start, r.undecodable)


# I P B B | I B B P B B | I B B P B B (the 2nd / 3rd key frames' B pictures are leading pictures)
def stream(second, third):
    return picture_list([(0, CLEAN), 3, 1, 2, (6, second), 4, 5, 9, 7, 8, (12, third), 10, 11, 15, 13, 14])


def parts_of(pics, codec=H264):
    cuts = places(pics, codec)
    return cuts, [part(pics, cuts, k, codec) for k in range(len(cuts) + 1)]


def test_each_part_starts_at_0_and_every_picture_lands_once():
    for codec in (H264, OTHER):
        for second, third in [(CLEAN, CLEAN), (OPEN, CLEAN), (CLEAN, OPEN), (OPEN, BROKEN)]:
            pics = stream(second, third)
            cuts, parts = parts_of(pics, codec)
            assert len(parts) == 3
            seen = []
            for k, (given, ret, (n, left, first, start, und)) in enumerate(parts):
                assert ret == 0
                assert first == 0 and min(pt for _, pt in given) == 0, "each part starts at 0"
                zero = cuts[k - 1][0] if k else 0
                assert all(pt == t - zero for t, pt in given), "on the part's clock: time - its zero"
                assert n == len(given)
                seen += [t for t, _ in given]
            # the leading pictures of an open key frame are left out (H.264's non-reference ones, MPEG-2 / VC-1's B)
            left_out = ({4 * F, 5 * F} if second != CLEAN else set()) | ({10 * F, 11 * F} if third != CLEAN else set())
            assert sorted(seen) == sorted(p[0] for p in pics if p[0] not in left_out), (codec, second, third)
            assert len(seen) == len(set(seen)), "no picture in two parts"


def test_a_clean_key_frame_part_starts_with_its_leading_pictures():
    cuts, parts = parts_of(stream(CLEAN, CLEAN))
    given, _, (n, left, first, start, und) = parts[1]
    assert cuts[0] == (4 * F, 6 * F)
    assert [t // F for t, _ in given] == [6, 4, 5, 9, 7, 8], "decoding order, from its key frame"
    assert (left, und, start) == (0, 0, CLEAN)


def test_an_open_key_frame_part_leaves_out_only_what_the_codec_allows():
    # H.264, non-reference leading pictures: left out
    cuts, parts = parts_of(stream(OPEN, CLEAN), H264)
    given, _, (n, left, first, start, und) = parts[1]
    assert cuts[0] == (6 * F, 6 * F)
    assert [t // F for t, _ in given] == [6, 9, 7, 8]
    assert (left, und, start) == (2, 0, OPEN)
    # the part before ends at the key frame in decoding order: the left-out pictures are in neither part
    assert [t // F for t, _ in parts[0][0]] == [0, 3, 1, 2]
    # H.264 with a reference leading picture (a B pyramid): it stays, the other goes
    pics = picture_list([(0, CLEAN), 3, 1, 2, (6, OPEN), ('R', 4), 5, 9, 7, 8])
    cuts, parts = parts_of(pics, H264)
    given, ret, (n, left, first, start, und) = parts[1]
    assert ret == 0 and cuts[0] == (4 * F, 6 * F)
    assert [t // F for t, _ in given] == [6, 4, 9, 7, 8] and (left, und, first) == (1, 1, 0)
    # MPEG-2 / VC-1: their B pictures left out, the part starts on its key frame
    cuts, parts = parts_of(stream(OPEN, CLEAN), OTHER)
    given, _, (n, left, first, start, und) = parts[1]
    assert cuts[0] == (6 * F, 6 * F) and [t // F for t, _ in given] == [6, 9, 7, 8] and (left, und) == (2, 0)


def test_hevc_keeps_radl_and_leaves_rasl_out():
    pics = picture_list([(0, CLEAN), 3, 1, 2, (6, OPEN), ('r', 4), 5, 9, 7, 8])
    cuts, parts = parts_of(pics, HEVC)
    assert cuts == [(5 * F, 6 * F)]
    given, ret, (n, left, first, start, und) = parts[1]
    assert ret == 0 and [t // F for t, _ in given] == [6, 5, 9, 7, 8] and (left, und, first) == (1, 0, 0)


def test_a_key_frame_not_where_the_cuts_say_fails_the_part():
    pics = stream(CLEAN, CLEAN)
    # a cut from another reading: no key frame at 7
    assert part(pics, [(7 * F, 7 * F)], 1)[1] < 0, "its own key frame not met"
    assert part(pics, [(7 * F, 7 * F)], 0)[1] < 0, "the next part's key frame not met"
    # a place whose zero is not its first picture
    assert part(pics, [(5 * F, 6 * F)], 1)[1] < 0, "the first picture is not at 0"


def test_a_track_a_cut_part_does_not_carry_is_left_out():
    pics = stream(CLEAN, CLEAN)
    given, ret, _ = part(pics, places(pics, H264), 1, others={0, 5, 9})
    assert ret == 0 and given


def test_the_leading_picture_rule():
    for codec in (OTHER, H264, HEVC):
        assert lib.ff_disctl_leading_decodes(CLEAN, codec, 0) == 1
        assert lib.ff_disctl_leading_kept(CLEAN, codec, 0, 0) == 1
    assert lib.ff_disctl_leading_decodes(OPEN, H264, 0) == 0
    assert lib.ff_disctl_leading_decodes(BROKEN, OTHER, 0) == 0
    assert lib.ff_disctl_leading_decodes(OPEN, HEVC, 0) == 1, "HEVC RADL"
    assert lib.ff_disctl_leading_decodes(OPEN, HEVC, 1) == 0, "HEVC RASL"
    assert lib.ff_disctl_leading_kept(OPEN, HEVC, 1, 0) == 0, "a RASL picture may be left out"
    assert lib.ff_disctl_leading_kept(OPEN, H264, 0, 0) == 0, "an H.264 non-reference picture may be left out"
    assert lib.ff_disctl_leading_kept(OPEN, H264, 0, 1) == 1, "an H.264 reference picture stays"
    assert lib.ff_disctl_leading_kept(OPEN, OTHER, 0, 0) == 0, "an MPEG-2 / VC-1 B picture is left out"
    assert lib.ff_disctl_leading_kept(BROKEN, OTHER, 0, 0) == 0, "after a broken link too"


# ---- audio ----

A = 34_560_000                      # an AC-3 frame: 1536 samples at 48 kHz = 32 ms
SAMPLE = 22_500                     # one sample at 48 kHz


def run_audio(cuts, k, frames, kind, rate=48000, bpf=4, starts=None):
    """Part k of audio frames [(time, dur, bytes)] on track 1 (the video's
    cuts given; no video frames): [(part time, dur, bytes)]."""
    pl = ffi.new("DRCutPlace[]", len(cuts))
    for i, (z, kt) in enumerate(cuts):
        pl[i].zero, pl[i].key_time = z, kt
    c = ffi.new("DRCut **")
    kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO, kind])
    table = ffi.new("int64_t[]", [v for s in (starts or []) for v in (0, s)]) if starts else ffi.NULL
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, len(cuts), k, H264, kinds, 2, table) == 0
    lib.ff_disctl_cut_pcm(c[0], 1, rate, bpf)
    if starts is not None:
        # the video's key frames at the title's start and at the cut, so the part's checks can run
        for t in (0, cuts[0][1]):
            v = ffi.new("DRFrame *")
            v.time, v.flags = t, lib.DR_F_KEY | lib.DR_F_START_CLEAN
            lib.ff_disctl_cut_frame(c[0], 0, v)
    out, keep = [], []
    for t, dur, data in frames:
        buf = ffi.new("uint8_t[]", data)
        keep.append(buf)
        f = ffi.new("DRFrame *")
        f.time, f.dur, f.data, f.size = t, dur, buf, len(data)
        if lib.ff_disctl_cut_frame(c[0], 1, f):
            out.append((f.time, f.dur, bytes(ffi.buffer(f.data, f.size))))
    n, first = ffi.new("int64_t *"), ffi.new("int64_t *")
    lib.ff_disctl_cut_track(c[0], 1, n, first)
    assert n[0] == len(out)
    if out:
        assert first[0] == out[0][0]
    if starts is not None:
        run_audio.finish = lib.ff_disctl_cut_finish(c[0])
    lib.ff_disctl_cut_close(c)
    return out


def test_audio_frames_go_by_their_time_and_a_part_starts_0_to_one_frame_after_its_0():
    frames = [(i * A, A, bytes([i % 256]) * 8) for i in range(100)]
    zero = 40 * F                       # 1668.3 ms: inside AC-3 frame 52 (1664 .. 1696 ms)
    parts = [run_audio([(zero, zero)], k, frames, lib.DR_CUT_TRACK_EVERY) for k in (0, 1)]
    first_after = next(i for i in range(100) if i * A >= zero)
    assert [p[2] for p in parts[0]] == [f[2] for f in frames[:first_after]], "frames before the 0: the part before"
    assert [p[2] for p in parts[1]] == [f[2] for f in frames[first_after:]], "nothing lost, nothing twice"
    delay = parts[1][0][0]
    assert 0 <= delay < A and delay == first_after * A - zero, "positive, under one frame, exact"
    assert parts[0][-1][0] < zero, "the part before may run on past its 0 by less than a frame"


def pcm_frames(n, samples, bpf=4):
    """n PCM frames of `samples` samples each, every sample frame numbered (bytes say which)."""
    out = []
    for i in range(n):
        data = b"".join((i * samples + s).to_bytes(bpf, "little") for s in range(samples))
        out.append((i * samples * SAMPLE, samples * SAMPLE, data))
    return out


def test_pcm_is_cut_between_two_samples_at_the_first_one_at_or_after_the_0():
    frames = pcm_frames(40, 1600)       # 1/30 s frames at 48 kHz, stereo 16-bit
    whole = b"".join(f[2] for f in frames)
    # at 23.976 a picture is 2002 samples: a 0 on a picture is on a sample
    zero = 15 * F                       # 15 pictures = 30030 samples
    p0, p1 = (run_audio([(zero, zero)], k, frames, lib.DR_CUT_TRACK_PCM) for k in (0, 1))
    assert b"".join(p[2] for p in p0) + b"".join(p[2] for p in p1) == whole, "every sample once"
    assert len(b"".join(p[2] for p in p0)) == 30030 * 4
    assert p1[0][0] == 0, "the part's audio starts exactly at its 0"
    assert int.from_bytes(p1[0][2][:4], "little") == 30030
    assert p0[-1][0] + p0[-1][1] == zero, "the part before ends at the 0"


def test_pcm_at_a_0_between_samples_starts_less_than_a_sample_after_it():
    frames = pcm_frames(40, 1600)
    zero = 15 * 36_036_000              # 15 pictures at 29.97 (1001/30000 s): 24024 samples exactly
    zero += 7_000                       # moved off the sample grid by 7000 ticks
    p0, p1 = (run_audio([(zero, zero)], k, frames, lib.DR_CUT_TRACK_PCM) for k in (0, 1))
    assert b"".join(p[2] for p in p0) + b"".join(p[2] for p in p1) == b"".join(f[2] for f in frames)
    assert 0 < p1[0][0] < SAMPLE, "under one sample, never before the 0"
    assert int.from_bytes(p1[0][2][:4], "little") == 24025


def test_pcm_without_its_layout_is_an_error():
    frames = pcm_frames(2, 1600)
    pl = ffi.new("DRCutPlace[]", 1)
    pl[0].zero = pl[0].key_time = F
    c = ffi.new("DRCut **")
    kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO, lib.DR_CUT_TRACK_PCM])
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, 1, 1, H264, kinds, 2, ffi.NULL) == 0
    buf = ffi.new("uint8_t[]", frames[0][2])
    f = ffi.new("DRFrame *")
    f.time, f.dur, f.data, f.size = 0, frames[0][1], buf, len(frames[0][2])
    assert lib.ff_disctl_cut_frame(c[0], 1, f) < 0
    lib.ff_disctl_cut_close(c)


# ---- audio that starts only at some units (TrueHD, a DTS-HD stream) ----

U = 900_000                         # a TrueHD unit: 1/1200 s


def units(n, every=128, first_entry=0):
    """n units of 1/1200 s; every `every`-th one (from first_entry) an entry unit."""
    u = ffi.new("CutUnit[]", n)
    for i in range(n):
        u[i].time, u[i].entry = i * U, (i - first_entry) % every == 0 and i >= first_entry
    return u


def start(u, n, zero, limit=128 * U):
    s = ffi.new("CutStart *")
    assert lib.cut_track_start(u, n, zero, limit, s) == 0
    return s.time, s.entry, s.carried, s.to_entry


def test_a_start_unit_track_starts_at_its_first_entry_unit_within_the_limit():
    u = units(1000)
    # the cut's 0 between entry units 256 and 384: it starts at 384, units 300..383 stay in the part before
    assert start(u, 1000, 300 * U - 1000) == (384 * U, 1, 84, -1)
    assert start(u, 1000, 384 * U) == (384 * U, 1, 0, -1), "on an entry unit"
    # no entry unit within the limit: the first unit at or after the 0, and how far the next entry is
    u = units(1000, every=600)
    assert start(u, 1000, 10 * U) == (10 * U, 0, 0, 590)
    # the track ends before the cut
    assert start(u, 1000, 2000 * U)[0] == 2 ** 63 - 1


def test_a_start_unit_track_is_routed_by_the_start_table():
    frames = [(i * U, U, bytes([i % 256])) for i in range(600)]
    zero = 300 * U - 1000
    table = [384 * U]                   # the unit its part starts at (from cut_track_start())
    p0 = run_audio([(zero, zero)], 0, frames, lib.DR_CUT_TRACK_ENTRY, starts=table)
    assert run_audio.finish == 0
    p1 = run_audio([(zero, zero)], 1, frames, lib.DR_CUT_TRACK_ENTRY, starts=table)
    assert run_audio.finish == 0
    assert [p[2] for p in p0] + [p[2] for p in p1] == [f[2] for f in frames], "every unit in one part"
    assert len(p0) == 384 and p0[-1][0] > zero, "the units up to its start unit stay in the part before"
    assert p1[0][0] == 384 * U - zero, "its part starts at the start unit, on the part's clock"


def test_a_start_unit_not_where_the_table_says_fails_the_part():
    frames = [(i * U, U, b"x") for i in range(600)]
    zero = 300 * U
    run_audio([(zero, zero)], 1, frames, lib.DR_CUT_TRACK_ENTRY, starts=[384 * U + 5])
    assert run_audio.finish < 0, "no unit at that time"
    run_audio([(zero, zero)], 0, frames, lib.DR_CUT_TRACK_ENTRY, starts=[384 * U + 5])
    assert run_audio.finish < 0, "the next part's start unit not met"


def test_a_track_given_whole_gets_every_unit_that_starts_before_the_parts_end():
    # the caller decodes such a track from its first unit and cuts between two samples itself
    frames = [(i * U, U, bytes([i % 256])) for i in range(600)]
    a, b = 200 * U + 7, 400 * U + 7     # two cuts, both inside a unit
    cuts = [(a, a), (b, b)]
    p0, p1, p2 = (run_audio(cuts, k, frames, lib.DR_CUT_TRACK_WHOLE) for k in (0, 1, 2))
    assert [p[2] for p in p0] == [f[2] for f in frames[:201]], "part 1: up to the unit that holds the cut"
    assert [p[2] for p in p1] == [f[2] for f in frames[:401]], "part 2: from the title's first unit"
    assert [p[2] for p in p2] == [f[2] for f in frames], "the last part: every unit"
    assert p0[0][0] == 0 and p1[0][0] == -a and p2[0][0] == -b, "on the part's clock: below 0 before the part"
    assert p1[200][0] == 200 * U - a < 0 <= p1[201][0], "the unit that holds the cut starts before the part's 0"
    # a unit without a time is given too (the decoder needs every unit in order)
    odd = frames[:10] + [(AV_NOPTS_VALUE, U, b"n")] + frames[10:]
    assert len(run_audio(cuts, 1, odd, lib.DR_CUT_TRACK_WHOLE)) == 402


# ---- subtitles and captions ----

def run_subs(cuts, k, units, kind):
    """Part k of subtitle units [(time, dur, bytes)] on track 1 (with the video's key frames at 0 and at
    the cut): [(part time, bytes)], finish's return."""
    pl = ffi.new("DRCutPlace[]", len(cuts))
    for i, (z, kt) in enumerate(cuts):
        pl[i].zero, pl[i].key_time = z, kt
    c = ffi.new("DRCut **")
    kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO, kind])
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, len(cuts), k, H264, kinds, 2, ffi.NULL) == 0
    for t in (0, cuts[0][1]):
        v = ffi.new("DRFrame *")
        v.time, v.flags = t, lib.DR_F_KEY | lib.DR_F_START_CLEAN
        lib.ff_disctl_cut_frame(c[0], 0, v)
    out, keep = [], []
    for t, dur, data in units:
        buf = ffi.new("uint8_t[]", data)
        keep.append(buf)
        f = ffi.new("DRFrame *")
        f.time, f.dur, f.data, f.size = t, dur, buf, len(data)
        if lib.ff_disctl_cut_frame(c[0], 1, f):
            out.append((f.time, bytes(ffi.buffer(f.data, f.size))))
    ret = lib.ff_disctl_cut_finish(c[0])
    lib.ff_disctl_cut_close(c)
    return out, ret


S = 1_080_000_000                   # one second


def test_subtitles_go_by_their_start_time_on_the_part_clock():
    units = [(t * S, S, bytes([t])) for t in (1, 5, 9, 10, 12)]
    zero = 10 * S
    p0, _ = run_subs([(zero, zero)], 0, units, lib.DR_CUT_TRACK_SUB)
    p1, _ = run_subs([(zero, zero)], 1, units, lib.DR_CUT_TRACK_SUB)
    assert [b for _, b in p0] == [bytes([1]), bytes([5]), bytes([9])]
    assert p1 == [(0, bytes([10])), (2 * S, bytes([12]))], "from the 0 on, on the part's clock"


def test_a_subtitle_shown_across_the_cut_is_said_on_both_sides():
    units = [(9 * S, 2 * S, b"a"), (12 * S, S, b"b")]        # 9 s to 11 s: across a cut at 10 s
    zero = 10 * S
    install_log(lib.AV_LOG_WARNING)
    p0, _ = run_subs([(zero, zero)], 0, units, lib.DR_CUT_TRACK_SUB)
    assert [b for _, b in p0] == [b"a"] and logged("its last subtitle is still shown at the part's end")
    install_log(lib.AV_LOG_WARNING)
    p1, _ = run_subs([(zero, zero)], 1, units, lib.DR_CUT_TRACK_SUB)
    assert [b for _, b in p1] == [b"b"] and logged("a subtitle of the part before is still shown")
    install_log(lib.AV_LOG_WARNING)
    run_subs([(zero, zero)], 0, [(8 * S, S, b"a"), (12 * S, S, b"b")], lib.DR_CUT_TRACK_SUB)
    assert not logged("still shown"), "nothing across: nothing said"


def pgs(state, objects):
    """A display set: a composition segment (state, objects) and the end segment."""
    body = bytes([7, 128, 4, 56, 16, 0, 1, state, 0, 0, objects]) + bytes(8 * objects)
    return bytes([0x16]) + len(body).to_bytes(2, "big") + body + bytes([0x80, 0, 0])


def test_pgs_display_sets_say_what_is_shown_across_a_cut_and_a_part_that_does_not_start_afresh():
    zero = 10 * S
    # shown at 9 s, cleared at 11 s: across the cut
    sets = [(9 * S, 0, pgs(0x80, 1)), (11 * S, 0, pgs(0x00, 0)), (12 * S, 0, pgs(0x80, 1))]
    install_log(lib.AV_LOG_WARNING)
    p0, _ = run_subs([(zero, zero)], 0, sets, lib.DR_CUT_TRACK_PGS)
    assert len(p0) == 1 and logged("the display set that clears it is the next part's")
    install_log(lib.AV_LOG_WARNING)
    p1, _ = run_subs([(zero, zero)], 1, sets, lib.DR_CUT_TRACK_PGS)
    assert [t for t, _ in p1] == [S, 2 * S]
    assert logged("a subtitle of the part before is still shown") and logged("its first PGS display set is a normal one")
    # cleared before the cut, the part starts with an epoch start: nothing said
    sets = [(8 * S, 0, pgs(0x80, 1)), (9 * S, 0, pgs(0x00, 0)), (12 * S, 0, pgs(0x80, 1))]
    install_log(lib.AV_LOG_WARNING)
    run_subs([(zero, zero)], 0, sets, lib.DR_CUT_TRACK_PGS)
    run_subs([(zero, zero)], 1, sets, lib.DR_CUT_TRACK_PGS)
    assert not logged("still shown") and not logged("normal one")


# ---- the part complete (the reading may stop), the bytes ----

def test_a_part_is_complete_once_the_video_and_every_audio_track_are_past_its_end():
    zero = 100 * F
    pl = ffi.new("DRCutPlace[]", 1)
    pl[0].zero = pl[0].key_time = zero
    c = ffi.new("DRCut **")
    kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO, lib.DR_CUT_TRACK_EVERY, lib.DR_CUT_TRACK_SUB])
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, 1, 0, H264, kinds, 3, ffi.NULL) == 0
    margin = 10 * F

    def frame(track, t, key=False, size=10):
        f = ffi.new("DRFrame *")
        buf = ffi.new("uint8_t[]", size)
        f.time, f.size, f.data = t, size, buf
        f.flags = lib.DR_F_KEY | lib.DR_F_START_CLEAN if key else 0
        return lib.ff_disctl_cut_frame(c[0], track, f)

    frame(0, 0, key=True)
    for n in range(1, 100):
        frame(0, n * F)
    frame(1, 90 * F)
    assert not lib.ff_disctl_cut_done(c[0], margin)
    frame(0, 100 * F, key=True)                      # the next part's key frame
    for n in range(101, 115):
        frame(0, n * F)
    assert not lib.ff_disctl_cut_done(c[0], margin), "the audio is not past the part's end yet"
    frame(1, 101 * F)                                # an audio unit of the next part
    assert lib.ff_disctl_cut_done(c[0], margin)
    title, part, left = ffi.new("int64_t *"), ffi.new("int64_t *"), ffi.new("int64_t *")
    lib.ff_disctl_cut_bytes(c[0], 0, title, part, left)
    assert (title[0], part[0], left[0]) == (115 * 10, 100 * 10, 0), "the title's bytes seen, the part's given"
    lib.ff_disctl_cut_bytes(c[0], 1, title, part, left)
    assert (title[0], part[0]) == (20, 10)
    lib.ff_disctl_cut_close(c)


def test_the_last_part_is_never_complete_early():
    pl = ffi.new("DRCutPlace[]", 1)
    pl[0].zero = pl[0].key_time = F
    c = ffi.new("DRCut **")
    kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO])
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, 1, 1, H264, kinds, 1, ffi.NULL) == 0
    for n in range(0, 500):
        f = ffi.new("DRFrame *")
        f.time, f.flags = n * F, lib.DR_F_KEY | lib.DR_F_START_CLEAN
        lib.ff_disctl_cut_frame(c[0], 0, f)
    assert not lib.ff_disctl_cut_done(c[0], F)
    lib.ff_disctl_cut_close(c)


def test_the_parts_bytes_add_up_to_the_titles_with_the_left_out_pictures():
    pics = stream(OPEN, CLEAN)
    cuts = places(pics, H264)
    totals = []
    for k in range(len(cuts) + 1):
        pl = ffi.new("DRCutPlace[]", len(cuts))
        for i, (z, kt) in enumerate(cuts):
            pl[i].zero, pl[i].key_time = z, kt
        c = ffi.new("DRCut **")
        kinds = ffi.new("int[]", [lib.DR_CUT_TRACK_VIDEO])
        assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, len(cuts), k, H264, kinds, 1, ffi.NULL) == 0
        keep = []
        for t, key, start, rasl, ref in pics:
            buf = ffi.new("uint8_t[]", 7)
            keep.append(buf)
            f = ffi.new("DRFrame *")
            f.time, f.data, f.size = t, buf, 7
            f.flags = (lib.DR_F_KEY | START_FLAG.get(start, 0) if key else 0) | (lib.DR_F_REF if ref else 0)
            lib.ff_disctl_cut_frame(c[0], 0, f)
        title, part, left = ffi.new("int64_t *"), ffi.new("int64_t *"), ffi.new("int64_t *")
        lib.ff_disctl_cut_bytes(c[0], 0, title, part, left)
        totals.append((title[0], part[0], left[0]))
        lib.ff_disctl_cut_close(c)
    assert all(t[0] == len(pics) * 7 for t in totals)
    assert sum(t[1] + t[2] for t in totals) == len(pics) * 7, "parts + left out = the title"
    assert sum(t[2] for t in totals) == 2 * 7, "the open key frame's two leading pictures"
