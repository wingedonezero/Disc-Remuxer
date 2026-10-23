"""mkv.tie_subtitles in the mkvmerge mode (writer.c tie_subtitles, subtie.c),
with the real mkvmerge: a Blu-ray built byte by byte, MPEG-2 at 29.97 with PGS
sets on pictures 3, 15, 20, 45 and 50. Pictures 15 and 45 sit exactly on a half
ms (500.5 and 1,501.5 ms): mkvmerge counts them a hair early (15 x 33,366,666
ns = 500.49999 ms -> 500), the disc's time rounds up (501). Tied, every set is
on its picture's ms in the MKV, also after mkvextract + a plain rebuild; off,
the two land 1 ms after their pictures; the stream files are the same either
way."""

import os
import shutil
import subprocess

import pytest

from helpers import DATA, DIST, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream
from unit.test_disctl_pgs import clear_set, display_set, obj
from unit.test_forced_copy_file import split

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
S, E = 27000000, 27000000 + 45000 * 4
V, PG = 0x1011, 0x1200
SETS = [(3, True), (15, True), (20, False), (45, True), (50, False)]   # (picture, shows / clears)

pytestmark = pytest.mark.skipif(not shutil.which("mkvmerge") or not shutil.which("mkvextract"),
                                reason="mkvmerge is not installed")


def disc(tmp_path):
    timed = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        timed.append((2 * S + 3003 * i, bd.ts_packets(V, bd.pes(0xe0, c, pts=2 * S + 3003 * i))))
    cc = 0
    for n, (picture, shows) in enumerate(SETS):
        pts = 2 * S + 3003 * picture
        data = display_set([obj(0)], n + 1) if shows else clear_set(n + 1, 0x00)
        for sg in split(data):
            ps = bd.ts_packets(PG, bd.pes(0xbd, sg, pts=pts), cc=cc)
            cc += len(ps)
            timed.append((pts, ps))
    packets = [p for _, ps in sorted(timed, key=lambda t: t[0]) for p in ps]
    data = bd.source_units(packets)
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),),
                        streams=((V, 0x02, b"\x31"), (PG, 0x90, b"eng")))}
    item = Item("00001", S, E, videos=[Stream(V, 0x02, b"\x31")], pg=[Stream(PG, 0x90, b"eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=((1, 0, S),))})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def rip(disc_dir, out, *sets):
    args = [*PROGRAM]
    for s in sets:
        args += ["--set", s]
    p = subprocess.run([*args, "demux", str(disc_dir), "0", str(out)], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout


def ffprobe():
    return shutil.which("ffprobe") or str(DIST / "ffprobe")


def times(mkv, sel):
    out = subprocess.run([ffprobe(), "-v", "error", "-select_streams", sel, "-show_entries", "packet=pts", "-of",
                          "csv=p=0", str(mkv)], capture_output=True, text=True, check=True).stdout
    return sorted(int(x) for x in out.split())


def test_tied_sets_sit_on_their_pictures_in_the_mkv_and_after_a_rebuild(tmp_path):
    d = disc(tmp_path)
    log = rip(d, tmp_path / "tied", "output.format=mkvmerge", "mkv.tie_subtitles=true")
    mkv = tmp_path / "tied" / "title_t00.mkv"
    video, subs = times(mkv, "v:0"), times(mkv, "s:0")
    assert subs == [video[p] for p, _ in SETS], (subs, [video[p] for p, _ in SETS])
    assert video[15] == 500 and video[45] == 1501, "mkvmerge's count: a hair early, rounded down"
    assert "Track 1 (PGS): 5 subtitles tied to their pictures on mkvmerge's count of the video: 2 moved (by at most " \
           "1 ms), 0 start on a later field of their picture (kept there)" in log, log
    assert not list((tmp_path / "tied").rglob("*.tied.*")), "the tied copy goes with the stream files"
    # extracted and rebuilt plain: the same times
    shutil.copy(mkv, tmp_path / "a.mkv")
    subprocess.run(["mkvextract", str(tmp_path / "a.mkv"), "tracks", f"0:{tmp_path}/v.m2v", f"1:{tmp_path}/s.sup"],
                   check=True, capture_output=True)
    subprocess.run(["mkvmerge", "-q", "-o", str(tmp_path / "b.mkv"), str(tmp_path / "v.m2v"), str(tmp_path / "s.sup")],
                   check=True, capture_output=True)
    assert times(tmp_path / "b.mkv", "v:0") == video and times(tmp_path / "b.mkv", "s:0") == subs


def test_untied_the_half_ms_sets_land_1_ms_after_their_pictures(tmp_path):
    d = disc(tmp_path)
    log = rip(d, tmp_path / "plain", "output.format=mkvmerge")
    mkv = tmp_path / "plain" / "title_t00.mkv"
    video, subs = times(mkv, "v:0"), times(mkv, "s:0")
    assert subs == [video[p] + (1 if p in (15, 45) else 0) for p, _ in SETS], (subs, video[:51])
    assert "tied to their pictures" not in log and "not tied to their pictures" not in log, \
        "the setting off: nothing done, nothing said"


def test_the_stream_files_keep_the_disc_s_times_whatever_the_setting(tmp_path):
    d = disc(tmp_path)
    rip(d, tmp_path / "a", "output.format=streams", "mkv.tie_subtitles=true")
    rip(d, tmp_path / "b", "output.format=streams")
    a, b = list((tmp_path / "a").rglob("*.sup")), list((tmp_path / "b").rglob("*.sup"))
    assert len(a) == 1 and a[0].read_bytes() == b[0].read_bytes()
