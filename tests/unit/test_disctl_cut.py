"""The cut stage of the title timeline (src/timeline/disctl_cut.c): of a
title's frames it keeps one cut part's, on the part's clock. The cuts come
from the same pictures through the cut rule (src/disc-remuxer/cuts.c), so
the two are checked against each other: every picture lands in exactly one
part (but the leading pictures left out at an open key frame), each part
starts at 0, and its pictures are the title's from its key frame on."""

from helpers import ffi, lib

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
    assert lib.ff_disctl_cut_open(c, ffi.NULL, pl, len(cuts), k, codec) == 0
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
            assert lib.ff_disctl_cut_frame(c[0], 1, o) == 0, "only the video is cut so far"
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
            # H.264 leaves its non-reference leading pictures out; MPEG-2 / VC-1 keep them
            left_out = set()
            if codec == H264:
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
    # MPEG-2 / VC-1: kept, the part starts with them
    cuts, parts = parts_of(stream(OPEN, CLEAN), OTHER)
    given, _, (n, left, first, start, und) = parts[1]
    assert cuts[0] == (4 * F, 6 * F) and [t // F for t, _ in given] == [6, 4, 5, 9, 7, 8] and (left, und) == (0, 2)


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


def test_other_tracks_are_left_out_so_far():
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
    assert lib.ff_disctl_leading_kept(OPEN, OTHER, 0, 0) == 1, "MPEG-2 / VC-1: kept"
