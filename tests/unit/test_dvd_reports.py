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
    assert check_property(folder) == "1 finding(s) that break the DVD-Video rules, 1 harmless"  # + BUILT


def test_a_value_that_breaks_a_rule_is_a_warning(tmp_path):
    def menu_audio_streams(b):
        b[0x102:0x104] = (2).to_bytes(2, "big")     # nr_of_vtsm_audio_streams: at most 1
    folder = disc(tmp_path, menu_audio_streams)
    log = info(folder)
    assert ("WARNING IFO check: VTS_01_0.IFO VTSI_MAT: vtsi_mat->nr_of_vtsm_audio_streams <= 1 does not hold: kept"
            in log), log
    assert "IFO check: VTS_01_0.IFO: 2 finding(s) that break the DVD-Video rules (above)" in log, log  # + BUILT
    assert check_property(folder) == "2 finding(s) that break the DVD-Video rules, 0 harmless"


def test_the_built_disc_has_only_its_one_sector_cell_finding(tmp_path):
    folder = disc(tmp_path)
    log = info(folder)
    assert [line for line in log.splitlines() if "check: " in line] == \
        [BUILT, "IFO check: VTS_01_0.IFO: 1 finding(s) that break the DVD-Video rules (above)"], log
    assert check_property(folder) == "1 finding(s) that break the DVD-Video rules, 0 harmless"
