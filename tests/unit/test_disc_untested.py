"""A codec the title reader reads but no real disc of the format has had yet
(src/disc/bluray/bluray_part.c, disc.c; src/disc-remuxer/writer.c): a Blu-ray
title with MPEG audio (tests/synth/bluray.py) is ripped, the track's line in
the log says "[untested on real discs]", and the title ends done with
warnings, not failed."""

import json
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
MP2 = (DATA / "sine_11520.mp2").read_bytes()       # MPEG-1 layer II, 48 kHz, 192 kb/s: 576-byte frames of 24 ms
S, E = 27000000, 27000000 + 90000 * 4


def pes_packets(pid, chunks, stream_id, step):
    out = []
    for i, c in enumerate(chunks):
        out += bd.ts_packets(pid, bd.pes(stream_id, c, pts=900 + step * i))
    return out


def test_mpeg_audio_on_a_blu_ray_is_ripped_and_said_untested(tmp_path):
    packets = (pes_packets(0x1011, [M2V[i:i + 60000] for i in range(0, len(M2V), 60000)], 0xe0, 3003) +
               pes_packets(0x1100, [MP2[i:i + 576] for i in range(0, len(MP2), 576)], 0xc0, 2160))
    data = bd.source_units(packets)
    streams = ((0x1011, 0x02, b"\x31"), (0x1100, 0x03, b"\x31eng"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")], audio=[Stream(0x1100, 0x03, b"\x31eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    disc = bd.write(tmp_path / "disc", files)
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "--json", "demux", str(disc), "0", str(out)],
                       capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    texts = [r.get("text", "") for r in recs if r.get("type") == "message"]
    assert any("MPEG audio on a Blu-ray [untested on real discs]" in t for t in texts), texts
    result = [r for r in recs if r.get("type") == "title_result"][0]
    assert result["status"] != "failed", result
    assert result["warnings"] > 0
    names = sorted(f.name for f in out.rglob("*") if f.is_file() and f.suffix != ".log")
    assert any(n.endswith(".mp2") for n in names), names
    assert any(n.endswith(".m2v") for n in names), names
