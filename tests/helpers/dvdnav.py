"""libdvdnav on built disc folders: DVD-Video commands, the disc builder
and the recorded playback of a title (dr_nav_trace / dr_nav_play in
tests/bridge/dvdnav_selftest.c)."""

import dataclasses
import struct

from helpers import ffi, lib
from synth import dvd
from synth.dvd import Cell, Pgc, Title, Vts

S = 2048
MAX_EVENTS = 8192


# ---- DVD-Video commands (8 bytes, DVD-Video Book command format) ----

def jump_tt(t):
    """JumpTT: title t."""
    return [0x30, 0x02, 0, 0, 0, t, 0, 0]


def link_pgcn(n):
    """LinkPGCN: program chain n."""
    return [0x20, 0x04, 0, 0, 0, 0, n >> 8, n & 0xff]


def link_pttn(p):
    """LinkPTTN: part of title p (no button)."""
    return [0x20, 0x05, 0, 0, 0, 0, (p >> 8) & 3, p & 0xff]


def link_sub(op):
    """Link with a sub-instruction (0 LinkNoLink, 7 LinkPrevPG), no button."""
    return [0x20, 0x01, 0, 0, 0, 0, 0, op]


def mov_sprm(dst, sprm):
    """Set (mov): GPRM dst = SPRM sprm."""
    return [0x61, 0, 0, dst, 0, 0x80 | sprm, 0, 0]


LINK_NO_LINK = link_sub(0)
LINK_PREV_PG = link_sub(7)


# ---- discs ----

def vobus(first, last, step=1):
    """NAV packs for a cell of sectors first..last with a VOBU every step
    sectors (the last VOBU runs to last)."""
    starts = list(range(first, last + 1, step))
    return dvd.cell_navs(starts, last + 1, 0)


def cell(first, last, secs=1, last_vobu=None, **kw):
    c = Cell.new(first, first if last_vobu is None else last_vobu, last, secs)
    return dataclasses.replace(c, **kw)


def build(tmp_path, pgcs, ptts, navs, vob_sectors, first_play=True, angles=None):
    """A disc folder with one title set: its program chains, its titles
    (ptts: per title the (pgcn, pgn) of each part) and title VOBs of
    vob_sectors sectors holding navs. A last title is added for the
    first-play program chain to jump to (JumpTT), so the navigator starts
    without stopping and without touching the tested chains; first_play
    False leaves the first-play chain out; angles: per title its number of
    angles (default 1)."""
    home = len(pgcs) + 1
    pgcs = list(pgcs) + [Pgc(cells=[Cell.new(vob_sectors, vob_sectors, vob_sectors, 0)])]
    ptts = list(ptts) + [[(home, 1)]]
    navs = list(navs) + [dvd.nav(vob_sectors, 1, None)]
    v = Vts(pgcs=pgcs, ptts=ptts, vobus=sorted(n.lbn for n in navs), vob_sectors=vob_sectors + 1)
    angles = list(angles or []) + [1] * (len(ptts) - len(angles or []))
    titles = [Title(1, k + 1, len(t), angles[k]) for k, t in enumerate(ptts)]
    fp = Pgc(cells=[], pre=[jump_tt(len(titles))]) if first_play else None
    vmg = dvd.vmg_ifo(titles, 1, first_play=fp)
    folder = tmp_path / "disc"
    dvd.write_disc(folder, vmg, [(dvd.vts_ifo(v), dvd.title_vobs(v.vob_sectors, navs))])
    return folder


# ---- playing ----

@dataclasses.dataclass
class Event:
    code: int
    title: int
    vtsn: int
    pgcn: int
    pgn: int
    celln: int
    value: int


def _run(fn, *args):
    ev = ffi.new("dr_nav_event[]", MAX_EVENTS)
    res = ffi.new("dr_nav_result *")
    fn(*args, ev, MAX_EVENTS, res)
    n = min(res.events, MAX_EVENTS)
    return res, [Event(e.code, e.title, e.vtsn, e.pgcn, e.pgn, e.celln, e.value) for e in ev[0:n]]


def trace(folder, title, opts=0, max_steps=20000):
    """A navigator opened by libdvdread on the folder plays title."""
    return _run(lib.dr_nav_trace, str(folder).encode(), title, opts, max_steps)


def trace_disc(disc, title, opts=0, max_steps=20000):
    """A navigator on a disc structure (DVDDisc *, dvd_nav.c) plays title."""
    return _run(lib.tb_dvd_nav_trace, disc, title, opts, max_steps)


def cells(events):
    """(pgcn, celln) of every cell change."""
    return [(e.pgcn, e.celln) for e in events if e.code == lib.DVDNAV_CELL_CHANGE]


def navs(events):
    """The NAV packets' sectors (their PCI's nv_pck_lbn)."""
    return [e.value for e in events if e.code == lib.DVDNAV_NAV_PACKET]


def stopped(res, events):
    return events and events[-1].code == lib.DVDNAV_STOP and res.failures == 0




# ---- the navigation scan (libavformat/dvd_scan.c) ----

from helpers import extern  # noqa: E402

SCAN_LINES = []


@extern
def tb_py_scan_line(opaque, line):
    SCAN_LINES.append(ffi.string(line).decode("latin-1"))


@dataclasses.dataclass
class ScanResult:
    name: str
    title: int
    pgc: int
    cells: list


def scan(disc, with_trace=False):
    """ff_dvd_scan() on a DVDDisc *: (return value, [ScanResult], trace lines)."""
    SCAN_LINES.clear()
    out = ffi.new("DVDScan *")
    ret = lib.tb_dvd_scan(disc, lib.tb_py_scan_line if with_trace else ffi.NULL, out)
    res = [ScanResult(ffi.string(r.name).decode(), r.title, r.pgc, list(ffi.unpack(r.cells, r.nb_cells)))
           for r in out.results[0:out.nb_results]]
    lib.ff_dvd_scan_free(out)
    return ret, res, list(SCAN_LINES)


def titles_text(disc, cell_mode=0, cell_check=0, min_length=0):
    """The navigation scan, then ff_dvd_titles_build() on a DVDDisc *:
    (return value, ff_dvd_titles_dump() text)."""
    dump = ffi.new("char **")
    scan_ret = ffi.new("int *")
    ret = lib.tb_dvd_titles(disc, cell_mode, cell_check, min_length, scan_ret, dump)
    text = ""
    if dump[0] != ffi.NULL:
        text = ffi.string(dump[0]).decode()
        lib.av_free(dump[0])
    return ret, text


def titles(disc, cell_mode=0, cell_check=0, min_length=0):
    """The navigation scan, then ff_dvd_titles_build() on a DVDDisc *:
    (return value, the scan's return value, [dict per title of
    ff_dvd_titles_dump(): name, ttn, vts, pgc, angle, cells, chapters,
    length, real, segments, size, map, scan, short])."""
    dump = ffi.new("char **")
    scan_ret = ffi.new("int *")
    ret = lib.tb_dvd_titles(disc, cell_mode, cell_check, min_length, scan_ret, dump)
    return ret, scan_ret[0], _dump(dump)


def _dump(dump):
    out = []
    if dump[0] != ffi.NULL:
        for line in ffi.string(dump[0]).decode().splitlines():
            w = line.split(" ")
            t = dict(zip(w[2::2], w[3::2]))
            t["short"] = line.endswith(" short")
            out.append(t)
        lib.av_free(dump[0])
    return out


def titles_from(disc, results, cell_mode=0, cell_check=0, min_length=0):
    """ff_dvd_titles_build() on a DVDDisc * with given scan results
    [(name, title, pgc, [cells])] instead of a scan: (return value, [dict per
    title as titles() gives])."""
    res = ffi.new("DVDScanResult[]", max(len(results), 1))
    keep = []
    for k, (name, title, pgc, cells) in enumerate(results):
        res[k].name = name.encode()
        res[k].title = title
        res[k].pgc = pgc
        c = ffi.new("uint8_t[]", list(cells) or [0])
        keep.append(c)
        res[k].cells = c
        res[k].nb_cells = len(cells)
    dump = ffi.new("char **")
    ret = lib.tb_dvd_titles_from(disc, res, len(results), cell_mode, cell_check, min_length, dump)
    return ret, _dump(dump)
