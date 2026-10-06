"""libavformat/discrip_chapters.c and the joiner's chapter marks: a title's
chapter records (times) become marks; the joiner puts each on the first
video key frame at, or up to 0.4 s before, it; the k-th marked frame starts
chapter k, which ends where the next starts (the last at the title's end);
fewer than two: no chapters."""

from helpers import ffi, lib
from helpers.discrip import Handles

S = 1080000000  # one second
CHAPTER_00 = lib.DR_CHAPTER_00


def plan(records, skip, chapter00):
    """The plan of records ((marks, atoms)), or the error code."""
    p = ffi.new("DRChapterPlan *")
    ret = lib.ff_discrip_chapter_plan(ffi.NULL, ffi.new("int64_t[]", records or [0]), len(records), skip,
                                      int(chapter00), p)
    if ret < 0:
        return ret
    out = ([p.marks[i] for i in range(p.nb_marks)], [p.atoms[i] for i in range(p.nb_atoms)])
    lib.ff_discrip_chapter_plan_free(p)
    return out


def test_a_first_record_at_the_start_and_the_snap():
    # within 0.1 s of the start: at 0, nothing added
    assert plan([0, 60 * S, 120 * S], 0, True) == ([0, 60 * S, 120 * S], [0, 1, 2])
    assert plan([107999999, 60 * S], 0, True) == ([0, 60 * S], [0, 1])
    # from 0.1 s on it is a chapter of its own; one is added at 0 only when asked
    assert plan([108000000, 60 * S], 0, False) == ([108000000, 60 * S], [0, 1])
    assert plan([108000000, 60 * S], 0, True) == ([0, 108000000, 60 * S], [CHAPTER_00, 0, 1])


def test_records_in_leading_segments_left_out():
    # 30 s of leading segments left out: record 0 is dropped, the rest move
    # back; record 0 starts the added chapter at 0
    assert plan([0, 60 * S, 120 * S], 30 * S, False) == ([0, 30 * S, 90 * S], [0, 1, 2])
    # the first kept record lands within 0.1 s of the start: it is at 0
    assert plan([0, 30 * S, 120 * S], 30 * S, False) == ([0, 90 * S], [1, 2])


def test_a_broken_tail_is_cut_and_a_backward_time_refuses_the_title():
    assert plan([0, 60 * S, 120 * S, 0], 0, False) == ([0, 60 * S, 120 * S], [0, 1, 2])
    assert plan([0, 60 * S, 120 * S, 90 * S], 0, False) == ([0, 60 * S, 120 * S], [0, 1, 2])
    assert isinstance(plan([0, 60 * S, 30 * S, 120 * S], 0, False), int)


def test_a_record_at_the_time_of_the_one_before_it_is_dropped():
    # one chapter per time; its record keeps its name (the next one is not
    # shifted onto it)
    assert plan([0, 60 * S, 60 * S, 120 * S], 0, False) == ([0, 60 * S, 120 * S], [0, 1, 3])


def test_no_records_no_chapters():
    assert plan([], 0, True) == ([], [])


# ---- through the joiner ----

def title(records, frames, marker, duration):
    """A title of frames 25 fps video frames, a key frame every 12, an empty
    key marker at marker (if any); the chapters of records (end = duration)
    as (start, end, record)."""
    f_dur = S // 25
    p = ffi.new("DRChapterPlan *")
    assert lib.ff_discrip_chapter_plan(ffi.NULL, ffi.new("int64_t[]", records), len(records), 0, 0, p) == 0

    def drop(track, f):
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    kinds = ffi.new("int[]", [lib.DR_KIND_VIDEO])
    cfg = ffi.new("DRJoinConfig *", {"nb_tracks": 1, "kinds": kinds, "marks": p.marks, "nb_marks": p.nb_marks,
                                     "out": lib.tb_py_join_out, "out_opaque": keep(drop)})
    j = ffi.new("DRJoin **")
    assert lib.ff_discrip_join_open(j, ffi.NULL, cfg) == 0
    assert lib.ff_discrip_join_segment(j[0]) == 0
    held = []
    for k in range(frames):
        if marker == k:
            held.append(ffi.new("DRFrame *", {"size": 0, "time": k * f_dur, "dur": 0,
                                              "flags": lib.DR_F_KEY | lib.DR_F_MARKER, "src": k * f_dur}))
            assert lib.ff_discrip_join_push(j[0], 0, held[-1]) == 0
        flags = (lib.DR_F_KEY if k % 12 == 0 else 0) | (lib.DR_F_BATCH if k in (frames - 1, 0) else 0)
        held.append(ffi.new("DRFrame *", {"size": 1, "time": k * f_dur, "dur": f_dur, "flags": flags,
                                          "src": k * f_dur}))
        assert lib.ff_discrip_join_push(j[0], 0, held[-1]) == 0
    assert lib.ff_discrip_join_finish(j[0]) == 0
    starts = ffi.new("int64_t **")
    ns = lib.ff_discrip_join_chapters(j[0], ffi.cast("const int64_t **", starts))
    lst = ffi.new("DRChapter **")
    nl = ffi.new("int *")
    assert lib.ff_discrip_chapter_list(p, starts[0], ns, duration, lst, nl) == 0
    out = [(lst[0][i].start, lst[0][i].end, lst[0][i].record) for i in range(nl[0])]
    lib.av_free(lst[0])
    lib.ff_discrip_join_close(j)
    lib.ff_discrip_chapter_plan_free(p)
    return out


def test_chapters_start_at_the_key_frames_that_take_the_marks():
    # key frames every 0.48 s; marks at 0, 1.0 s, 2.3 s: the key frames at 0,
    # 0.96 s (0.04 s before), 1.92 s (0.38 s before)
    def k(n):
        return n * 12 * S // 25
    assert title([0, S, 2300000000], 100, None, 4 * S) == [(0, k(2), 0), (k(2), k(4), 1), (k(4), 4 * S, 2)]


def test_a_mark_without_a_key_frame_after_it_is_no_chapter():
    # the second mark lies past the last key frame: one chapter only = none
    assert title([0, 10 * S], 100, None, 4 * S) == []


def test_an_empty_marker_takes_no_chapter():
    # an empty key marker (a placeholder) at 0.92 s, within 0.4 s of the
    # mark at 0.96 s: the mark goes on the real key frame at 0.96 s
    c = title([0, 960000000], 100, 23, 4 * S)
    assert c[1][0] == 24 * S // 25
    assert len(c) == 2
