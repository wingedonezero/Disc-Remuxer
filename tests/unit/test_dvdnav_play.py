"""Our libdvdnav's behaviour on built disc folders (libs/README.md): a
navigator plays a title and every event is recorded (dr_nav_trace in
tests/bridge/dvdnav_selftest.c). Each test names what upstream libdvdnav
7.0.0 does instead; some of those would hang or abort the test process
(an endless link loop, an assert), which the test notes."""

from helpers import lib
from helpers.dvdnav import (LINK_NO_LINK, LINK_PREV_PG, Pgc, build, cell, cells, dvd, ffi, link_pgcn,
                            link_pttn, mov_sprm, navs, stopped, struct, trace, vobus)


def two_cells(tmp_path, **kw):
    """Title 1: one program chain of two cells (sectors 0-9, 10-19)."""
    p = Pgc(cells=[cell(0, 9), cell(10, 19)], programs=[1, 2])
    return build(tmp_path, [p], [[(1, 1), (1, 2)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20, **kw)


# ---- opening, copies ----

def test_a_title_plays_its_cells_and_stops(tmp_path):
    res, ev = trace(two_cells(tmp_path), 1)
    assert res.status == 0
    assert cells(ev) == [(1, 1), (1, 2)]
    assert navs(ev) == [0, 5, 10, 15]
    assert stopped(res, ev)
    assert res.data_blocks == 20 - 4


def test_the_navigator_opens_on_application_file_callbacks(tmp_path):
    # dvdnav_open_files (new): the same playback, every file read through the callbacks
    folder = two_cells(tmp_path)
    res, ev = trace(folder, 1, lib.DR_NAV_FILES)
    assert res.status == 0 and res.files_opened > 0
    assert (cells(ev), navs(ev)) == (cells(trace(folder, 1)[1]), navs(trace(folder, 1)[1]))


def test_a_copy_made_before_playback_starts_plays_like_the_original(tmp_path):
    # upstream dvdnav_dup fails before a program chain is set (and opens the
    # VTSI again); ours copies in any state, sharing the IFO handles
    folder = two_cells(tmp_path)
    res, ev = trace(folder, 1, lib.DR_NAV_COPY)
    assert res.status == 0, "the copy was made"
    assert cells(ev) == [(1, 1), (1, 2)] and stopped(res, ev)


def test_a_disc_without_a_first_play_chain_or_menus_fails_the_navigator(tmp_path):
    # upstream plays a NULL program chain (crash); ours records the failure
    res, ev = trace(two_cells(tmp_path, first_play=False), 1)
    assert res.status == -2, "title play fails"
    assert res.failures == 1


# ---- what is played ----

def test_title_play_follows_the_jump_of_a_pre_command_only_when_asked(tmp_path):
    # PGC 1 (the title's) jumps to PGC 2 in its pre commands
    p1 = Pgc(cells=[cell(0, 9)], pre=[link_pgcn(2)])
    p2 = Pgc(cells=[cell(10, 19)])
    folder = build(tmp_path, [p1, p2], [[(1, 1)]], vobus(0, 9) + vobus(10, 19), 20)
    assert cells(trace(folder, 1)[1]) == [(1, 1)], "default (upstream): the jump is ignored"
    res, ev = trace(folder, 1, lib.DR_NAV_FOLLOW_JUMPS)
    assert cells(ev) == [(2, 1)], "followed, as a player does"
    assert ev[0].title == -1, "PGC 2 is no part of a title"


def test_positions_are_reported_in_a_chain_no_part_of_title_leads_to(tmp_path):
    # dvdnav_current_title_program2 (new): upstream's position call fails there
    p1 = Pgc(cells=[cell(0, 9)], post=[link_pgcn(2)])
    p2 = Pgc(cells=[cell(10, 19)])
    folder = build(tmp_path, [p1, p2], [[(1, 1)]], vobus(0, 9) + vobus(10, 19), 20)
    res, ev = trace(folder, 1)
    changes = [e for e in ev if e.code == lib.DVDNAV_CELL_CHANGE]
    assert [(e.title, e.vtsn, e.pgcn, e.pgn, e.celln) for e in changes] == [(1, 1, 1, 1, 1), (-1, 1, 2, 1, 1)]


def test_endless_links_without_anything_to_play_stop_the_navigator(tmp_path):
    # PGC 1 has no cells; its post command links to itself. Upstream loops
    # forever inside one call (this test would hang); ours stops after 2500 links.
    p1 = Pgc(cells=[], post=[link_pgcn(1)])
    folder = build(tmp_path, [p1], [[(1, 1)]], [], 0)
    res, ev = trace(folder, 1, max_steps=100)
    assert res.steps < 100
    assert ev and ev[-1].code == lib.DVDNAV_STOP
    assert res.failures == 0, "a guard, not a broken assumption"


def test_a_cell_command_that_links_to_nowhere_continues_with_the_next_cell(tmp_path):
    # upstream: the cell walk stalls (the next cell is never reached)
    p = Pgc(cells=[cell(0, 9, cell_cmd_nr=1), cell(10, 19)], cell_cmds=[LINK_NO_LINK])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9) + vobus(10, 19), 20)
    res, ev = trace(folder, 1, max_steps=2000)
    assert cells(ev) == [(1, 1), (1, 2)]
    assert stopped(res, ev)


def test_short_cells_are_not_turned_into_stills(tmp_path):
    # a 2-sector cell of 5 s whose last VOBU starts at its last sector:
    # upstream invents a 5 s still for it
    p = Pgc(cells=[cell(0, 1, secs=5, last_vobu=1), cell(2, 11)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 1) + vobus(2, 11, 5), 12)
    res, ev = trace(folder, 1)
    assert [e for e in ev if e.code == lib.DVDNAV_STILL_FRAME] == []
    assert cells(ev) == [(1, 1), (1, 2)]


def test_a_real_cell_still_is_kept(tmp_path):
    p = Pgc(cells=[cell(0, 9, still_time=3), cell(10, 19)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9) + vobus(10, 19), 20)
    res, ev = trace(folder, 1)
    assert [e.value for e in ev if e.code == lib.DVDNAV_STILL_FRAME] == [3]


def test_link_prev_pg_at_the_first_program_goes_to_the_previous_chain(tmp_path):
    # the title starts in PGC 2 (previous chain: PGC 1); its first cell's
    # command is LinkPrevPG. Upstream asserts (aborts the process).
    p1 = Pgc(cells=[cell(0, 9)])
    p2 = Pgc(cells=[cell(10, 19, cell_cmd_nr=1)], cell_cmds=[LINK_PREV_PG], prev_pgc_nr=1)
    folder = build(tmp_path, [p1, p2], [[(2, 1)]], vobus(0, 9) + vobus(10, 19), 20)
    res, ev = trace(folder, 1)
    assert cells(ev) == [(2, 1), (1, 1)]
    assert stopped(res, ev)


def test_the_disc_s_tests_of_system_registers_are_recorded(tmp_path):
    # SPRM_flags (new): the pre command reads SPRM 20 (region)
    p = Pgc(cells=[cell(0, 9)], pre=[mov_sprm(0, 20)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9), 10)
    res, ev = trace(folder, 1)
    assert res.sprm_flags == 1 << 20


# ---- random and shuffle program chains ----

def vmgi_seed(folder):
    """The random order's state as vm_reset seeds it from the VMGI."""
    d = (folder / "VIDEO_TS" / "VIDEO_TS.IFO").read_bytes()
    state = ffi.new("uint32_t *", 0)
    for at in (0x0c, 0x1c, 0xd8):  # vmg_last_sector, vmgi_last_sector, vmgm_vobu_admap
        lib.vm_rand_seed(state, struct.unpack(">I", d[at:at + 4])[0])
    return state[0]


def random_disc(tmp_path, mode, entry):
    """PGC 1 (one cell) is the title's; at its end entry (a post command)
    leads into PGC 2: four one-cell programs played in random order (mode =
    pg_playback_mode). PGC 2 is also parts 2-5 of the title."""
    p1 = Pgc(cells=[cell(0, 9)], post=[entry])
    p2 = Pgc(cells=[cell(10 + 10 * k, 19 + 10 * k) for k in range(4)], programs=[1, 2, 3, 4],
             pg_playback_mode=mode)
    navs_ = vobus(0, 9) + [n for k in range(4) for n in vobus(10 + 10 * k, 19 + 10 * k)]
    return build(tmp_path, [p1, p2], [[(1, 1), (2, 1), (2, 2), (2, 3), (2, 4)]], navs_, 50)


def expected_random_programs(state, count, n=4):
    """Programs a random chain entered with a new cycle plays: element k of
    the order of state (advanced once at the entry) for k = 0..count-1; the
    run ends early after the last program (n), whose next cell leaves the
    chain."""
    s = ffi.new("uint32_t *", state)
    lib.vm_rand_next(s)
    out = []
    for k in range(count):
        pg = 1 + lib.vm_rand_shuffle(s[0], n, k)
        out.append(pg)
        if pg == n:
            break
    return out


def test_a_random_chain_plays_its_programs_in_the_disc_s_own_order(tmp_path):
    # upstream picks the first program with rand() seeded from the clock (a
    # different order every run) and plays the rest in sequence
    folder = random_disc(tmp_path, 0x03, link_pgcn(2))
    expected = expected_random_programs(vmgi_seed(folder), 4)
    res, ev = trace(folder, 1)
    played = [c for p, c in cells(ev) if p == 2]
    assert played == expected
    assert cells(trace(folder, 1)[1]) == cells(ev), "the same every time"


def test_shuffle_is_played_like_random(tmp_path):
    folder = random_disc(tmp_path, 0x83, link_pgcn(2))
    played = [c for p, c in cells(trace(folder, 1)[1]) if p == 2]
    assert played == expected_random_programs(vmgi_seed(folder), 4)


def test_a_part_link_into_a_random_chain_starts_a_new_random_cycle(tmp_path):
    # LinkPTTN 3 names program 2 of PGC 2; upstream plays program 2
    folder = random_disc(tmp_path, 0x03, link_pttn(3))
    played = [c for p, c in cells(trace(folder, 1)[1]) if p == 2]
    assert played[0] == expected_random_programs(vmgi_seed(folder), 1)[0]


# ---- the playback clock ----

def test_the_playback_clock_counts_cell_time_and_skipped_stills(tmp_path):
    # two cells of two VOBUs each, NAV cell times 0 s and 1 s, the first
    # cell ending in a 3 s still: 2 x 1 s of cells + 3 s of still
    def timed(first, last):
        n = vobus(first, last, 5)
        n[1].c_eltm = (0, 0, 1, 0)
        return n
    p = Pgc(cells=[cell(0, 9, still_time=3), cell(10, 19)])
    folder = build(tmp_path, [p], [[(1, 1)]], timed(0, 9) + timed(10, 19), 20)
    res, ev = trace(folder, 1)
    unit = 512
    assert res.clock == (2 * (90000 // unit) + (3 * 5625 >> 5)) * unit


# ---- navigation-only mode ----

def long_cell(tmp_path, fake_last=False, button_first=False, fwda=None):
    """One cell of 0x1200 sectors with a VOBU every 0x10 sectors (last VOBU
    at 0x11f0)."""
    n = vobus(0, 0x11ff, 0x10)
    if fake_last:
        n[-1].pci_lbn = 0x1234  # a NAV packet naming another sector
    if button_first:
        n[0].hli_ss = 1
    if fwda is not None:
        # the first forward pointer: fwda sectors ahead, or "none" (0x3fffffff)
        # where that leaves the cell's last VOBU behind
        for x in n:
            x.fwda = [fwda if x.lbn + fwda <= 0x11f0 else 0x3fffffff]
    p = Pgc(cells=[cell(0, 0x11ff, secs=100, last_vobu=0x11f0)])
    return build(tmp_path, [p], [[(1, 1)]], n, 0x1200)


def test_navigation_only_skips_data_and_jumps_to_the_cell_s_last_vobu(tmp_path):
    folder = long_cell(tmp_path)
    res, ev = trace(folder, 1)
    assert len(navs(ev)) == 0x120 and res.data_blocks == 0x1200 - 0x120, "flag off: every block"
    res, ev = trace(folder, 1, lib.DR_NAV_NAV_ONLY)
    assert navs(ev) == [0, 0x11f0]
    assert res.data_blocks == 0 and res.empty_blocks > 0
    assert stopped(res, ev)


def test_navigation_only_never_jumps_to_a_fake_nav_packet(tmp_path):
    res, ev = trace(long_cell(tmp_path, fake_last=True), 1, lib.DR_NAV_NAV_ONLY)
    assert res.data_blocks == 0, "navigation only"
    assert len(navs(ev)) == 0x120, "every VOBU in turn"
    assert navs(ev)[-1] == 0x1234


def test_navigation_only_does_not_jump_from_a_vobu_with_buttons(tmp_path):
    res, ev = trace(long_cell(tmp_path, button_first=True), 1, lib.DR_NAV_NAV_ONLY)
    assert navs(ev) == [0, 0x10, 0x11f0]


def test_navigation_only_follows_the_forward_pointers_when_the_last_vobu_cannot_be_taken(tmp_path):
    # the last VOBU is fake: the DSI's first forward pointer (0x40 sectors)
    # is taken while it has one, then the VOBUs follow one by one
    res, ev = trace(long_cell(tmp_path, fake_last=True, fwda=0x40), 1, lib.DR_NAV_NAV_ONLY)
    assert navs(ev) == list(range(0, 0x11c1, 0x40)) + [0x11d0, 0x11e0, 0x1234]
