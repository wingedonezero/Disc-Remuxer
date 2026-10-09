"""The title reader leaves filler clips at a title's ends out (disctitle.c,
the reference's rip does the same; LOOK INTO AGAIN LATER): a built DVD
title of two segments, the MPEG-2 test stream with AC-3 audio, its last
four GOPs a segment of its own (a cell starting a new timeline; their first
frame rate + 4 pictures span more than 1.001 s, so the end test for short
segments of small pictures does not take it). Video only and under 15 s,
that segment is filler and left out; with its audio it stays."""

from helpers import DATA, PROGRAM
from helpers.disctl import pictures
from helpers.dvdnav import Pgc, build, cell
from synth import dvd
from synth.dvd import S, flags
from test_dvd_streams import put
from test_dvd_titles import ac3, info
import subprocess

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
BASE = 90000


def two_segments(tmp_path, audio_in_last):
    pics = pictures(M2V)
    gops = [k for k, (a, b, _, _) in enumerate(pics) if b"\x00\x00\x01\xb8" in M2V[a:b][:64]]
    cut = gops[-4]
    timed = [(a, b, disp, typ, BASE + disp * 3003) for a, b, disp, typ in pics]
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    n = (len(pics) * 3003) // 2880
    audio = [(frames[k % len(frames)], BASE + k * 2880) for k in range(n)]
    cut_pts = timed[cut][4] - 3 * 3003
    first, navs1 = dvd.mux_vobus(M2V, timed[:cut], 0, [x for x in audio if x[1] < cut_pts], BASE - 3003)
    second, navs2 = dvd.mux_vobus(M2V, timed[cut:], len(first), [x for x in audio if x[1] >= cut_pts] if audio_in_last
                                  else [], BASE - 3003 + cut * 3003)
    total = len(first) + len(second)
    secs1 = cut * 1001 // 30000
    p = Pgc(cells=[cell(0, len(first) - 1, secs1, last_vobu=navs1[-1].lbn),
                   cell(len(first), total - 1, 1, last_vobu=navs2[-1].lbn, flags=flags.STC_DISCONTINUITY)],
            audio_control=bytes([0x80, 0]) + bytes(14))
    folder = build(tmp_path, [p], [[(1, 1)]], navs1 + navs2, total, video_attr=bytes([0x40, 0]),
                   audio_attrs=[ac3(b"en")])
    for k, sec in enumerate(first + second):
        put(folder, k, sec)
    return folder, M2V[pics[0][0]:pics[cut][0]]


def demux(folder, out):
    p = subprocess.run([*PROGRAM, "demux", str(folder), "0", str(out)], capture_output=True,
                       text=True)
    return p.stdout + p.stderr


def test_a_trailing_segment_of_half_a_second_without_audio_is_filler(tmp_path):
    folder, kept = two_segments(tmp_path, False)
    ts, _ = info(folder)
    assert ts[0]["files"] == ["1", "2"], "the disc info lists both segments"
    log = demux(folder, tmp_path / "out")
    assert "segment 2 (2 of 2): filler at the title's end: left out" in log, log
    video = next((tmp_path / "out").glob("*.m2v")).read_bytes()
    assert video == kept


def test_a_trailing_segment_with_audio_stays(tmp_path):
    folder, kept = two_segments(tmp_path, True)
    log = demux(folder, tmp_path / "out")
    assert "filler" not in log, log
    video = next((tmp_path / "out").glob("*.m2v")).read_bytes()
    assert video == M2V
