"""A forced-only PGS copy in the stream files (writer.c): with forced
subtitles a .sup of its own with only the sets that show or clear them;
without any, no file and the track list says "no forced subtitles: no file"
(not a warning). A Blu-ray built byte by byte (tests/synth/bluray.py)."""

import json
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream
from unit.test_disctl_pgs import clear_set, display_set, obj

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
S, E = 27000000, 27000000 + 45000 * 4
V, PG = 0x1011, 0x1200


def disc(tmp_path, sets):
    timed = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        timed.append((2 * S + 3003 * i, bd.ts_packets(V, bd.pes(0xe0, c, pts=2 * S + 3003 * i))))
    cc = 0
    for k, data in enumerate(sets):
        pts = 2 * S + 9000 * (k + 1)
        for sg in split(data):
            ps = bd.ts_packets(PG, bd.pes(0xbd, sg, pts=pts), cc=cc)
            cc += len(ps)
            timed.append((pts, ps))
    packets = [p for _, ps in sorted(timed, key=lambda t: t[0]) for p in ps]
    data = bd.source_units(packets)
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),),
                        streams=((V, 0x02, b"\x31"), (PG, 0x90, b"eng")))}
    item = Item("00001", S, E, videos=[Stream(V, 0x02, b"\x31")], pg=[Stream(PG, 0x90, b"eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=((1, 0, S), (1, 0, S + 90000)))})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def split(data):
    out, i = [], 0
    while i < len(data):
        n = 3 + int.from_bytes(data[i + 1:i + 3], "big")
        out.append(data[i:i + n])
        i += n
    return out


def rip(tmp_path, disc_dir):
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "--json", "demux", str(disc_dir), "0", str(out)], capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    msgs = [(r.get("level"), r.get("text", "")) for r in recs if r.get("type") == "message"]
    return msgs, sorted(f.name for f in out.rglob("*.sup"))


def test_no_forced_subtitles_no_file_and_no_warning(tmp_path):
    msgs, sups = rip(tmp_path, disc(tmp_path, [display_set([obj(0)], 1), clear_set(2, 0x00)]))
    line = [(lv, t) for lv, t in msgs if t.startswith("  02  eng PGS")]
    assert len(sups) == 1, sups
    assert len(line) == 1 and line[0][0] == "info" and line[0][1].endswith("no forced subtitles: no file"), msgs


def test_forced_subtitles_give_a_file_of_their_own(tmp_path):
    msgs, sups = rip(tmp_path, disc(tmp_path, [display_set([obj(0)], 1), clear_set(2, 0x00),
                                               display_set([obj(0, forced=True)], 3), clear_set(4, 0x00)]))
    assert len(sups) == 2 and any("(forced)" in s for s in sups), (sups, msgs)
