"""Blu-ray streams that give no track are said when a title is ripped
(bluray_tracks.c ff_bluray_title_left_out): a stream of the clip the
playlist's stream table does not list (here the disc's menus and an audio
stream), a stream of a kind no track is made of, a further video stream.
The streams that are ripped, and a Dolby Vision enhancement layer read with
its video, are not. A disc built byte by byte (tests/synth/bluray.py)."""

import json
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
S, E = 27000000, 27000000 + 45000 * 4
V, A, HIDDEN, IG, ODD, V2 = 0x1011, 0x1100, 0x1101, 0x1400, 0x1102, 0x1012


def disc(tmp_path):
    timed = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        timed.append((2 * S + 3003 * i, bd.ts_packets(V, bd.pes(0xe0, c, pts=2 * S + 3003 * i))))
        timed.append((2 * S + 3003 * i, bd.ts_packets(V2, bd.pes(0xe0, c, pts=2 * S + 3003 * i), cc=i)))
    for pid in (A, HIDDEN, ODD):
        timed.append((2 * S, bd.ts_packets(pid, bd.pes(0xfd, AC3, ext=0x71, pts=2 * S))))
    packets = [p for _, ps in sorted(timed, key=lambda t: t[0]) for p in ps]
    data = bd.source_units(packets)
    streams = ((V, 0x02, b"\x31"), (V2, 0x02, b"\x31"), (A, 0x81, b"\x61eng"), (HIDDEN, 0x81, b"\x61fra"),
               (ODD, 0x99, b"\x61deu"), (IG, 0x91, b"eng"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(V, 0x02, b"\x31")],
                audio=[Stream(A, 0x81, b"\x61eng"), Stream(ODD, 0x99, b"\x61deu")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=((1, 0, S), (1, 0, S + 90000)))})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def rip(tmp_path, disc_dir, title="0"):
    p = subprocess.run([*PROGRAM, "--json", "--set", "titles.min_length=0", "demux", str(disc_dir), title,
                        str(tmp_path / ("out" + title))], capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    return [(r.get("level"), r.get("text", "")) for r in recs if r.get("type") == "message"]


def test_each_stream_that_gives_no_track_is_said_with_why(tmp_path):
    d = disc(tmp_path)
    msgs = rip(tmp_path, d)
    said = [(lv, t) for lv, t in msgs if "of clip 00001" in t]
    log = "\n".join(t for _, t in msgs)
    table = "is not in the playlist's stream table of every item (video, audio, subtitles): not ripped"
    assert said == [
        ("info", f"Blu-ray: title 00001.mpls: stream PID 0x1012 of clip 00001 (MPEG-2 video) {table}"),
        ("info", f"Blu-ray: title 00001.mpls: stream PID 0x1101 of clip 00001 (AC-3 audio) {table}"),
        ("warning", "Blu-ray: title 00001.mpls: stream PID 0x1102 of clip 00001 (an unknown kind, stream coding "
                    "type 0x99) is a kind no track is made of: not ripped"),
        ("info", "Blu-ray: title 00001.mpls: stream PID 0x1400 of clip 00001 (interactive graphics: the disc's "
                 "menus): not ripped"),
    ], log
    # the clip as a title of its own keeps every stream of the clip: the
    # second video is the one left out
    msgs = rip(tmp_path, d, "1")
    said = [(lv, t) for lv, t in msgs if "of clip 00001" in t]
    assert said == [
        ("info", "Blu-ray: title 00001.m2ts: stream PID 0x1012 of clip 00001 (MPEG-2 video) is a further video "
                 "stream: only the first is ripped"),
        ("warning", "Blu-ray: title 00001.m2ts: stream PID 0x1102 of clip 00001 (an unknown kind, stream coding "
                    "type 0x99) is a kind no track is made of: not ripped"),
        ("info", "Blu-ray: title 00001.m2ts: stream PID 0x1400 of clip 00001 (interactive graphics: the disc's "
                 "menus): not ripped"),
    ], "\n".join(t for _, t in msgs)
