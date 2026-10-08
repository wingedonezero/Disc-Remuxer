"""The title list's stream details on Blu-ray (libavformat/bluray_details.c,
src/disc-remuxer/codecs.c): a disc folder built here (tests/synth/bluray.py)
whose clip carries MPEG-2 video, AC-3, DTS and TrueHD with its AC-3 core,
each on its own PID and PES key (stream id 0xFD with its extension, as on a
disc), read by the program's info: every track's details come from its own
stream. With the 7 corpus discs every track's details equal the reference
program's info (implementation notes), but for LPCM's bit rate (the
reference's is not the stream's) and the 3-D second view's profile text."""

import json
import subprocess

from helpers import DATA, DIST
from synth import bluray as bd
from synth.bluray import Item, Stream

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
DTS = (DATA / "sine_5120.dts").read_bytes()
THD = (DATA / "sine_4813.thd").read_bytes()
S, E = 27000000, 27000000 + 90000 * 4


def frames(data, size):
    return [data[i:i + size] for i in range(0, len(data), size)]


def stream_packets(pid, chunks, stream_id=0xfd, ext=None):
    """Each chunk in a PES packet of its own (a PTS on each), in TS packets."""
    out = []
    for i, c in enumerate(chunks):
        out += bd.ts_packets(pid, bd.pes(stream_id, c, ext=ext, pts=900 + 3003 * i))
    return out


def disc(tmp_path, video=M2V):
    packets = (stream_packets(0x1011, frames(video, 60000), 0xe0) +
               stream_packets(0x1100, frames(AC3, 768), ext=0x71) +
               stream_packets(0x1101, frames(DTS, 1884), ext=0x71) +
               stream_packets(0x1102, frames(THD, 600), ext=0x72) +
               stream_packets(0x1102, frames(AC3, 768), ext=0x76))
    data = bd.source_units(packets)
    streams = ((0x1011, 0x02, b"\x31"), (0x1100, 0x81, b"\x31eng"), (0x1101, 0x82, b"\x31eng"),
               (0x1102, 0x83, b"\x31eng"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")],
                audio=[Stream(0x1100, 0x81, b"\x31eng"), Stream(0x1101, 0x82, b"\x31eng"),
                       Stream(0x1102, 0x83, b"\x31eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def info(path):
    out = subprocess.run([str(DIST / "disc-remuxer"), "--json", "info", str(path)], capture_output=True, text=True)
    titles = [json.loads(l) for l in out.stdout.splitlines() if l.startswith('{"type":"title"')]
    assert titles, out.stdout + out.stderr
    return titles


def test_each_track_describes_itself_from_its_stream(tmp_path):
    t = info(disc(tmp_path))[0]
    by_codec = {}
    for k in t["tracks"]:
        by_codec.setdefault(k["description"], k)
    v = t["tracks"][0]
    assert (v["description"], v["width"], v["height"], v["frame_rate"], v["aspect"]) == \
        ("Mpeg2", 64, 48, "30000/1001", "4:3")
    ac3 = by_codec["Dolby Digital"]
    assert (ac3["channels"], ac3["layout"], ac3["sample_rate"], ac3["bit_rate"]) == (2, "stereo", 48000, 192000)
    dts = by_codec["DTS"]
    assert (dts["channels"], dts["sample_rate"]) == (2, 48000), "the DTS core's own values"
    thd = by_codec["TrueHD"]
    # FFmpeg's TrueHD encoder states 24 bits in its major sync: the details
    # give what the stream says
    assert (thd["codec"], thd["channels"], thd["sample_rate"], thd["bits"]) == ("TrueHD", 2, 48000, 24)
    assert [k["description"] for k in t["tracks"]].count("Dolby Digital") == 2, "TrueHD's AC-3 core: its own key"


def test_details_read_to_the_end_of_a_short_clip(tmp_path):
    # a video of one picture: the parser gives it out (and the decoder its
    # frame) only when the stream ends
    second = M2V.find(b"\x00\x00\x01\x00", M2V.find(b"\x00\x00\x01\x00") + 4)
    t = info(disc(tmp_path, video=M2V[:second]))[0]
    assert (t["tracks"][0]["width"], t["tracks"][0]["height"]) == (64, 48)
