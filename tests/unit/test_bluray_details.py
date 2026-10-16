"""The title list's stream details on Blu-ray (the disc layer's stream pass,
src/disc/pass.c; src/disc-remuxer/codecs.c): a disc folder built here (tests/synth/bluray.py)
whose clip carries MPEG-2 video, AC-3, DTS and TrueHD with its AC-3 core,
each on its own PID and PES key (stream id 0xFD with its extension, as on a
disc), read by the program's info: every track's details come from its own
stream. With the 7 corpus discs every track's details equal the reference
program's info (implementation notes), but for LPCM's bit rate (the
reference's is not the stream's) and the 3-D second view's profile text."""

import json
import subprocess

import pytest

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth import h264
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
    out = subprocess.run([*PROGRAM, "--json", "info", str(path)], capture_output=True, text=True)
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


def avc_disc(tmp_path, mbs, aspect_ratio_idc, attr, full_range=None):
    """A clip of SD AVC only: three grey IDR pictures of mbs macroblocks
    whose SPS gives the sample aspect ratio (aspect_ratio_idc)."""
    au = (h264.aud(0) + h264.sps(mbs=mbs, crop_bottom=0, aspect_ratio_idc=aspect_ratio_idc, timing=(1, 50),
                                 hrd=False, pic_struct=False, full_range=full_range) +
          h264.pps(cabac=0) + h264.idr_grey(mbs[0] * mbs[1]))
    packets = stream_packets(0x1011, [au] * 3, 0xe0)
    data = bd.source_units(packets)
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=((0x1011, 0x1b, attr),))}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x1b, attr)])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


@pytest.mark.parametrize("mbs, idc, attr, size, aspect", [
    # a whole 720-sample line of a 4:3 picture with horizontal overscan
    # (H.264 Table E-1): 15:11 by the numbers, shown as the 4:3 the clip's
    # attributes call it
    ((45, 36), 2, b"\x23\x20", (720, 576), "4:3"),     # 12:11, 576 lines
    ((45, 30), 3, b"\x14\x20", (720, 480), "4:3"),     # 10:11, 480 lines
    ((45, 36), 4, b"\x23\x30", (720, 576), "16:9"),    # 16:11: 20:11 by the numbers
    ((45, 36), 1, b"\x23\x20", (720, 576), "5:4"),     # square pixels: the ratio itself
])
def test_sd_avc_aspect(tmp_path, mbs, idc, attr, size, aspect):
    v = info(avc_disc(tmp_path, mbs, idc, attr))[0]["tracks"][0]
    assert (v["width"], v["height"], v["aspect"]) == (*size, aspect)


@pytest.mark.parametrize("full", [1, 0])
def test_a_video_that_says_full_colour_range_is_said(tmp_path, full):
    d = avc_disc(tmp_path, (45, 36), 2, b"\x23\x20", full_range=full)
    p = subprocess.run([*PROGRAM, "--set", "titles.min_length=0", "info", str(d)], capture_output=True, text=True)
    line = ("Blu-ray: title 00001.m2ts: video stream 0 says full colour range (disc video is limited range): kept as "
            "on the disc")
    assert (line in p.stdout) == bool(full), p.stdout
