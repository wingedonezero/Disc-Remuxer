"""libavformat/dvdvideo_cells.c: which program chains and cells are not
real content (rejection reasons), on discs built byte by byte."""

import dataclasses

from helpers import ffi, lib
from helpers.dvd import OpenDisc
from synth.dvd import Cell, Nav, Pgc, Title, Vts, cell_navs, flags, nav, time


def one_set(folder, pgcs, vobus, vob_sectors, navs):
    """A disc of one title set holding pgcs (one title, chapter 1 = PGC 1),
    its VOBU map, title VOBs and NAV packs."""
    vts = Vts(pgcs, [[(1, 1)]], list(vobus), vob_sectors)
    return OpenDisc.build(folder, [Title(1, 1, 1)], [(vts, navs)])


def shared_sets(folder, pgcs, vobus, vob_sectors, navs):
    """Two title sets with the same pgcs, whose title VOBs overlap by the
    title search table (both start at sector 0)."""
    def vts():
        return Vts(list(pgcs), [[(1, 1)]], list(vobus), vob_sectors), list(navs)
    return OpenDisc.build(folder, [Title(1, 1, 1), Title(2, 1, 1)], [vts(), vts()])


def rejection(d, pgcn):
    reason = ffi.new("int *", -1)
    pgc = lib.ff_dvdvideo_disc_pgc(d.d, 1, pgcn)
    assert pgc != ffi.NULL
    ret = lib.ff_dvdvideo_pgc_rejection(d.d, 1, pgcn, pgc, reason)
    assert ret >= 0, f"error {ret}"
    return reason[0] if ret == 1 else None


def cell_rejected(d, pgcn, i, quick, prev):
    pgc = lib.ff_dvdvideo_disc_pgc(d.d, 1, pgcn)
    ret = lib.ff_dvdvideo_cell_rejected(d.d, 1, pgc, i, int(quick), prev)
    assert ret >= 0, f"error {ret}"
    return ret == 1


def plain_cells(n, length, secs):
    """n cells of length blocks each from block 0, secs seconds each, one
    VOBU per cell; with their NAV packs."""
    cells = [Cell.new(k * length, k * length, k * length + length - 1, secs) for k in range(n)]
    starts = [k * length for k in range(n)]
    navs = [dataclasses.replace(nav(s, length, None), vobu_e_ptm=45000) for s in starts]
    return cells, starts, navs


def pgc(cells, programs):
    """One PGC with programs at programs."""
    return Pgc(cells, programs=list(programs))


def test_a_chain_without_cells_is_rejected(tmp_path):
    cells, starts, navs = plain_cells(1, 10, 10)
    d = one_set(tmp_path, [Pgc(cells), Pgc([])], starts, 10, navs)
    assert rejection(d, 1) is None
    assert rejection(d, 2) == 0


def test_too_little_data_per_second_is_rejected(tmp_path):
    # 4 cells without a playback time: no accepted second at all
    cells, starts, navs = plain_cells(4, 1000, 10)
    for c in cells:
        c.playback_time = bytes([0, 0, 0, 0x40])
    d = one_set(tmp_path / "rate0", [Pgc(cells)], starts, 4000, navs)
    assert rejection(d, 1) == 3
    # 4 cells of 10 blocks and 60 s each: 40 blocks in 240 s, below 50 a second
    cells, starts, navs = plain_cells(4, 10, 60)
    d = one_set(tmp_path / "rate", [Pgc(cells)], starts, 40, navs)
    assert rejection(d, 1) == 4
    # 1000 blocks per 10 s cell: accepted
    cells, starts, navs = plain_cells(4, 1000, 10)
    d = one_set(tmp_path / "rate-ok", [Pgc(cells)], starts, 4000, navs)
    assert rejection(d, 1) is None


def test_a_broken_program_map_is_rejected(tmp_path):
    cells, starts, navs = plain_cells(3, 10, 10)
    pgcs = [pgc(cells, [1, 4]), pgc(cells, [2, 1, 3]), pgc(cells, [1, 2, 3])]
    d = one_set(tmp_path, pgcs, starts, 30, navs)
    assert rejection(d, 1) == 5, "an entry cell past the last cell"
    assert rejection(d, 2) == 6, "entry cells out of order"
    assert rejection(d, 3) is None


def test_seamless_joins_must_be_contiguous(tmp_path):
    # cell 3 joins cell 2 seamlessly; program 1 is not examined
    def joined(gap):
        return [Cell.new(0, 0, 9, 10), Cell.new(10, 10, 19, 10),
                Cell.new(20 + gap, 20 + gap, 29 + gap, 10).with_flags(flags.SEAMLESS_PLAY)]
    starts = [0, 10, 20, 25]
    navs = [nav(s, 5, None) for s in starts]
    d = one_set(tmp_path, [pgc(joined(0), [1, 2, 3]), pgc(joined(5), [1, 2, 3])], starts, 40, navs)
    assert rejection(d, 1) is None
    assert rejection(d, 2) == 7, "a gap between seamlessly joined cells"


def test_a_gap_after_an_interleaved_cell_must_be_interleaved_units(tmp_path):
    # cell 2 interleaved, cell 3 plain and seamless: the 5 blocks between them
    # must be interleaved units
    cells = [
        Cell.new(0, 0, 9, 10),
        Cell.new(10, 10, 19, 10).with_flags(flags.INTERLEAVED),
        Cell.new(25, 25, 34, 10).with_flags(flags.SEAMLESS_PLAY),
    ]
    units = dataclasses.replace(nav(20, 5, None), category=0x6000, ilvu_ea=4)
    base = [nav(0, 10, None), nav(10, 10, None), nav(25, 10, None)]
    d = one_set(tmp_path / "ok", [pgc(cells, [1, 2, 3])], [0, 10, 20, 25], 35, base + [units])
    assert rejection(d, 1) is None, "an interleaved unit fills the gap"
    d = one_set(tmp_path / "gap", [pgc(cells, [1, 2, 3])], [0, 10, 25], 35, base)
    assert rejection(d, 1) == 8, "nothing fills the gap"


def test_protection_patterns_on_shared_title_vobs_are_rejected(tmp_path):
    # 12 cells (100 blocks, 2 s each), all after the first with an STC discontinuity
    cells, starts, navs = plain_cells(12, 100, 2)
    for c in cells[1:]:
        c.flags = flags.STC_DISCONTINUITY
    d = shared_sets(tmp_path / "stc12", [Pgc(cells)], starts, 1200, navs)
    assert rejection(d, 1) == 1
    # the same on title VOBs of its own: accepted
    d = one_set(tmp_path / "stc12-own", [Pgc(cells)], starts, 1200, navs)
    assert rejection(d, 1) is None
    # 7 flagged cells, not in sector order
    cells, starts, navs = plain_cells(7, 100, 2)
    for c in cells[1:]:
        c.flags = flags.STC_DISCONTINUITY
    cells[2], cells[4] = cells[4], cells[2]
    d = shared_sets(tmp_path / "stc7", [Pgc(cells)], starts, 700, navs)
    assert rejection(d, 1) == 2


def test_a_broken_interleaved_angle_block_on_shared_title_vobs_is_rejected(tmp_path):
    # cell 2 starts an interleaved angle block that cell 3 does not continue
    cells = [
        Cell.new(0, 0, 9, 10),
        Cell.new(10, 10, 19, 10).with_flags(flags.FIRST_IN_BLOCK | flags.ANGLE_BLOCK | flags.INTERLEAVED),
        Cell.new(20, 20, 29, 10).with_flags(flags.SEAMLESS_PLAY),
    ]
    navs = [nav(s, 10, None) for s in [0, 10, 20]]
    d = shared_sets(tmp_path, [pgc(cells, [1, 2, 3])], [0, 10, 20], 30, navs)
    assert rejection(d, 1) == 9


def test_cell_rejects(tmp_path):
    cells = [
        Cell.new(0, 0, 9, 10),
        # no playback time
        dataclasses.replace(Cell.new(10, 10, 19, 10), playback_time=bytes([0, 0, 0, 0x40])),
        # one second, one VOBU
        Cell.new(20, 20, 29, 1),
        # five seconds with a cell command
        dataclasses.replace(Cell.new(30, 30, 39, 5), cell_cmd_nr=1),
        # six seconds with a cell command
        dataclasses.replace(Cell.new(40, 40, 49, 6), cell_cmd_nr=1),
        Cell.new(50, 50, 59, 10),
    ]
    cells[2].playback_time = time(1)
    starts = [10 * k for k in range(6)]
    navs = [dataclasses.replace(nav(s, 10, None), vobu_e_ptm=45000) for s in starts]
    p = Pgc(cells, cell_cmds=[bytes([0x20, 0x01, 0, 0, 0, 0, 0, 0x0d])])
    d = one_set(tmp_path, [p], starts, 60, navs)
    assert not cell_rejected(d, 1, 0, False, -1)
    assert cell_rejected(d, 1, 1, True, 0), "no playback time"
    assert cell_rejected(d, 1, 2, False, -1), "a one-second cell without the cell before it"
    assert not cell_rejected(d, 1, 2, False, 1), "a one-second cell after the cell before it"
    assert cell_rejected(d, 1, 3, False, 2), "a cell command on a short cell"
    assert not cell_rejected(d, 1, 4, False, 3), "a cell command on a six-second cell"


def test_a_cell_outside_the_title_vobs_is_rejected(tmp_path):
    cells = [Cell.new(0, 0, 9, 10), Cell.new(10, 10, 19, 10)]
    d = one_set(tmp_path, [Pgc(cells)], [0, 10], 15, [nav(0, 10, None), nav(10, 5, None)])
    assert not cell_rejected(d, 1, 0, True, -1)
    assert cell_rejected(d, 1, 1, True, 0), "ends past the 15 blocks of the title VOBs"


def test_every_vobu_of_a_long_cell_must_be_a_nav_pack(tmp_path):
    # a 31-minute cell of 60 VOBUs of 10 blocks
    starts = [10 * k for k in range(60)]

    def cell():
        return [Cell.new(0, 590, 599, 31 * 60)]
    navs = cell_navs(starts, 600, 0)
    d = one_set(tmp_path / "ok", [Pgc(cell())], starts, 600, navs)
    assert not cell_rejected(d, 1, 0, False, -1)
    # VOBU 37 has no NAV pack: found on every run (no random sample)
    holed = navs[:37] + navs[38:]
    for run in range(3):
        d = one_set(tmp_path / f"hole-{run}", [Pgc(cell())], starts, 600, holed)
        assert cell_rejected(d, 1, 0, False, -1)


def test_a_long_single_cell_of_tiny_uniform_vobus_is_filler(tmp_path):
    # one cell, 31 minutes, 1001 VOBUs of 2 blocks
    starts = [2 * k for k in range(1001)]
    d = one_set(tmp_path, [Pgc([Cell.new(0, 2000, 2001, 31 * 60)])], starts, 2002, [])
    assert cell_rejected(d, 1, 0, False, -1)


def test_cell_commands_that_may_link(tmp_path):
    cmds = [
        [0x20, 0x01, 0, 0, 0, 0, 0, 0x00],        # link without a target
        [0x20, 0x01, 0, 0, 0, 0, 0, 0x0d],        # link
        [0x20, 0x31, 0, 0x05, 0, 0x05, 0, 0x0d],  # link if GPRM 5 > GPRM 5: never
        [0x20, 0x31, 0, 0x05, 0, 0x06, 0, 0x0d],  # link if GPRM 5 > GPRM 6
        [0x60, 0x01, 0, 0, 0, 0, 0, 0x00],
        [0x70, 0x04, 0, 0, 0, 0, 0, 0x01],
    ]
    p = Pgc([Cell.new(0, 0, 9, 10)], cell_cmds=[bytes(c) for c in cmds])
    d = one_set(tmp_path, [p], [0], 10, [nav(0, 10, None)])

    def may(k):
        return lib.ff_dvdvideo_cell_cmd_may_link(lib.ff_dvdvideo_disc_pgc(d.d, 1, 1), k) == 1
    assert not may(0), "no command"
    assert not may(1)
    assert may(2)
    assert not may(3)
    assert may(4)
    assert not may(5)
    assert may(6)
    assert not may(7), "past the table"
