"""disc/dvd/dvd_scan.c: the navigation scan (step D2). Built disc
folders: the disc is played from its start (its first-play chain jumps to
a last, one-cell title) and then title by title; what each title's program
chains play is recorded. With DVD_SCAN_REFERENCE (a folder of <disc>.expected
trace files) and DVD_SCAN_DISCS (a list of "<disc>\\t<path>" lines) the scan
of every listed disc must give its expected trace line for line: every
navigator copy, parameter, event, sector read and path line."""

import dataclasses
import pathlib

from helpers.disc import env_or_skip
from helpers.dvdnav import (NOP, Pgc, build, call_ss_vmgm_pgc, cell, if_goto, jump_ss_vmgm_pgc, jump_tt, scan,
                            set_gprm, vobus)
from synth.dvd import END_OF_CELL, Cell, Menu, Nav, flags
from test_dvd_disc import Disc
from test_dvd_nav import open_disc

LINK_TOP_PGC = [0x20, 0x01, 0, 0, 0, 0, 0, 0x09]


def rnd(dst, n):
    """Set (Rnd): GPRM dst = a random number from 1 to n."""
    return [0x78, 0, 0, dst, n >> 8, n & 0xff, 0, 0]


def if_link_pgcn(reg, value, pgcn):
    """if GPRM reg == value: LinkPGCN pgcn."""
    return [0x20, 0x80 | 0x20 | 0x04, 0, reg, value >> 8, value & 0xff, pgcn >> 8, pgcn & 0xff]


def results(folder):
    d = Disc.folder(folder)
    ret, res, lines = scan(d.d, True)
    d.close()
    return ret, {(r.title, r.pgc): r for r in res}, lines


def test_a_title_s_cells_are_recorded_and_named(tmp_path):
    p = Pgc(cells=[cell(0, 9), cell(10, 19)], programs=[1, 2])
    folder = build(tmp_path, [p], [[(1, 1), (1, 2)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    ret, res, lines = results(folder)
    assert ret == 0
    assert res[(1, 1)].cells == [1, 2]
    assert all(r.name[0].isalpha() and r.name[-1].isdigit() for r in res.values())
    assert lines[0] == "nav 1" and "log AP: 0:00:FFFFFFFF:00\\n" in lines


def test_a_cell_is_recorded_at_its_first_data_block(tmp_path):
    # cell 2's VOBUs are NAV packs only (no data block): played, not recorded
    p = Pgc(cells=[cell(0, 9), cell(10, 19)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 1), 20)
    ret, res, lines = results(folder)
    assert ret == 0 and res[(1, 1)].cells == [1]
    assert "ev 6 0 1 1 1 2 0 1 5 00000000" in lines, "cell 2 was played"


def test_the_disc_s_random_numbers_are_the_c_library_s(tmp_path):
    # title 1's chain draws a number from 1 to 4 and links to chain 1 + it
    # (chains 2-5 are parts 2-5 of title 1); the first draw of rand() (seed
    # 1) is 1804289383: 1 + 4 * r / 2^31 = 4
    pre = [rnd(0, 4)] + [if_link_pgcn(0, v, 1 + v) for v in range(1, 5)]
    pgcs = [Pgc(cells=[cell(0, 9)], pre=pre)] + [Pgc(cells=[cell(10 * k, 10 * k + 9)]) for k in range(1, 5)]
    navs_ = [n for k in range(5) for n in vobus(10 * k, 10 * k + 9, 5)]
    folder = build(tmp_path, pgcs, [[(1, 1), (2, 1), (3, 1), (4, 1), (5, 1)]], navs_, 50)
    ret, res, lines = results(folder)
    assert ret == 0
    assert sorted(res) == [(1, 5)]


def test_a_looping_chain_entered_in_its_middle_is_recorded_once_from_its_first_cell(tmp_path):
    # title 1 starts at program 2 of a three-cell chain whose end links to
    # its top: played 2 3 1 2 3 1 ... until the cell-change limit (each
    # program is a part of the title: the navigation reports the title)
    p = Pgc(cells=[cell(0, 9), cell(10, 19), cell(20, 29)], programs=[1, 2, 3], post=[LINK_TOP_PGC])
    folder = build(tmp_path, [p], [[(1, 2), (1, 3), (1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5) + vobus(20, 29, 5), 30)
    ret, res, lines = results(folder)
    assert ret == 0
    assert res[(1, 1)].cells == [1, 2, 3]


def test_an_angle_block_counts_whole(tmp_path):
    # cells 2 and 3 are the two angles of a block: angle 1 plays cell 2
    angle1 = flags.ANGLE_BLOCK | flags.FIRST_IN_BLOCK
    angle2 = flags.ANGLE_BLOCK | flags.LAST_IN_BLOCK
    p = Pgc(cells=[cell(0, 9), cell(10, 19).with_flags(angle1), cell(20, 29).with_flags(angle2), cell(30, 39)],
            programs=[1, 2, 4])
    navs_ = vobus(0, 9, 5) + vobus(10, 19, 5) + vobus(20, 29, 5) + vobus(30, 39, 5)
    folder = build(tmp_path, [p], [[(1, 1), (1, 2), (1, 3)]], navs_, 40)
    ret, res, lines = results(folder)
    assert ret == 0
    assert res[(1, 1)].cells == [1, 2, 3, 4]


def test_a_read_that_fails_fails_the_scan(tmp_path):
    # the title's second cell lies past the end of its title VOBs
    p = Pgc(cells=[cell(0, 9), cell(10, 19)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10)
    v = folder / "VIDEO_TS" / "VTS_01_1.VOB"
    v.write_bytes(v.read_bytes()[:10 * 2048])          # without the home title's sector too
    ret, res, lines = results(folder)
    assert ret < 0 and res == {}
    assert "fail" in lines


def test_corpus_scans_give_the_reference_conversation():
    ref, listing = env_or_skip("DVD_SCAN_REFERENCE", "DVD_SCAN_DISCS")
    differ = []
    for line in pathlib.Path(listing).read_text().splitlines():
        disc, path = line.split("\t")[:2]
        expected = pathlib.Path(ref) / f"{disc}.expected"
        if not expected.exists():
            continue
        d = open_disc(pathlib.Path(path))
        ret, res, lines = scan(d.d, True)
        d.close()
        if lines != expected.read_text().splitlines():
            differ.append(disc)
    assert differ == []


def set_angle(n):
    """SetSTN: SPRM 3 (the angle) = n."""
    return [0x51, 0, 0, 0, 0, 0x80 | n, 0, 0]


def angle_disc(tmp_path, pre):
    # cell 1, then an angle block of two cells (angles 1 and 2)
    first = flags.ANGLE_BLOCK | flags.FIRST_IN_BLOCK
    last = flags.ANGLE_BLOCK | flags.LAST_IN_BLOCK
    p = Pgc(cells=[cell(0, 9), cell(10, 19, flags=first), cell(20, 29, flags=last)], pre=pre)
    return build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5) + vobus(20, 29, 5), 30, angles=[2])


def test_the_angle_the_disc_s_commands_select_is_recorded(tmp_path):
    ret, res, _ = results(angle_disc(tmp_path, [set_angle(2)]))
    assert ret == 0
    assert res[(1, 1)].cells == [1, 2, 3], "the whole angle block is recorded"
    assert res[(1, 1)].angles == [2]


def test_without_an_angle_command_angle_1_is_recorded(tmp_path):
    ret, res, _ = results(angle_disc(tmp_path, []))
    assert ret == 0 and res[(1, 1)].angles == [1]


def test_an_angle_beyond_the_block_plays_its_first_cell(tmp_path):
    # angle 5 of a block of two: the navigator falls back to the block's first cell
    ret, res, _ = results(angle_disc(tmp_path, [set_angle(5)]))
    assert ret == 0 and res[(1, 1)].angles == [1]


def test_a_chain_without_an_angle_block_records_no_angle(tmp_path):
    p = Pgc(cells=[cell(0, 9), cell(10, 19)])
    ret, res, _ = results(build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20))
    assert ret == 0 and res[(1, 1)].angles == []



def test_the_angles_are_kept_in_the_order_the_disc_plays_them(tmp_path):
    # the chain plays itself twice: angle 3 - 1 = 2, then 3 - 2 = 1 (not in number order)
    pre = [[0x73, 0, 0, 0, 0, 1, 0, 0],         # GPRM 0 += 1
           [0x71, 0, 0, 2, 0, 3, 0, 0],         # GPRM 2 = 3
           [0x64, 0, 0, 2, 0, 0, 0, 0],         # GPRM 2 -= GPRM 0
           [0x41, 0, 0, 0, 0, 0x82, 0, 0]]      # SetSTN: angle = GPRM 2
    first = flags.ANGLE_BLOCK | flags.FIRST_IN_BLOCK
    last = flags.ANGLE_BLOCK | flags.LAST_IN_BLOCK
    p = Pgc(cells=[cell(0, 9), cell(10, 19, flags=first), cell(20, 29, flags=last)], pre=pre,
            post=[if_link_pgcn(0, 1, 1)])
    navs = vobus(0, 9, 5) + vobus(10, 19, 5) + vobus(20, 29, 5)
    ret, res, _ = results(build(tmp_path, [p], [[(1, 1)]], navs, 30, angles=[2]))
    assert ret == 0 and res[(1, 1)].angles == [2, 1]


# ---- dvd.menu_revisit (ff_dvd_scan_revisit) ----

def intro_once_disc(tmp_path):
    """A "play the intro once" menu: the first-play chain sets g6 = 1 and goes
    to the VMG menu (an endless still with one button: JumpTT 1); title 1's
    pre-commands go back to the menu while g6 is 1, clearing it, so only the
    second press of the button plays the title."""
    title_pre = [if_goto(6, 0, 4), set_gprm(6, 0), call_ss_vmgm_pgc(1), NOP]
    title = Pgc(cells=[cell(0, 9, 30)], pre=title_pre)
    still = dataclasses.replace(Cell.new(0, 0, 0, 0), still_time=0xff, vob_id_nr=1, cell_nr=1)
    button = Nav(lbn=0, vobu_ea=0, next_vobu=END_OF_CELL, vobu_e_ptm=45000, buttons=[jump_tt(1)])
    menu = Menu(pgcs=[Pgc(cells=[still], entry_id=0x82)], navs=[button], vob_sectors=1)
    return build(tmp_path, [title], [[(1, 1)]], vobus(0, 9, 5), 10, menu=menu,
                 first_play_pre=[set_gprm(6, 1), jump_ss_vmgm_pgc(1)])


def scan_results(folder, revisit):
    d = Disc.folder(folder)
    ret, res, _ = scan(d.d, revisit=revisit)
    d.close()
    return ret, {(r.title, r.pgc): r for r in res}


def test_a_title_only_a_second_visit_to_a_menu_reaches_is_found_with_the_revisit_only(tmp_path):
    folder = intro_once_disc(tmp_path)
    ret, normal = scan_results(folder, False)
    assert ret == 0 and (1, 1) not in normal, "the normal scan never plays title 1 (the menu was explored with g6 = 1)"
    ret, more = scan_results(folder, True)
    assert ret == 0 and (1, 1) in more, "with g6 = 0 the menu is explored again and its button plays title 1"
    assert more[(1, 1)].cells == [1] and more[(1, 1)].name.startswith("r")


def test_the_revisit_keeps_every_recording_of_the_normal_scan_as_it_is(tmp_path):
    folder = intro_once_disc(tmp_path)
    _, normal = scan_results(folder, False)
    _, more = scan_results(folder, True)
    assert all(more[k] == r for k, r in normal.items()), "only additions"


def test_without_a_menu_the_revisit_adds_nothing(tmp_path):
    folder = build(tmp_path, [Pgc(cells=[cell(0, 9, 30)])], [[(1, 1)]], vobus(0, 9, 5), 10)
    _, normal = scan_results(folder, False)
    _, more = scan_results(folder, True)
    assert more == normal
