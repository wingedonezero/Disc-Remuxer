"""libavformat/dvdvideo_vobu.c: stepping from VOBU to VOBU through a title
set's title VOBs, on discs built byte by byte. The VOBU address map is used
while it can be trusted; a block it lacks (found by a lookup, or a cell off
the map at open) makes it untrusted, and the NAV packs are used from then
on. The NAV-pack rules: the end of a cell, the VOBU after one that ends its
cell inside a continuing interleaved unit (a blanked NAV pack counts), a NAV
pack pointing at itself; seamless-angle cells step by the map only when
their VOBU chain checks out."""

import dataclasses
import struct

from helpers import ffi, install_log, lib, logged
from helpers.dvd import OpenDisc
from synth.dvd import Cell, Nav, Pgc, Title, Vts, flags, nav

STEP_END_OF_CELL = 0xfffffffe  # DVDVIDEO_VOBU_END_OF_CELL
IN_ILVU = 0x4000  # sml_pbi.category bit: inside an interleaved unit


class TestDisc:
    """An open disc built from one title set with one PGC of cells."""
    __test__ = False

    def __init__(self, folder, cells, vobus, vob_sectors, navs):
        install_log(lib.AV_LOG_DEBUG)
        self.folder = folder
        vts = Vts([Pgc(cells)], [[(1, 1)]], list(vobus), vob_sectors)
        self.d = OpenDisc.build(folder, [Title(1, 1, 1)], [(vts, list(navs))])

    def step(self, celln, sector):
        """(next, len), or None when there is no step."""
        st = ffi.new("DVDVideoVobuStep *")
        cell = lib.ff_dvdvideo_disc_cell(self.d.d, 1, 1, celln)
        assert cell != ffi.NULL
        ret = lib.ff_dvdvideo_vobu_step(self.d.d, 1, cell, sector, st)
        assert ret >= 0, f"error {ret}"
        return (st.next, st.len) if ret == 1 else None


def test_a_trusted_map_steps_plain_cells_without_nav_packs(tmp_path):
    # only the last VOBU has a NAV pack: every step before it is the map's
    d = TestDisc(tmp_path, [Cell.new(0, 20, 29, 10)], [0, 10, 20], 30, [nav(20, 10, None)])
    assert d.step(1, 0) == (10, 10)
    assert d.step(1, 10) == (20, 10)
    assert d.step(1, 20) == (STEP_END_OF_CELL, 10), "the last VOBU by its NAV pack"


def test_a_block_the_map_lacks_makes_it_untrusted(tmp_path):
    # block 15 is a VOBU start the map does not list (NAV packs at 15 and 20)
    d = TestDisc(tmp_path, [Cell.new(0, 20, 29, 10)], [0, 10, 20], 30, [nav(15, 5, 20), nav(20, 10, None)])
    assert d.step(1, 0) == (10, 10), "trusted: no NAV pack needed at 0"
    assert d.step(1, 15) == (20, 5)
    assert logged("block 15 of its title VOBs is not in the VOBU address map")
    # untrusted now: block 0 has no NAV pack, so there is no step from it
    assert d.step(1, 0) is None


def test_a_cell_off_the_map_makes_it_untrusted_at_open(tmp_path):
    # the cell's last VOBU (15) is not in the map: the NAV packs are used
    d = TestDisc(tmp_path, [Cell.new(0, 15, 29, 10)], [0, 10, 20], 30, [nav(0, 15, 15), nav(15, 15, None)])
    assert d.step(1, 0) == (15, 15)


def test_nav_pack_steps(tmp_path):
    def ilvu(n, ilvu_ea):
        return dataclasses.replace(n, category=IN_ILVU, ilvu_ea=ilvu_ea)
    cells = [Cell.new(0, 0, 19, 5).with_flags(flags.INTERLEAVED),
             Cell.new(20, 30, 39, 5).with_flags(flags.INTERLEAVED)]
    navs = [
        # VOBU 0 ends its cell, but its interleaved unit goes on to block 9:
        # the VOBU at 5 follows it
        ilvu(nav(0, 5, None), 9),
        nav(5, 5, None),
        # VOBU 20 ends its cell while its unit ends before it: damaged
        ilvu(nav(20, 10, None), 4),
        # VOBU 30 points at itself: the cell's last VOBU -> end of cell
        Nav(lbn=30, vobu_ea=9, next_vobu=0),
    ]
    d = TestDisc(tmp_path, cells, [0, 5, 20, 30], 40, navs)
    assert d.step(1, 0) == (5, 5), "repaired: the unit goes on"
    assert d.step(2, 20) == (STEP_END_OF_CELL, 10)
    assert logged("the interleaved unit of the VOBU at block 20 of its title VOBs (byte 40960) ends before the VOBU does")
    assert d.step(2, 30) == (STEP_END_OF_CELL, 10), "points at itself at the cell's end"


def test_a_nav_pack_pointing_at_itself_inside_a_cell_steps_by_its_length(tmp_path):
    navs = [Nav(lbn=0, vobu_ea=9, next_vobu=0), nav(10, 10, None)]
    d = TestDisc(tmp_path, [Cell.new(0, 10, 19, 5).with_flags(flags.INTERLEAVED)], [0, 10], 20, navs)
    assert d.step(1, 0) == (10, 10)


def test_a_blanked_nav_pack_after_an_interleaved_unit_counts(tmp_path):
    navs = [dataclasses.replace(nav(0, 5, None), category=IN_ILVU, ilvu_ea=9)]
    d = TestDisc(tmp_path, [Cell.new(0, 0, 9, 5).with_flags(flags.INTERLEAVED)], [0, 5], 10, navs)
    # block 5: the first 32 bytes 0xFF, the PCI and DSI still naming block 5
    vob = tmp_path / "VIDEO_TS" / "VTS_01_1.VOB"
    data = bytearray(vob.read_bytes())
    b = memoryview(data)[5 * 2048:6 * 2048]
    b[:32] = b"\xff" * 32
    b[0x2d:0x31] = struct.pack(">I", 5)
    b[0x40b:0x40f] = struct.pack(">I", 5)
    b[0x40f:0x413] = struct.pack(">I", 4)
    del b
    vob.write_bytes(data)
    assert d.step(1, 0) == (5, 5)
    assert logged("block 5 is a blanked NAV pack, taken as one")


def test_a_block_without_a_nav_pack_gives_no_step(tmp_path):
    d = TestDisc(tmp_path, [Cell.new(0, 0, 9, 5).with_flags(flags.INTERLEAVED)], [0], 10, [])
    assert d.step(1, 0) is None


def test_seamless_angle_cells_use_the_map_only_when_their_chain_checks_out(tmp_path):
    # the map says 0 -> 12; the NAV packs chain 0 -> 10 -> 20 -> 30
    chain = [nav(0, 10, 10), nav(10, 10, 20), nav(20, 10, 30), nav(30, 10, None)]

    def cell():
        return [Cell.new(0, 30, 39, 5).with_flags(flags.SEAMLESS_ANGLE)]
    d = TestDisc(tmp_path / "ok", cell(), [0, 12, 20, 30], 40, chain)
    assert d.step(1, 0) == (12, 12), "chain checks out: the map"
    # VOBU 10 says its successor is at 15, not right after it: no chain
    broken = list(chain)
    broken[1] = nav(10, 10, 15)
    d = TestDisc(tmp_path / "broken", cell(), [0, 12, 20, 30], 40, broken)
    assert d.step(1, 0) == (10, 10), "no chain: the NAV pack"
