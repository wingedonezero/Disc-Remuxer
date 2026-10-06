"""IFO tables that break one rule each, on a small built disc (DVD-Video
Book layout, field names as libdvdread's ifo_types.h): missing or short
tables, tables whose entries run past their recorded end, program chains
whose offsets or command counts are out of range. Each case is one change to
the same valid disc.

With DVDVIDEO_IFO_CASES=<folder>, test_write_ifo_cases writes every case as
a disc folder <folder>/<case> (to run other DVD software on the same
inputs)."""

import copy
import pathlib
import struct

from helpers import LOG, install_log, lib
from helpers.disc import env_or_skip
from helpers.dvd import titles
from synth.dvd import Cell, Pgc, Title, Vts, cell_navs, time, title_vobs, vmg_ifo, vts_ifo, write_disc

S = 2048


def rd32(d, at):
    return struct.unpack_from(">I", d, at)[0]


def wr32(d, at, v):
    d[at:at + 4] = struct.pack(">I", v)


def wr16(d, at, v):
    d[at:at + 2] = struct.pack(">H", v)


def table(d, ptr):
    """The byte where the table named by the sector pointer at ptr starts."""
    return rd32(d, ptr) * S


# VMGI_MAT / VTSI_MAT pointers.
VMG_TT_SRPT = 0xc4
VMG_VTS_ATRT = 0xd0
VMG_NR_OF_TITLE_SETS = 0x3e
VTS_PTT_SRPT = 0xc8
VTS_PGCIT = 0xcc
VTSM_VOBU_ADMAP = 0xdc
VTS_C_ADT = 0xe0
VTS_VOBU_ADMAP = 0xe4


class Disc:
    """A disc as files: the VMG and the title sets [(IFO, title VOBs)]."""

    def __init__(self, vmg, sets):
        self.vmg = vmg
        self.sets = sets

    def write(self, folder):
        write_disc(folder, self.vmg, self.sets)


def base():
    """The valid disc: one title set with three titles of one program chain
    each (cells of 4, 6 and 8 VOBUs, 0.5 s per VOBU)."""
    navs = []
    t0 = [0]

    def cell(first, vobus):
        starts = [first + 10 * k for k in range(vobus)]
        navs.extend(cell_navs(starts, first + 10 * vobus, t0[0]))
        t0[0] += 45000 * vobus
        c = Cell.new(first, first + 10 * (vobus - 1), first + 10 * vobus - 1, vobus // 2)
        c.playback_time = time(vobus // 2)
        return c
    a, b, c = cell(0, 4), cell(40, 6), cell(100, 8)
    vobus = sorted(n.lbn for n in navs)

    def pgc(cells, title):
        return Pgc(cells, entry_id=0x80 | title)
    vts = Vts([pgc([a], 1), pgc([b], 2), pgc([c], 3)], [[(1, 1)], [(2, 1)], [(3, 1)]], vobus, 180)
    ts = [Title(1, 1, 1), Title(1, 2, 1), Title(1, 3, 1)]
    return Disc(vmg_ifo(ts, 1), [(vts_ifo(vts), title_vobs(vts.vob_sectors, navs))])


def _tt_srpt_short(d):
    wr32(d.vmg, table(d.vmg, VMG_TT_SRPT) + 4, 8 + 12 * 2 - 1)


def _tt_srpt_last_byte_0(d):
    wr32(d.vmg, table(d.vmg, VMG_TT_SRPT) + 4, 0)


def _title_bad_set(d):
    d.vmg[table(d.vmg, VMG_TT_SRPT) + 8 + 12 * 2 + 6] = 5


def _set_start_mismatch(d):
    t = table(d.vmg, VMG_TT_SRPT)
    wr32(d.vmg, t + 8 + 8, 100)
    wr32(d.vmg, t + 8 + 12 + 8, 200)


def _c_adt_short(d):
    wr32(d.sets[0][0], table(d.sets[0][0], VTS_C_ADT) + 4, 3)


def _vobu_admap_short(d):
    wr32(d.sets[0][0], table(d.sets[0][0], VTS_VOBU_ADMAP), 2)


def _vtsm_vobu_admap_unloadable(d):
    wr32(d.sets[0][0], VTSM_VOBU_ADMAP, rd32(d.sets[0][0], VTS_PTT_SRPT))


def _ptt_count_large(d):
    wr16(d.sets[0][0], table(d.sets[0][0], VTS_PTT_SRPT), 4)


def _ptt_misaligned(d):
    ifo = d.sets[0][0]
    t = table(ifo, VTS_PTT_SRPT)
    wr32(ifo, t + 8 + 4, rd32(ifo, t + 8 + 4) + 2)


def _ptt_last_byte_plus_1(d):
    ifo = d.sets[0][0]
    t = table(ifo, VTS_PTT_SRPT)
    wr32(ifo, t + 4, rd32(ifo, t + 4) + 1)


def _pgc2(ifo):
    t = table(ifo, VTS_PGCIT)
    return t, t + rd32(ifo, t + 8 + 8 + 4)


def _cells_offset_0(d):
    _, p = _pgc2(d.sets[0][0])
    wr16(d.sets[0][0], p + 0xe8, 0)


def _129_commands(d):
    # the command table goes at the end of the PGCIT (its sector has room)
    ifo = d.sets[0][0]
    t, p = _pgc2(ifo)
    end = t + rd32(ifo, t + 4) + 1
    tbl = (end + 3) // 4 * 4
    length = 8 + 8 * 129
    assert tbl + length <= t + S, "room for the table"
    wr16(ifo, p + 0xe4, tbl - p)
    wr16(ifo, tbl, 129)
    wr16(ifo, tbl + 6, length - 1)
    wr32(ifo, t + 4, tbl + length - t - 1)


def _command_table_past_pgcit(d):
    ifo = d.sets[0][0]
    t, p = _pgc2(ifo)
    end = t + rd32(ifo, t + 4) + 1
    wr16(ifo, p + 0xe4, end + 64 - p)


def _duplicate_of_unreadable(d):
    ifo = d.sets[0][0]
    t = table(ifo, VTS_PGCIT)
    wr32(ifo, t + 8 + 8 + 4, 0x8000)
    wr32(ifo, t + 8 + 16 + 4, 0x8000)


def _bad_signature(d):
    d.sets[0][0][0] = ord("X")


def cases():
    """Every case: name, what it breaks, the change."""
    return [
        ("base", "nothing (the valid disc)", lambda d: None),
        # ---- VMG ----
        ("vmg_atrt_sector_0", "VMG: VTS_ATRT pointer 0", lambda d: wr32(d.vmg, VMG_VTS_ATRT, 0)),
        ("vmg_tt_srpt_short", "TT_SRPT last_byte covers 2 of its 3 entries", _tt_srpt_short),
        ("vmg_tt_srpt_last_byte_0", "TT_SRPT last_byte 0", _tt_srpt_last_byte_0),
        ("vmg_title_bad_set", "title 3 names title set 5 (the disc has 1)", _title_bad_set),
        ("vmg_100_title_sets", "vmg_nr_of_title_sets 100", lambda d: wr16(d.vmg, VMG_NR_OF_TITLE_SETS, 100)),
        ("vmg_set_start_mismatch", "titles 1 / 2 give title set 1 the start sectors 100 / 200", _set_start_mismatch),
        # ---- VTS tables ----
        ("vts_c_adt_sector_0", "VTS_C_ADT pointer 0", lambda d: wr32(d.sets[0][0], VTS_C_ADT, 0)),
        ("vts_c_adt_short", "VTS_C_ADT last_byte 3 (shorter than its header)", _c_adt_short),
        ("vts_vobu_admap_sector_0", "VTS_VOBU_ADMAP pointer 0", lambda d: wr32(d.sets[0][0], VTS_VOBU_ADMAP, 0)),
        ("vts_vobu_admap_short", "VTS_VOBU_ADMAP last_byte 2 (no entry)", _vobu_admap_short),
        ("vtsm_vobu_admap_unloadable", "VTSM_VOBU_ADMAP names a sector whose length runs past the IFO",
         _vtsm_vobu_admap_unloadable),
        ("vts_ptt_count_large", "VTS_PTT_SRPT says 4 titles, holds 3", _ptt_count_large),
        ("vts_ptt_misaligned", "VTS_PTT_SRPT: title 2's offset 2 bytes into its entry", _ptt_misaligned),
        ("vts_ptt_last_byte_plus_1", "VTS_PTT_SRPT last_byte one past its end", _ptt_last_byte_plus_1),
        ("vts_pgcit_sector_0", "VTS_PGCIT pointer 0", lambda d: wr32(d.sets[0][0], VTS_PGCIT, 0)),
        # ---- program chains ----
        ("pgc_cells_offset_0", "PGC 2: cell_playback_offset 0 with 1 cell", _cells_offset_0),
        ("pgc_129_commands", "PGC 2: a command table of 129 pre commands", _129_commands),
        ("pgc_command_table_past_pgcit", "PGC 2: command table offset past the PGCIT's end",
         _command_table_past_pgcit),
        ("pgc_duplicate_of_unreadable", "PGCs 2 and 3 share a start past the end of the IFO",
         _duplicate_of_unreadable),
        # ---- the IFO itself ----
        ("vts_ifo_bad_signature", "VTS_01_0.IFO signature broken, its BUP intact", _bad_signature),
    ]


def write_case(folder, name, change):
    """Writes case name with its IFO also as the BUP; the IFO-signature case
    keeps an intact BUP."""
    good = base()
    d = copy.deepcopy(good)
    change(d)
    d.write(folder)
    if name == "vts_ifo_bad_signature":
        (folder / "VIDEO_TS" / "VTS_01_0.BUP").write_bytes(good.sets[0][0])


def test_write_ifo_cases():
    """Writes the cases as disc folders for other DVD software."""
    out = pathlib.Path(env_or_skip("DVDVIDEO_IFO_CASES", need="a folder to write the cases into"))
    for name, what, change in cases():
        write_case(out / name, name, change)
        print(f"{name}\t{what}")


def test_every_case_differs_from_the_valid_disc():
    good = base()
    for name, _, change in cases()[1:]:
        d = copy.deepcopy(good)
        change(d)
        assert d.vmg != good.vmg or d.sets != good.sets, f"{name} changes nothing"


def plan_of(folder, name):
    """The title plan of case name through the whole path (source, disc, the
    navigation scan, titles), cell mode automatic, no minimum length."""
    change = next(c for n, _, c in cases() if n == name)
    write_case(folder / name, name, change)
    return titles(folder / name, attempts=1, cell_mode=lib.DVDVIDEO_CELLS_AUTO,
                  title_order=lib.DVDVIDEO_ORDER_AUTO, min_length=0)


def test_a_disc_without_a_first_play_program_chain_or_menus_fails_the_scan_not_the_program(tmp_path):
    # the navigator has no program chain to start with: the scan fails (the
    # navigator stops), the titles come from the IFOs alone
    p = plan_of(tmp_path, "base")
    assert not isinstance(p, int)
    assert p.scan is not None and p.scan.failure, "the scan fails"
    assert [t.name for t in p.titles] == ["1", "2", "3"]


def outcome(folder, name):
    """The selected titles, the findings (other than "title") and the log
    lines of case name."""
    install_log(lib.AV_LOG_VERBOSE)
    p = plan_of(folder, name)
    assert not isinstance(p, int), f"{name}: {p}"
    ts = [t.name for t in p.titles if t.not_selected == 0]
    events = [f"{e[0]} {e[1]}" for e in p.events if e[0] != "title"]
    return ts, events, list(LOG)


def test_every_case_gives_its_titles_and_findings(tmp_path):
    # (case, selected titles, findings, a log line that must appear)
    want = [
        ("base", ["1", "2", "3"], [], ""),
        # VTS_ATRT is not needed to find or play titles
        ("vmg_atrt_sector_0", ["1", "2", "3"], [], "no readable title set attribute table (VTS_ATRT)"),
        # entries past the title search table's recorded end are read
        ("vmg_tt_srpt_short", ["1", "2", "3"], [], "gives 3 titles, its recorded end (last_byte 31) covers 2"),
        ("vmg_tt_srpt_last_byte_0", ["1", "2", "3"], [], ""),
        # a title naming a missing title set: left out, the others stay
        ("vmg_title_bad_set", ["1", "2"], ["title-set-invalid 3\t5\t1"], ""),
        # more than 99 title sets: the first 99 are used
        ("vmg_100_title_sets", ["1", "2", "3"], [], "gives 100 title sets"),
        ("vmg_set_start_mismatch", ["1", "2", "3"],
         ["title-set-start-mismatch 1\t200\t100", "title-set-start-mismatch 1\t0\t100",
          "title-set-start-mismatch 1\t6\t100"], ""),
        # the cell address table is not needed
        ("vts_c_adt_sector_0", ["1", "2", "3"], [], "no readable cell address table (VTS_C_ADT)"),
        ("vts_c_adt_short", ["1", "2", "3"], [], "no readable cell address table (VTS_C_ADT)"),
        # no VOBU address map at all: the title set cannot be used
        ("vts_vobu_admap_sector_0", [],
         ["title-set-missing 1", "title-set-missing 2", "title-set-missing 3"], ""),
        # a map that cannot be read: not trusted, VOBUs from the NAV packs
        ("vts_vobu_admap_short", ["1", "2", "3"], [], "its VOBU address map (VTS_VOBU_ADMAP) cannot be read"),
        ("vtsm_vobu_admap_unloadable", ["1", "2", "3"], [],
         "its menu VOBU address map (VTSM_VOBU_ADMAP) cannot be read"),
        # part-of-title tables: our reading keeps every title whose own entries are sound
        ("vts_ptt_count_large", ["1", "2", "3"], [], ""),
        ("vts_ptt_misaligned", ["1", "3"], [], ""),
        ("vts_ptt_last_byte_plus_1", ["1", "2", "3"], ["ifo-corrupt VTS_01_0.IFO\t2052"], ""),
        ("vts_pgcit_sector_0", [], ["title-set-missing 1", "title-set-missing 2", "title-set-missing 3"], ""),
        # program chains that cannot be used: their titles are left out
        ("pgc_cells_offset_0", ["1", "3"], ["ptt-unresolved 1\t2\t2\t1"], ""),
        ("pgc_129_commands", ["1", "3"], ["ptt-unresolved 1\t2\t2\t0"], ""),
        ("pgc_command_table_past_pgcit", ["1", "2", "3"], [], ""),
        ("pgc_duplicate_of_unreadable", ["1"], ["ptt-unresolved 1\t2\t2\t0", "ptt-unresolved 1\t3\t3\t0"], ""),
        ("vts_ifo_bad_signature", ["1", "2", "3"], [], ""),
    ]
    assert len(want) == len(cases()), "every case has its expectation"
    failed = []
    for name, ts, events, line in want:
        t, e, log = outcome(tmp_path, name)
        if t != ts or e != events or (line and not any(line in entry for entry in log)):
            failed.append(f"{name}: titles {t} findings {e}")
    assert not failed, failed
