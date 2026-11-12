"""Cuts (src/disc-remuxer/cuts.c): the places a part can start, read from
a title's pictures in decoding order, and the rule that turns a chapter or a
time into a cut. Times in ticks (DR_TICKS_PER_SECOND = 1,080,000,000);
pictures of 23.976 fps video (1001/24000 s = 45,045,000 ticks)."""

from helpers import ffi, lib

TPS = 1_080_000_000
F = 45_045_000                      # one picture at 23.976
MS = TPS // 1000
CLEAN, BROKEN, OPEN = lib.DR_DISC_START_CLEAN, lib.DR_DISC_START_BROKEN, lib.DR_DISC_START_OPEN
OTHER, H264, HEVC = lib.DR_CUT_OTHER, lib.DR_CUT_H264, lib.DR_CUT_HEVC


def pics(*specs):
    """Pictures in decoding order: (picture number shown, start) for key
    frames, a number for the others; ('r', n) an HEVC RASL picture, ('R', n)
    an H.264 reference picture."""
    out = []
    for s in specs:
        if isinstance(s, tuple) and s[0] in ('r', 'R'):
            out.append((s[1] * F, 0, 0, s[0] == 'r', s[0] == 'R'))
        elif isinstance(s, tuple):
            out.append((s[0] * F, 1, s[1], 0, 1))
        else:
            out.append((s * F, 0, 0, 0, 0))
    a = ffi.new("CutPicture[]", len(out))
    for i, (t, key, start, rasl, ref) in enumerate(out):
        a[i].time, a[i].duration, a[i].key, a[i].start, a[i].rasl, a[i].ref = t, F, key, start, rasl, ref
    return a, len(out)


def points(specs, codec=OTHER):
    """(zero, key frame, its start, kept decodable, kept not decodable, left out), in pictures."""
    a, n = pics(*specs)
    p, np_ = ffi.new("CutPoint **"), ffi.new("int *")
    assert lib.cut_points(a, n, codec, p, np_) == 0
    got = [(p[0][i].zero // F, p[0][i].key_time // F, p[0][i].start, p[0][i].kept, p[0][i].undecodable,
            p[0][i].left_out) for i in range(np_[0])]
    lib.free(p[0])
    return got


# two groups of an H.264 / MPEG-2 stream: I P B B | I B B P B B (the second
# key frame's B pictures follow it in decoding order and are shown before it)
GOPS = [(0, CLEAN), 3, 1, 2, (6, None), 4, 5, 9, 7, 8, (12, CLEAN), 10, 11]


def gops(second):
    return [(s[0], second) if isinstance(s, tuple) and s[1] is None else s for s in GOPS]


def test_an_open_key_frame_leaves_out_what_the_codec_allows_and_keeps_the_rest():
    # H.264: non-reference leading pictures are left out (no frame_num goes missing)
    assert points(gops(OPEN), H264) == [(6, 6, OPEN, 0, 0, 2), (10, 12, CLEAN, 2, 0, 0)]
    assert points(gops(BROKEN), H264)[0] == (6, 6, BROKEN, 0, 0, 2)
    # H.264: a reference leading picture is kept (left out it would leave a frame_num gap); the others go
    assert points([(0, CLEAN), 3, 1, 2, (6, OPEN), ('R', 4), 5, 9, 7, 8], H264) == [(4, 6, OPEN, 0, 1, 1)]
    # MPEG-2 / VC-1: their B pictures left out; the part starts on its key frame
    assert points(gops(OPEN), OTHER)[0] == (6, 6, OPEN, 0, 0, 2)


def test_a_clean_key_frame_keeps_its_leading_pictures_and_starts_with_them():
    # a closed GOP: its B pictures decode from the I picture after them
    for codec in (OTHER, H264, HEVC):
        assert points(gops(CLEAN), codec)[0] == (4, 6, CLEAN, 2, 0, 0)


def test_hevc_marks_each_leading_picture():
    # a CRA picture with a RASL picture (left out) and a RADL picture (kept)
    specs = [(0, CLEAN), 3, 1, 2, (6, OPEN), ('r', 4), 5, 9, 7, 8]
    assert points(specs, HEVC) == [(5, 6, OPEN, 1, 0, 1)]


def verdicts(specs, codec):
    """Per place: clean, its short words, how long the left-out pictures are shown and when the part's first
    picture that decodes is (both in pictures)."""
    a, n = pics(*specs)
    p, np_ = ffi.new("CutPoint **"), ffi.new("int *")
    assert lib.cut_points(a, n, codec, p, np_) == 0
    brief, full = ffi.new("char[]", 64), ffi.new("char[]", 768)
    out = []
    for i in range(np_[0]):
        ok = lib.cut_point_clean(p[0] + i, brief, 64, full, 768)
        out.append((ok, ffi.string(brief).decode(), p[0][i].lost // F, p[0][i].shown // F, ffi.string(full).decode()))
    lib.free(p[0])
    return out


def test_a_cut_says_whether_it_is_clean_and_what_it_costs():
    # a closed GOP, an IDR picture, a CRA picture with only RADL pictures: clean in every codec
    for codec in (OTHER, H264, HEVC):
        ok, brief, lost, shown, full = verdicts(gops(CLEAN), codec)[0]
        assert (ok, brief, lost, shown) == (1, "clean", 0, 0) and full.startswith("a clean cut: nothing lost")
    assert verdicts([(0, CLEAN), 3, 1, 2, (6, OPEN), 4, 5, 9, 7, 8], HEVC)[0][:2] == (1, "clean"), "RADL pictures decode"
    # an open MPEG-2 / VC-1 key frame: its two B pictures lost, the part starts at 0 on the key frame
    ok, brief, lost, shown, full = verdicts(gops(OPEN), OTHER)[0]
    assert (ok, brief, lost, shown) == (0, "open: loses 2 picture(s)", 2, 0)
    assert "the part before loses its last 0.083 s of pictures" in full
    # H.264 with a reference leading picture: one lost, one kept at the start that players do not show
    ok, brief, lost, shown, full = verdicts([(0, CLEAN), 3, 1, 2, (6, OPEN), ('R', 4), 5, 9, 7, 8], H264)[0]
    assert (ok, brief, lost, shown) == (0, "open: loses 1, 1 hidden", 1, 2)
    assert "starts with 1 picture(s) that cannot be decoded" in full and "first picture shown is at 0.083 s" in full


def test_the_first_picture_is_no_place_to_cut():
    assert points([(0, CLEAN), 1, 2]) == []


def req(*items):
    r = ffi.new("CutRequest[]", len(items))
    for i, it in enumerate(items):
        if isinstance(it, str):
            r[i].chapter = int(it[1:])
        else:
            r[i].time = it
    return r, len(items)


def resolve(specs, items, rule=lib.CUT_NEAREST, chapters=(), end=None, codec=OTHER):
    a, n = pics(*specs)
    p, np_ = ffi.new("CutPoint **"), ffi.new("int *")
    assert lib.cut_points(a, n, codec, p, np_) == 0
    r, nr = req(*items)
    res = ffi.new("CutResult[]", nr)
    ch = ffi.new("int64_t[]", [c for c in chapters] or [0])
    if end is None:
        end = max(a[i].time for i in range(n)) + F
    made = lib.cut_resolve(p[0], np_[0], ch, len(chapters), end, r, nr, rule, res)
    out = []
    for i in range(nr):
        if res[i].point < 0:
            out.append(('no', ffi.string(res[i].why).decode(), res[i].same_as))
        else:
            out.append((p[0][res[i].point].zero // F, res[i].on_point))
    lib.free(p[0])
    return made, out


# key frames shown at 6, 24 and 48 (each its own group, nothing shown before it)
STREAM = [(0, CLEAN)] + list(range(1, 6)) + [(6, CLEAN)] + list(range(7, 24)) + [(24, OPEN)] + \
         list(range(25, 48)) + [(48, CLEAN)] + list(range(49, 60))


def test_a_time_on_a_key_frame_is_cut_there_within_1_ms():
    assert resolve(STREAM, [24 * F]) == (1, [(24, 1)])
    assert resolve(STREAM, [24 * F + MS]) == (1, [(24, 1)])
    assert resolve(STREAM, [24 * F - MS]) == (1, [(24, 1)])
    # just past 1 ms is no longer on it (nearest still takes it)
    assert resolve(STREAM, [24 * F + MS + 1]) == (1, [(24, 0)])


def test_a_closed_gops_key_frame_time_is_on_its_point_too():
    # the chapter on the I picture of a closed GOP: the part starts 2 pictures earlier
    assert resolve(gops(CLEAN), [6 * F]) == (1, [(4, 1)])


def test_nearest_takes_the_nearest_key_frame_and_the_earlier_one_on_a_tie():
    assert resolve(STREAM, [10 * F]) == (1, [(6, 0)])
    assert resolve(STREAM, [20 * F]) == (1, [(24, 0)])
    assert resolve(STREAM, [15 * F]) == (1, [(6, 0)]), "9 pictures from both: the earlier one"


def test_next_takes_the_first_key_frame_after_the_time():
    assert resolve(STREAM, [10 * F], rule=lib.CUT_NEXT) == (1, [(24, 0)])
    assert resolve(STREAM, [24 * F], rule=lib.CUT_NEXT) == (1, [(24, 1)]), "on a key frame: cut there"
    assert resolve(STREAM, [50 * F], rule=lib.CUT_NEXT) == (0, [('no', 'no key frame after it', -1)])


def test_chapters_are_cut_by_their_start():
    ch = [0, 6 * F, 23 * F, 50 * F]
    assert resolve(STREAM, ['c2', 'c3'], chapters=ch) == (2, [(6, 1), (24, 0)])
    assert resolve(STREAM, ['c1'], chapters=ch) == (0, [('no', "at the title's start: no part before it", -1)])
    assert resolve(STREAM, ['c5'], chapters=ch) == (0, [('no', 'the title has no such chapter', -1)])


def test_what_cannot_be_cut_is_refused_with_why():
    assert resolve(STREAM, [0])[1] == [('no', "at the title's start: no part before it", -1)]
    assert resolve(STREAM, [60 * F])[1] == [('no', "not before the end of the title's video", -1)]
    # two requests that land on one key frame: the second is no cut
    assert resolve(STREAM, [24 * F, 23 * F]) == (1, [(24, 1), ('no', 'the same key frame as an earlier cut', 0)])
    assert resolve([(0, CLEAN), 1, 2, 3], [2 * F])[1] == [('no', "the title's video has no key frame to cut at",
                                                          -1)]


def parse(spec):
    r, n, bad = ffi.new("CutRequest **"), ffi.new("int *"), ffi.new("const char **")
    if lib.cut_parse(spec.encode(), r, n, bad) < 0:
        return None
    out = [(r[0][i].chapter, r[0][i].time, ffi.string(r[0][i].text).decode()) for i in range(n[0])]
    lib.free(r[0])
    return out


def test_cuts_are_read_as_chapters_and_times():
    assert parse("c5, 45:42.322 ,1:23:45.6") == [
        (5, 0, "c5"),
        (0, (45 * 60 + 42) * TPS + 322 * MS, "45:42.322"),
        (0, (3600 + 23 * 60 + 45) * TPS + 600 * MS, "1:23:45.6")]
    assert parse("95:00") == [(0, 95 * 60 * TPS, "95:00")], "minutes past 59 without hours"
    assert parse("0:00:01.000000001") == [(0, TPS + 1, "0:00:01.000000001")], "9 decimals: 1 ns = 1.08 ticks, cut"


def test_what_is_not_a_cut_is_refused():
    for bad in ["", "5", "45:60", "1:60:00", "c0", "c", "cx", "a,b", "c5,", ",c5", "1:2:3:4", "45:42.",
                "45:42.1234567890"]:
        assert parse(bad) is None, bad


def test_a_time_is_written_to_the_ns():
    buf = ffi.new("char[40]")
    assert ffi.string(lib.cut_time_text(buf, 40, 32900 * F)) == b"00:22:52.204.166.667"


# ---- cuts on a title written as its parts: each cut inside the part it falls in ----

def in_part(item, starts, chapters, k):
    r, n, bad = ffi.new("CutRequest **"), ffi.new("int *"), ffi.new("const char **")
    assert lib.cut_parse(item.encode(), r, n, bad) == 0 and n[0] == 1
    out = ffi.new("CutInPart *")
    lib.cut_in_part(r[0], ffi.new("int64_t[]", starts), ffi.new("int[]", chapters), len(starts), k, out)
    lib.free(r[0])
    return {lib.CUT_ELSEWHERE: None, lib.CUT_INSIDE: ("in", out.chapter, out.time),
            lib.CUT_ON_BREAK: ("break", out.chapter, out.time)}[out.where]


def test_a_chapter_cut_goes_to_the_part_that_holds_the_chapter_with_its_own_number():
    starts, chapters = [0, 1000 * TPS, 1500 * TPS], [29, 4, 2]     # the title's chapters 1-29, 30-33, 34-35
    assert in_part("c6", starts, chapters, 0) == ("in", 6, 0) and in_part("c6", starts, chapters, 1) is None
    assert in_part("c29", starts, chapters, 0) == ("in", 29, 0), "a part's last chapter"
    assert in_part("c31", starts, chapters, 1) == ("in", 2, 0) and in_part("c31", starts, chapters, 0) is None
    assert in_part("c35", starts, chapters, 2) == ("in", 2, 0)
    # a part's first chapter is where the disc breaks the title: no cut
    assert in_part("c30", starts, chapters, 1) == ("break", 1, 0) and in_part("c30", starts, chapters, 0) is None
    assert in_part("c34", starts, chapters, 2) == ("break", 1, 0)
    # the title's first chapter is an ordinary request (the cut rule refuses the first picture)
    assert in_part("c1", starts, chapters, 0) == ("in", 1, 0)
    # a chapter the title does not have stays with the last part (refused there with its reason)
    assert in_part("c40", starts, chapters, 2) == ("in", 7, 0) and in_part("c40", starts, chapters, 1) is None


def test_a_time_cut_goes_to_the_part_it_lies_in_counted_from_that_parts_start():
    starts, chapters = [0, 1000 * TPS, 1500 * TPS], [29, 4, 2]
    assert in_part("10:00", starts, chapters, 0) == ("in", 0, 600 * TPS)
    assert in_part("20:00", starts, chapters, 1) == ("in", 0, 200 * TPS) and in_part("20:00", starts, chapters, 0) is None
    assert in_part("30:00", starts, chapters, 2) == ("in", 0, 300 * TPS), "past the last start: the last part's"
    # on a part's start, within 1 ms either way: the disc's own break, that part's, no cut
    for t in ("16:40", "16:39.9995", "16:40.0005"):
        assert in_part(t, starts, chapters, 1)[0] == "break", t
        assert in_part(t, starts, chapters, 0) is None, t
    # 2 ms before the break is a cut inside the part before, 2 ms after it one inside the part
    assert in_part("16:39.998", starts, chapters, 0) == ("in", 0, 1000 * TPS - 2 * MS)
    assert in_part("16:40.002", starts, chapters, 1) == ("in", 0, 2 * MS)
    # every request lands in exactly one part
    for item in ("c1", "c17", "c30", "c33", "c35", "0:01", "16:40", "25:00", "99:00"):
        assert sum(in_part(item, starts, chapters, k) is not None for k in range(3)) == 1, item
