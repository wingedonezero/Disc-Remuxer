"""What libdvdread and libdvdnav find in a DVD's data reaches the log as
reports: every information file is read once by libdvdread at load (all its
tables, all its checks), the navigation adds what it finds; each finding once
however often the navigation reads the file again, with the file, the
table, the field's byte offset and the disc's own bytes there. Harmless
findings (fields that must be zero and are not) are detail lines with one
summary line per file, rule breaks warnings; the disc info gives the result.
A built DVD with one title set: VTS_01_0.IFO (and its BUP) changed by hand.
The builder's own stub program chain has a cell of one sector; libdvdread's
rule for the cell address table wants its start below its last sector, so
every built disc has that one finding (BUILT)."""

import json
import subprocess

from helpers import PROGRAM
from helpers.dvdnav import Pgc, build
from synth.dvd import copy_attributes
from test_dvd_streams import put, video
from test_dvd_titles import ac3, c, vobus


BUILT = ("WARNING IFO check: VTS_01_0.IFO C_ADT: c_adt->cell_adr_table[i].start_sector < "
         "c_adt->cell_adr_table[i].last_sector does not hold: kept")


def disc(tmp_path, patch=None):
    p = Pgc(cells=[c(0, 9, 5, 1)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10, audio_attrs=[ac3(b"en")])
    put(folder, 1, video())
    if patch:
        for name in ("VTS_01_0.IFO", "VTS_01_0.BUP"):
            f = folder / "VIDEO_TS" / name
            b = bytearray(f.read_bytes())
            patch(b)
            f.write_bytes(bytes(b))
        # the video manager's copy of the attributes follows, as authoring does
        ifo = (folder / "VIDEO_TS" / "VTS_01_0.IFO").read_bytes()
        for name in ("VIDEO_TS.IFO", "VIDEO_TS.BUP"):
            f = folder / "VIDEO_TS" / name
            f.write_bytes(copy_attributes(f.read_bytes(), [ifo]))
    return folder


def info(folder, *opts):
    p = subprocess.run([*PROGRAM, *opts, "info", str(folder)], capture_output=True, text=True)
    return p.stdout + p.stderr


def check_property(folder):
    p = subprocess.run([*PROGRAM, "--json", "info", str(folder)], capture_output=True, text=True)
    recs = [json.loads(line) for line in p.stdout.splitlines() if line.startswith("{")]
    return next(r for r in recs if r.get("type") == "disc")["properties"]["IFO and navigation check"]


def unused_audio_entry(b):
    # the title set has one audio stream: entry 2 of vts_audio_attr (0x204 + 8) must be zero
    b[0x20c:0x214] = bytes.fromhex("04c1656e00000000")


def test_an_unused_entry_that_is_not_zero_is_one_harmless_finding_with_its_place_and_bytes(tmp_path):
    folder = disc(tmp_path, unused_audio_entry)
    log = info(folder)
    assert "IFO check: VTS_01_0.IFO: 1 harmless finding(s) (see the detail lines): kept" in log, log
    assert "libdvdnav:" not in log and "libdvdread:" not in log, log
    detail = info(folder, "-v")
    line = ("IFO check: VTS_01_0.IFO VTSI_MAT at byte 0x20c (bytes 04 c1 65 6e 00 00 00 00): "
            "vtsi_mat->vts_audio_attr[i] is not zero (must be zero): kept")
    # once, though the navigation reads the file again
    assert detail.count(line) == 1, detail
    # + BUILT; the video manager's copy of the attributes holds the entry too (one more harmless finding,
    # in VIDEO_TS.IFO)
    assert check_property(folder) == "1 finding(s) that break the DVD-Video rules, 2 harmless"


def test_a_value_that_breaks_a_rule_is_a_warning(tmp_path):
    def menu_audio_streams(b):
        b[0x102:0x104] = (2).to_bytes(2, "big")     # nr_of_vtsm_audio_streams: at most 1
    folder = disc(tmp_path, menu_audio_streams)
    log = info(folder)
    assert ("WARNING IFO check: VTS_01_0.IFO VTSI_MAT: vtsi_mat->nr_of_vtsm_audio_streams <= 1 does not hold: kept"
            in log), log
    assert "IFO check: VTS_01_0.IFO: 2 finding(s) that break the DVD-Video rules (above)" in log, log  # + BUILT
    # + BUILT, + the same rule broken in the video manager's copy of the attributes (VIDEO_TS.IFO)
    assert check_property(folder) == "3 finding(s) that break the DVD-Video rules, 0 harmless"


def test_the_built_disc_has_only_its_one_sector_cell_finding(tmp_path):
    folder = disc(tmp_path)
    log = info(folder)
    assert [line for line in log.splitlines() if "check: " in line] == \
        [BUILT, "IFO check: VTS_01_0.IFO: 1 finding(s) that break the DVD-Video rules (above)"], log
    assert check_property(folder) == "1 finding(s) that break the DVD-Video rules, 0 harmless"


# ---- the tables against each other ----

def cadt_entry(b, k):
    """Byte offset of entry k of the title set's cell address table."""
    return int.from_bytes(b[0xe0:0xe4], "big") * 2048 + 8 + 12 * k


def test_a_cell_whose_end_the_cell_address_table_gives_otherwise_is_a_warning(tmp_path):
    def last_piece_end(b):
        at = cadt_entry(b, 0) + 8                   # (VOB 1, cell 1): its last sector
        b[at:at + 4] = (8).to_bytes(4, "big")
    log = info(disc(tmp_path, last_piece_end))
    assert "WARNING IFO check: VTS_01_0.IFO VTS_C_ADT at byte" in log, log
    assert "program chain 1 cell 1 ends at sector 9, its last piece in the cell address table at 8" in log, log


def test_a_cell_missing_from_the_cell_address_table_is_a_warning(tmp_path):
    def other_vob(b):
        at = cadt_entry(b, 0)
        b[at:at + 2] = (7).to_bytes(2, "big")       # its entry names VOB 7
    log = info(disc(tmp_path, other_vob))
    assert "program chain 1 cell 1 (VOB id 1, cell id 1) is not in the cell address table" in log, log


def vmg_copy(folder, patch):
    for name in ("VIDEO_TS.IFO", "VIDEO_TS.BUP"):
        f = folder / "VIDEO_TS" / name
        b = bytearray(f.read_bytes())
        at = int.from_bytes(b[0xd0:0xd4], "big") * 2048
        patch(b, at + int.from_bytes(b[at + 8:at + 12], "big"))     # title set 1's entry
        f.write_bytes(bytes(b))


def test_the_video_manager_s_copy_of_a_title_set_s_attributes_must_be_its_own(tmp_path):
    folder = disc(tmp_path)
    def video(b, entry):
        b[entry + 8 + 0x100] ^= 0x40                # vts_video_attr (VTSI_MAT 0x200): another aspect
    vmg_copy(folder, video)
    log = info(folder)
    assert "WARNING IFO check: VIDEO_TS.IFO VMG_VTS_ATRT at byte" in log, log
    assert "title set 1's attributes differ from those in its own IFO, first at VTSI_MAT byte 0x200" in log, log
    folder = disc(tmp_path)
    def category(b, entry):
        b[entry + 7] = 1                            # vts_category
    vmg_copy(folder, category)
    log = info(folder)
    assert "title set 1's category is 00000001 in the title set attribute table, 00000000 in its own IFO" in log, log
