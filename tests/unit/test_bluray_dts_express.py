"""DTS-HD without a core on a Blu-ray (bluray_probe.c: no core substream
found; bluray_tracks.c / bluray_part.c / disc.c: the track marked; the
title timeline cuts and times its extension substream frames): a disc built
byte by byte (tests/synth/bluray.py) with a DTS Express stream and a DTS-HD
stream with its core. The Express stream comes out as its frames, timed by
their LBR headers; the DTS-HD stream and its core come out as before."""

import json
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream
from unit.test_disctl_dts import exss, express

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
DTS = (DATA / "sine_5120.dts").read_bytes()
CORES = [DTS[i:i + 1884] for i in range(0, len(DTS), 1884)]
S, E = 27000000, 27000000 + 45000 * 4
V, HD, X = 0x1011, 0x1100, 0x1101


def disc(tmp_path, fs):
    timed = []                                            # (pts, TS packets) in time order
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        timed.append((2 * S + 3003 * i, bd.ts_packets(V, bd.pes(0xe0, c, pts=2 * S + 3003 * i))))
    for i, f in enumerate(CORES):                         # DTS-HD: the core (0x71), then its extension (0x72)
        pts = 2 * S + 960 * i
        timed.append((pts, bd.ts_packets(HD, bd.pes(0xfd, f, ext=0x71, pts=pts), cc=2 * i)))
        timed.append((pts, bd.ts_packets(HD, bd.pes(0xfd, exss(600, i), ext=0x72, pts=pts), cc=2 * i + 1)))
    for i, f in enumerate(fs):                            # DTS Express: extension substreams only
        pts = 2 * S + 7680 * i
        timed.append((pts, bd.ts_packets(X, bd.pes(0xfd, f, ext=0x72, pts=pts), cc=i)))
    packets = [p for _, ps in sorted(timed, key=lambda t: t[0]) for p in ps]
    data = bd.source_units(packets)
    streams = ((V, 0x02, b"\x31"), (HD, 0x86, b"\x61eng"), (X, 0x86, b"\x31eng"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(V, 0x02, b"\x31")],
                audio=[Stream(HD, 0x86, b"\x61eng"), Stream(X, 0x86, b"\x31eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=((1, 0, S), (1, 0, S + 90000)))})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def rip(tmp_path, disc_dir):
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "--json", "--set", "titles.min_length=0", "demux", str(disc_dir), "0", str(out)],
                       capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    texts = [r.get("text", "") for r in recs if r.get("type") == "message"]
    return texts, {f.name: f.read_bytes() for f in out.rglob("*") if f.is_file()}


def test_dts_express_is_ripped_as_its_frames_and_dts_hd_as_before(tmp_path):
    fs = express(44)
    texts, files = rip(tmp_path, disc(tmp_path, fs))
    log = "\n".join(texts)
    dts = {n: b for n, b in files.items() if n.endswith(".dts")}
    assert "no frame start" not in log, log
    assert "fails its checksum" not in log, log
    assert len(dts) == 3, (sorted(files), log)
    by_size = sorted(dts.values(), key=len)
    assert b"".join(CORES) in by_size                                     # the core taken out of DTS-HD
    assert b"".join(f + exss(600, i) for i, f in enumerate(CORES)) in by_size
    assert b"".join(fs) in by_size                                        # DTS Express: every frame, as it was


def test_a_frame_whose_checksum_fails_is_told_and_kept(tmp_path):
    # the title check (verify) reads each frame's checksums: a failing one
    # is a warning with its kind; the frame stays as it is on the disc
    fs = express(44, bad=(10,))
    texts, files = rip(tmp_path, disc(tmp_path, fs))
    log = "\n".join(texts)
    assert "fails its checksum (the DTS Express frame's checksum)" in log, log
    assert log.count("fails its checksum") == 1, log
    assert b"".join(fs) in files.values()
