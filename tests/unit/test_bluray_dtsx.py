"""DTS:X on a Blu-ray (disc/pass.c: the stream pass walks a DTS-HD track's
lossless frames with dr_dca_dtsx; writer.c names the stream file by it): a
disc built byte by byte (tests/synth/bluray.py) with two DTS-HD streams,
core + extension substream, whose lossless (XLL) frames carry the DTS:X
marker 0xF14000D3 (one the processing library's decoder does not know) or
none. The first is DTS-HD Master Audio + DTS:X in the title list and in its
file name, the second plain DTS-HD Master Audio."""

import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream
from unit.test_disctl_dts import coreless, xll_frame

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
DTS = (DATA / "sine_5120.dts").read_bytes()
CORES = [DTS[i:i + 1884] for i in range(0, len(DTS), 1884)]
S, E = 27000000, 27000000 + 45000 * 4
V, X, MA = 0x1011, 0x1100, 0x1101


def disc(tmp_path):
    timed = []                                            # (pts, TS packets) in time order
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        timed.append((2 * S + 3003 * i, bd.ts_packets(V, bd.pes(0xe0, c, pts=2 * S + 3003 * i))))
    for pid, marker in ((X, 0xF14000D3), (MA, None)):     # DTS-HD: the core (0x71), then its extension (0x72)
        ext = coreless("xll_frame", xll_frame(marker))
        for i, f in enumerate(CORES):
            pts = 2 * S + 960 * i
            timed.append((pts, bd.ts_packets(pid, bd.pes(0xfd, f, ext=0x71, pts=pts), cc=2 * i)))
            timed.append((pts, bd.ts_packets(pid, bd.pes(0xfd, ext, ext=0x72, pts=pts), cc=2 * i + 1)))
    packets = [p for _, ps in sorted(timed, key=lambda t: t[0]) for p in ps]
    data = bd.source_units(packets)
    streams = ((V, 0x02, b"\x31"), (X, 0x86, b"\x61eng"), (MA, 0x86, b"\x61fra"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(V, 0x02, b"\x31")],
                audio=[Stream(X, 0x86, b"\x61eng"), Stream(MA, 0x86, b"\x61fra")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=((1, 0, S), (1, 0, S + 90000)))})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def test_dts_x_is_named_by_its_marker_in_the_title_list_and_the_file_name(tmp_path):
    d = disc(tmp_path)
    p = subprocess.run([*PROGRAM, "--set", "titles.min_length=0", "info", str(d)], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    tracks = [l for l in p.stdout.splitlines() if " audio " in l]
    assert "eng DTS-HD Master Audio + DTS:X" in tracks[0], tracks
    assert "fra DTS-HD Master Audio" in tracks[2] and "DTS:X" not in tracks[2], tracks
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "--set", "titles.min_length=0", "demux", str(d), "0", str(out)],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    names = sorted(f.name for f in out.rglob("*.dts"))
    assert any("[eng]_DTS-X" in n for n in names), names
    assert any("[fra]_DTS-HD MA" in n for n in names), names
