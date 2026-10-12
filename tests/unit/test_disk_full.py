"""A full disk: a stream file that cannot be written fails its title with the
file's name and the reason (no space left), and the rip stops there, as every
title after it would fail the same way. /dev/full stands in for the full disk
(every write to it fails with ENOSPC): the video file's name is a link to it.
A built DVD title (the damage test's disc without its holes)."""

import os
import subprocess

from helpers import PROGRAM
from helpers.dvdnav import Pgc, build, cell
from helpers.disctl import pictures
from synth import dvd
from test_disc_damage import AC3, BASE, M2V
from test_dvd_streams import put
from test_dvd_titles import ac3


def small_disc(tmp_path):
    pics = pictures(M2V)
    timed = [(a, b, disp, typ, BASE + disp * 3003) for a, b, disp, typ in pics]
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    n = (len(pics) * 3003) // 2880
    audio = [(frames[k % len(frames)], BASE + k * 2880) for k in range(n)]
    sectors, navs = dvd.mux_vobus(M2V, timed, 0, audio, BASE - 3003)
    total = len(sectors)
    p = Pgc(cells=[cell(0, total - 1, len(pics) * 1001 // 30000, last_vobu=navs[-1].lbn)],
            audio_control=bytes([0x80, 0]) + bytes(14))
    folder = build(tmp_path / "disc", [p], [[(1, 1)]], navs, total, video_attr=bytes([0x40, 0]),
                   audio_attrs=[ac3(b"en")])
    for k, sec in enumerate(sectors):
        put(folder, k, sec)
    return folder


def demux(folder, out):
    p = subprocess.run([*PROGRAM, "demux", str(folder), "0", str(out)], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def test_a_full_disk_names_the_file_and_stops_the_rip(tmp_path):
    folder = small_disc(tmp_path)
    code, log = demux(folder, tmp_path / "first")
    assert code == 0, log
    video = [os.path.relpath(os.path.join(d, f), tmp_path / "first")
             for d, _, fs in os.walk(tmp_path / "first") for f in fs if f.endswith(".m2v")]
    assert len(video) == 1, video
    target = tmp_path / "full" / video[0]
    target.parent.mkdir(parents=True)
    os.symlink("/dev/full", target)
    code, log = demux(folder, tmp_path / "full")
    assert code == 1, log
    assert f"failed: the disk is full (no space left writing {target})" in log, log
    assert "Stopped: the disk is full; 1 of 1 title not written" in log, log
    assert "Input/output error" not in log, log
