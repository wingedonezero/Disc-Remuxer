"""Damaged data on a disc: blocks the title reader leaves out (every byte
zero, or no MPEG pack) are said once per range, not once per block: blocks
one after the other of one kind make one range, a gap or another kind
starts the next. Each range names the file and the offset where it lies on
the disc, what it is, and where the title passes it; the title's last line
and the run's summary count them. A built DVD title, the MPEG-2 test stream
with AC-3 audio, one VOBU per GOP (about 0.5 s), every NAV pack kept (the
navigation scan steps through a cell this short VOBU by VOBU): VOBUs 40-47
all zeros after their NAV pack (8 ranges, about 4 s of the title, as a disc
image with a hole of zeros), then VOBU 70 zeros after its NAV pack but for
its last two sectors, which are junk (another kind: a range of its own)."""

import re
import subprocess

from helpers import DATA, PROGRAM
from helpers.disctl import pictures
from helpers.dvdnav import Pgc, build, cell
from synth import dvd
from synth.dvd import S
from test_dvd_streams import put
from test_dvd_titles import ac3

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
BASE = 90000


def damaged_disc(tmp_path):
    pics = pictures(M2V)
    timed = [(a, b, disp, typ, BASE + disp * 3003) for a, b, disp, typ in pics]
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    n = (len(pics) * 3003) // 2880
    audio = [(frames[k % len(frames)], BASE + k * 2880) for k in range(n)]
    sectors, navs = dvd.mux_vobus(M2V, timed, 0, audio, BASE - 3003)
    total = len(sectors)
    p = Pgc(cells=[cell(0, total - 1, len(pics) * 1001 // 30000, last_vobu=navs[-1].lbn)],
            audio_control=bytes([0x80, 0]) + bytes(14))
    folder = build(tmp_path, [p], [[(1, 1)]], navs, total, video_attr=bytes([0x40, 0]), audio_attrs=[ac3(b"en")])
    holes = [(navs[k].lbn + 1, navs[k + 1].lbn - 1, "zero") for k in range(40, 48)]
    holes += [(navs[70].lbn + 1, navs[71].lbn - 3, "zero"), (navs[71].lbn - 2, navs[71].lbn - 1, "junk")]
    for k, sec in enumerate(sectors):
        for a, b, kind in holes:
            if a <= k <= b:
                sec = bytes(S) if kind == "zero" else b"\xff" * S
        put(folder, k, sec)
    return folder, holes


def demux(folder, out, *opts):
    p = subprocess.run([*PROGRAM, *opts, "demux", str(folder), "0", str(out)], capture_output=True, text=True)
    return p.stdout + p.stderr


def test_damaged_blocks_are_said_once_per_range_with_their_place_on_the_disc(tmp_path):
    folder, holes = damaged_disc(tmp_path)
    log = demux(folder, tmp_path / "out", "-v")
    ranges = [line for line in log.splitlines() if re.match(r"(WARNING )?Damaged: VTS_01_1\.VOB", line)]
    assert len(ranges) == len(holes) == 10, log
    for line, (a, b, kind) in zip(ranges, holes):
        n = b - a + 1
        assert f"({n} sector{'s' if n > 1 else ''}," in line, line
        assert ("every byte is zero" if kind == "zero" else "no MPEG data") in line, line
        assert re.search(r"; at \d+:\d\d:\d\d\.\d{3} of the title", line), line
        # the exact bytes in the detail line
        assert f"= VTS_01_1.VOB byte {a * S} to VTS_01_1.VOB byte {(b + 1) * S - 1}" in log, log
    assert "10 of them damaged data" in log, log
    assert "Damaged data left out: 10 ranges" in log, log
    assert log.count("  Title #0: VTS_01_1.VOB") == 10, log


def test_a_range_says_where_the_title_passes_it_and_about_how_much_video_is_missing(tmp_path):
    folder, holes = damaged_disc(tmp_path)
    log = demux(folder, tmp_path / "out")
    # no line per block at the standard level
    assert "is not usable" not in log, log
    found = re.findall(r"^(?:WARNING )?Damaged: .*; at (\d+):(\d\d):(\d\d)\.(\d{3}) of the title(?:, about ([\d.]+) s "
                       r"of video)?$", log, re.M)
    assert len(found) == 10, log
    at = [int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000 for h, m, s, ms, _ in found]
    about = [float(x) if x else 0 for *_, x in found]
    # VOBUs 40-47 have no picture left: their ranges lie where the pictures of
    # VOBU 39 end, the video's disc times jump over eight GOPs there
    pics = pictures(M2V)
    gops = [k for k, (a, b, _, _) in enumerate(pics) if b"\x00\x00\x01\xb8" in M2V[a:b][:64]]
    assert len(set(at[:8])) == 1 and abs(at[0] - gops[40] * 1001 / 30000) < 0.1, (at, gops[40])
    assert all(abs(x - (gops[48] - gops[40]) * 1001 / 30000) < 0.15 for x in about[:8]), about
    # VOBU 70's two ranges: the same place, later
    assert at[8] == at[9] and at[8] > at[0], at


def test_a_title_without_damage_after_one_with_damage_reports_none(tmp_path):
    # the disc is opened once for every title: title 1 plays the damaged
    # cell, title 2 a clean stretch after the damage (VOBUs 72-90); title 2
    # must not report title 1's ranges
    pics = pictures(M2V)
    timed = [(a, b, disp, typ, BASE + disp * 3003) for a, b, disp, typ in pics]
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    n = (len(pics) * 3003) // 2880
    audio = [(frames[k % len(frames)], BASE + k * 2880) for k in range(n)]
    sectors, navs = dvd.mux_vobus(M2V, timed, 0, audio, BASE - 3003)
    total = len(sectors)
    ctl = bytes([0x80, 0]) + bytes(14)
    p1 = Pgc(cells=[cell(0, total - 1, len(pics) * 1001 // 30000, last_vobu=navs[-1].lbn)], audio_control=ctl)
    p2 = Pgc(cells=[cell(navs[72].lbn, navs[91].lbn - 1, 10, last_vobu=navs[90].lbn, cell_nr=2)], audio_control=ctl)
    folder = build(tmp_path, [p1, p2], [[(1, 1)], [(2, 1)]], navs, total, video_attr=bytes([0x40, 0]),
                   audio_attrs=[ac3(b"en")])
    holes = [(navs[k].lbn + 1, navs[k + 1].lbn - 1) for k in range(40, 48)]    # every NAV pack kept
    for k, sec in enumerate(sectors):
        put(folder, k, bytes(S) if any(a <= k <= b for a, b in holes) else sec)
    p = subprocess.run([*PROGRAM, "--set", "titles.min_length=0", "demux", str(folder), "all", str(tmp_path / "out")],
                       capture_output=True, text=True)
    log = p.stdout + p.stderr
    done = re.findall(r"^Title #(\d+): done(.*)$", log, re.M)
    assert len(done) == 2, log
    assert "damaged data" in done[0][1], log
    assert "damaged data" not in done[1][1], log
    assert re.findall(r"^  Title #(\d+): VTS_01_1\.VOB", log, re.M) and \
        set(re.findall(r"^  Title #(\d+): VTS_01_1\.VOB", log, re.M)) == {done[0][0]}, log


def test_a_title_s_warning_count_is_its_warning_lines(tmp_path):
    # the title's last line counts the warning lines above it, as the run's summary counts them
    folder, holes = damaged_disc(tmp_path)
    log = demux(folder, tmp_path / "out")
    section = log[log.index("=== Title 1/1"):log.index("=== Summary")]
    n = int(re.search(r"done with (\d+) warning", section).group(1))
    assert n == sum(1 for line in section.splitlines() if line.startswith("WARNING")), section
    total = int(re.search(r"; (\d+) warnings?, ", log).group(1))
    assert total == sum(1 for line in log.splitlines() if line.startswith("WARNING")), log
