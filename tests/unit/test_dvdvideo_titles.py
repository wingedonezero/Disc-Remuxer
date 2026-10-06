"""libavformat/dvdvideo_titles.c: the title plan, on discs built byte by
byte: one title per distinct program chain of a title's parts of title
("1", "1/1"), chapters placed in the segments, segments split at a
discontinuity, the cell list from a navigation-scan result (cell walk) or by
trimming, titles with a fake length or below the minimum length listed but
not selected, duplicates dropped, the title order, one title per angle."""

import dataclasses

from helpers import ffi, lib
from helpers.dvd import OpenDisc, plan_from_c
from synth.dvd import Cell, Pgc, Title, Vts, cell_navs, flags, time

CELLS_AUTO, CELLS_WALK = lib.DVDVIDEO_CELLS_AUTO, lib.DVDVIDEO_CELLS_WALK
ORDER_AUTO, ORDER_TABLE = lib.DVDVIDEO_ORDER_AUTO, lib.DVDVIDEO_ORDER_TABLE


class Navs:
    """NAV packs being collected, and the time the next cell starts at."""

    def __init__(self):
        self.navs = []
        self.t0 = 0

    def cell(self, first, vobus, secs):
        """A cell of vobus VOBUs of 10 blocks from block first, secs seconds;
        its NAV packs (0.5 s per VOBU) are collected."""
        starts = [first + 10 * k for k in range(vobus)]
        self.navs += cell_navs(starts, first + 10 * vobus, self.t0)
        self.t0 += 45000 * vobus
        return Cell.new(first, first + 10 * (vobus - 1), first + 10 * vobus - 1, secs)

    def vobus(self):
        return sorted(n.lbn for n in self.navs)


class BuiltScan:
    """A DVDVideoScan with one result per (title, pgcn, cells)."""

    def __init__(self, results):
        self._cells = [ffi.new("uint8_t[]", list(c) or [0]) for _, _, c in results]
        self._results = ffi.new("DVDVideoScanResult[]", max(len(results), 1))
        for i, (title, pgcn, cells) in enumerate(results):
            r = self._results[i]
            r.name = b"A1"
            r.title = title
            r.pgcn = pgcn
            r.cells = self._cells[i]
            r.nb_cells = len(cells)
        self.c = ffi.new("DVDVideoScan *", {"results": self._results, "nb_results": len(results)})


def scan(results):
    return BuiltScan(results)


def plan(d, s, cell_mode, title_order, min_length):
    opt = ffi.new("DVDVideoTitleOptions *", [cell_mode, title_order, min_length])
    p = ffi.new("DVDVideoTitlePlan **")
    assert lib.ff_dvdvideo_titles_plan(ffi.NULL, d.d, s.c if s else ffi.NULL, opt, p) >= 0
    out = plan_from_c(p[0], None)
    lib.ff_dvdvideo_titles_free(p)
    return out


def names(p):
    return [t.name for t in p.titles]


def events(p, name):
    return [e[1] for e in p.events if e[0] == name]


def test_a_title_its_chapters_and_segments(tmp_path):
    n = Navs()
    cells = [
        n.cell(0, 4, 2),
        n.cell(40, 4, 2),
        # a discontinuity starts a new segment
        n.cell(80, 4, 2).with_flags(flags.STC_DISCONTINUITY),
    ]
    pgc = Pgc(cells, programs=[1, 2, 3])
    vts = Vts([pgc], [[(1, 1), (1, 2), (1, 3)]], n.vobus(), 120)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 3)], [(vts, n.navs)])
    p = plan(d, None, CELLS_AUTO, ORDER_AUTO, 0)
    assert names(p) == ["1"]
    t = p.titles[0]
    assert t.cells == [0, 1, 2]
    assert len(t.segments) == 2
    assert (t.segments[0].label, t.segments[0].size) == ("1-2", 80 * 2048)
    assert len(t.segments[0].extents) == 1, "contiguous VOBUs join into one extent"
    assert t.segments[1].label == "3"
    # chapter 2 at cell 2 (block 40), 2 s after the start; chapter 3 starts segment 2
    assert [(c.segment, c.offset) for c in t.chapters] == [(0, 0), (0, 40 * 2048), (1, 0)]
    # its time runs to the END of its first VOBU (2 s + 0.5 s)
    assert t.chapters[1].time == 5 * 45000 * 12000
    assert (t.declared_secs, t.measured_secs, t.not_selected) == (6, 6, 0)
    assert events(p, "title") == ["1\t3\t0:00:06"]


def test_one_title_per_program_chain_of_its_parts(tmp_path):
    n = Navs()
    a = n.cell(0, 4, 2)
    b = n.cell(40, 4, 2)
    vts = Vts([Pgc([a]), Pgc([b])], [[(1, 1), (2, 1)]], n.vobus(), 80)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 2)], [(vts, n.navs)])
    p = plan(d, None, CELLS_AUTO, ORDER_AUTO, 0)
    assert names(p) == ["1", "1/1"]
    assert (p.titles[0].pgcn, p.titles[1].pgcn) == (1, 2)


def test_cell_walk_takes_the_cells_the_navigation_played(tmp_path):
    n = Navs()
    cells = [n.cell(40 * k, 4, 2) for k in range(4)]
    vts = Vts([Pgc(cells)], [[(1, 1)]], n.vobus(), 160)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 1)], [(vts, n.navs)])
    p = plan(d, scan([(1, 1, [2, 3])]), CELLS_WALK, ORDER_AUTO, 0)
    assert p.titles[0].cells == [1, 2]
    assert events(p, "cells-cut-start") == ["1"]
    assert events(p, "cells-cut-end") == ["4\t4"]
    # cell walk without a scan result for the title: no title
    p = plan(d, scan([]), CELLS_WALK, ORDER_AUTO, 0)
    assert p.titles == []


def test_fake_and_short_titles_are_listed_not_selected(tmp_path):
    n = Navs()
    # declares 1000 s, its NAV packs play 2 s
    fake = dataclasses.replace(n.cell(0, 4, 2), playback_time=time(1000))
    short = n.cell(40, 4, 2)
    vts = Vts([Pgc([fake]), Pgc([short])], [[(1, 1)], [(2, 1)]], n.vobus(), 80)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 1), Title(1, 2, 1)], [(vts, n.navs)])
    p = plan(d, None, CELLS_AUTO, ORDER_AUTO, 120)
    assert names(p) == ["1", "2"]
    assert p.titles[0].not_selected == lib.DVDVIDEO_TITLE_FAKE
    assert p.titles[0].segments == [], "a fake title gets no segments"
    assert events(p, "fake-length") == ["1\t0:16:40\t0:00:02"]
    assert p.titles[1].not_selected == lib.DVDVIDEO_TITLE_SHORT
    assert len(p.titles[1].segments) == 1, "a short title is built: it can still be chosen"
    assert events(p, "short") == ["2\t2\t120"]


def test_a_title_that_repeats_another_is_dropped(tmp_path):
    n = Navs()
    a = n.cell(0, 4, 2)
    vts = Vts([Pgc([a])], [[(1, 1)], [(1, 1)]], n.vobus(), 40)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 1), Title(1, 2, 1)], [(vts, n.navs)])
    p = plan(d, None, CELLS_AUTO, ORDER_AUTO, 0)
    assert names(p) == ["1"]
    assert events(p, "duplicate") == ["1\t#01\t#02"]


def test_title_order(tmp_path):
    n = Navs()
    a = n.cell(0, 4, 2)
    b = n.cell(40, 4, 2)
    vts = Vts([Pgc([a]), Pgc([b])], [[(1, 1)], [(2, 1)]], n.vobus(), 80)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 1), Title(1, 2, 1)], [(vts, n.navs)])
    # the scan reached title 2 only: it comes first (title 1 has no scan
    # result and is left out by cell walk, but listed by the trim of walk_trim)
    s = scan([(2, 2, [1])])
    assert names(plan(d, s, CELLS_AUTO, ORDER_AUTO, 0)) == ["2"]
    assert names(plan(d, s, 3, ORDER_AUTO, 0)) == ["1", "2"], "walk_trim: the title table's order"
    assert names(plan(d, s, 3, 1, 0)) == ["2", "1"], "scan-first asked for"
    assert names(plan(d, s, 3, ORDER_TABLE, 0)) == ["1", "2"]


def test_one_title_per_angle(tmp_path):
    n = Navs()
    first = n.cell(0, 4, 2)
    a1 = n.cell(40, 4, 2).with_flags(flags.FIRST_IN_BLOCK | flags.ANGLE_BLOCK)
    a2 = n.cell(80, 4, 2).with_flags(flags.LAST_IN_BLOCK | flags.ANGLE_BLOCK)
    vts = Vts([Pgc([first, a1, a2])], [[(1, 1)]], n.vobus(), 120)
    d = OpenDisc.build(tmp_path, [Title(1, 1, 1, nr_of_angles=2)], [(vts, n.navs)])
    p = plan(d, None, CELLS_AUTO, ORDER_AUTO, 0)
    assert names(p) == ["1", "1"]
    assert (p.titles[0].angle, p.titles[0].cells) == (0, [0, 1])
    assert (p.titles[1].angle, p.titles[1].cells) == (1, [0, 2])
    assert events(p, "angle") == ["2\t1"]


def two_title_set():
    """One title set with two titles (program chains 1 and 2)."""
    n = Navs()
    a = n.cell(0, 4, 2)
    b = n.cell(40, 4, 3)
    return Vts([Pgc([a]), Pgc([b])], [[(1, 1)], [(2, 1)]], n.vobus(), 80), n.navs


def test_a_title_naming_a_title_set_the_disc_does_not_have_is_left_out_with_a_warning(tmp_path):
    d = OpenDisc.build(tmp_path, [Title(1, 1, 1), Title(5, 1, 1)], [two_title_set()])
    p = plan(d, None, CELLS_AUTO, ORDER_TABLE, 0)
    assert names(p) == ["1"], "the rest of the disc stays usable"
    assert events(p, "title-set-invalid") == ["2\t5\t1"]


def test_a_title_giving_another_start_for_its_title_set_is_reported(tmp_path):
    def at(ttn, sector):
        return Title(1, ttn, 1, title_set_starting_sector=sector)
    # the first non-zero start is the set's; a later different one (0 too) is
    # reported (set, this start, the first); then the set's start by the
    # disc's layout: the VMG (3 IFO blocks, vmg_last_sector 5) ends before
    # block 6 (set, layout start, the first)
    for test, starts, want in [
        ("titles-start-layout", [6, 6], []),
        ("titles-start-same", [100, 100], ["1\t6\t100"]),
        ("titles-start-zero-first", [0, 100], ["1\t6\t100"]),
        ("titles-start-differs", [100, 200], ["1\t200\t100", "1\t6\t100"]),
        ("titles-start-zero-later", [100, 0], ["1\t0\t100", "1\t6\t100"]),
        ("titles-start-unset", [0, 0], []),
    ]:
        d = OpenDisc.build(tmp_path / test, [at(1, starts[0]), at(2, starts[1])], [two_title_set()])
        p = plan(d, None, CELLS_AUTO, ORDER_TABLE, 0)
        assert events(p, "title-set-start-mismatch") == want, test
        assert len(names(p)) == 2, f"{test}: both titles stay"
