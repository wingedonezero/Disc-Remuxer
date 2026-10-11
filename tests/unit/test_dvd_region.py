"""The disc's regions in the disc info ("Region"): VIDEO_TS.IFO's
vmg_category (VMGI_MAT 0x22, after the specification version at 0x21,
big-endian), bits 16-23 (byte 0x23), a set bit = not for that region (1-8).
A built DVD whose VIDEO_TS.IFO (and BUP) get the mask byte, and the
specification version 1.1 (0x11) real discs have, which must not count."""

import json
import subprocess

import pytest

from helpers import PROGRAM
from test_dvd_reports import disc


def region(tmp_path, mask):
    folder = disc(tmp_path)
    for name in ("VIDEO_TS.IFO", "VIDEO_TS.BUP"):
        f = folder / "VIDEO_TS" / name
        b = bytearray(f.read_bytes())
        b[0x21] = 0x11                  # specification_version
        b[0x23] = mask
        f.write_bytes(bytes(b))
    p = subprocess.run([*PROGRAM, "--json", "info", str(folder)], capture_output=True, text=True)
    recs = [json.loads(line) for line in p.stdout.splitlines() if line.startswith("{")]
    return next(r for r in recs if r.get("type") == "disc")["properties"]["Region"]


@pytest.mark.parametrize("mask, want", [
    (0x00, "all (region free)"),
    (0xfe, "1"),
    (0xfd, "2"),
    (0xf5, "2, 4"),
    (0x7f, "8"),
    (0xff, "none (every region locked out)"),
])
def test_the_region_mask_says_which_regions_the_disc_is_for(tmp_path, mask, want):
    assert region(tmp_path, mask) == want
