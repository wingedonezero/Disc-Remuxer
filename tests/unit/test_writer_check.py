"""The end check of written files (src/disc-remuxer/check.c): a stream file
read back must keep the disc stream's details. 16-bit stereo LPCM is written
as a plain RIFF WAVE (WAVE_FORMAT_PCM: no channel mask; its two channels are
left and right by the format's definition): read back it is stereo, not
"2 channels" of no layout (a built Blu-ray, tests/synth/bluray.py)."""

import json
import struct
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
S, E = 27000000, 27000000 + 90000 * 4


def lpcm_unit(k, samples=240):
    """A Blu-ray LPCM payload: the 4-byte header (its size; stereo, 48 kHz;
    16 bits) and samples x 2 channels of big-endian 16-bit sound."""
    body = b"".join(struct.pack(">hh", (i * 37 + k) % 3000 - 1500, (i * 53 + k) % 3000 - 1500) for i in range(samples))
    return struct.pack(">H", len(body)) + bytes([0x31, 0x40]) + body


def pes_packets(pid, chunks, stream_id, step):
    out = []
    for i, c in enumerate(chunks):
        out += bd.ts_packets(pid, bd.pes(stream_id, c, pts=900 + step * i))
    return out


def test_16_bit_stereo_lpcm_written_as_wave_keeps_its_layout(tmp_path):
    packets = (pes_packets(0x1011, [M2V[i:i + 60000] for i in range(0, len(M2V), 60000)], 0xe0, 3003) +
               pes_packets(0x1100, [lpcm_unit(k) for k in range(800)], 0xbd, 450))
    data = bd.source_units(packets)
    streams = ((0x1011, 0x02, b"\x31"), (0x1100, 0x80, b"\x31eng"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")], audio=[Stream(0x1100, 0x80, b"\x31eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    disc = bd.write(tmp_path / "disc", files)
    p = subprocess.run([*PROGRAM, "--json", "demux", str(disc), "0", str(tmp_path / "out")],
                       capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    texts = [r.get("text", "") for r in recs if r.get("type") == "message"]
    assert not [t for t in texts if "Channel layout" in t], texts
    result = [r for r in recs if r.get("type") == "title_result"][0]
    assert result["status"] != "failed", texts
    wav = [f for f in (tmp_path / "out").rglob("*.wav")]
    assert wav and wav[0].read_bytes()[20:22] == b"\x01\x00", "WAVE_FORMAT_PCM"
