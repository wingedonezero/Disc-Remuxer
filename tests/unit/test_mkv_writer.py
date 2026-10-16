"""The Matroska writer (src/mkv, src/disc-remuxer/mkvout.c; output.format =
mkv): a built Blu-ray of H.264 and LPCM into one file, read back element by
element: the EBML head, the track headers mkvmerge writes for these codecs
(the H.264 configuration record from the SPS and PPS, the sizes, the default
durations, the languages), every block (the video's NAL units with 4-byte
sizes, the access unit delimiters left out; the samples in 40 ms blocks),
the cues (each video key frame), the statistics tags. On real discs the
file equals mkvmerge's byte for byte (implementation notes); here the rules
are checked one by one."""

import struct
import subprocess

from helpers import PROGRAM
from synth import bluray as bd
from synth import h264
from synth.bluray import Item, Stream
from test_stream_files import lpcm_disc

S, E = 27000000, 27000000 + 90000 * 4
FRAMES = 50                         # 2 s of 25 fps
SPS = h264.sps(mbs=(4, 3), crop_bottom=0, aspect_ratio_idc=1, timing=(1, 50), hrd=False, pic_struct=False)
PPS = h264.pps(cabac=0)
IDR = h264.idr_grey(4 * 3)


def lpcm_samples(k):
    return [((i * 37 + k * 11 + c * 101) % 3000 - 1500) for i in range(240) for c in range(2)]


def avc_lpcm_disc(tmp_path):
    """FRAMES H.264 IDR pictures at 25 fps (each access unit: delimiter, SPS,
    PPS, slice) and 16-bit stereo LPCM in 5 ms units; returns the disc and
    the samples in order."""
    video, audio, raw = [], [], []
    for i in range(FRAMES):
        au = h264.aud(0) + SPS + PPS + IDR
        video += bd.ts_packets(0x1011, bd.pes(0xe0, au, pts=54000000 + 3600 * i))
    for k in range(FRAMES * 8):
        s = lpcm_samples(k)
        raw += s
        body = b"".join(v.to_bytes(2, "big", signed=True) for v in s)
        audio += bd.ts_packets(0x1100, bd.pes(0xbd, struct.pack(">H", len(body)) + bytes([3 << 4 | 1, 1 << 6]) + body,
                                              pts=54000000 + 450 * k))
    # interleaved as on a disc: each picture with the audio of its time
    packets = []
    for i in range(FRAMES):
        packets += video[i * len(video) // FRAMES:(i + 1) * len(video) // FRAMES]
        packets += audio[i * len(audio) // FRAMES:(i + 1) * len(audio) // FRAMES]
    data = bd.source_units(packets)
    vattr, aattr = b"\x31", bytes([3 << 4 | 1]) + b"jpn"
    streams = ((0x1011, 0x1b, vattr), (0x1100, 0x80, aattr))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x1b, vattr)], audio=[Stream(0x1100, 0x80, aattr)])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files), raw


def demux_mkv(disc, out):
    p = subprocess.run([*PROGRAM, "--set", "output.format=mkv", "--set", "titles.min_length=0", "demux", str(disc),
                        "0", str(out)], capture_output=True, text=True)
    mkv = list(out.rglob("*.mkv"))
    assert p.returncode == 0 and len(mkv) == 1, p.stdout + p.stderr
    return mkv[0].read_bytes(), p.stdout


# ---- reading EBML ----

def vint(b, i, keep_marker=False):
    n = 1
    while not b[i] & (0x80 >> (n - 1)):
        n += 1
    v = b[i] if keep_marker else b[i] & (0xFF >> n)
    for k in range(1, n):
        v = v << 8 | b[i + k]
    return v, i + n


MASTERS = {0x1A45DFA3, 0x18538067, 0x114D9B74, 0x1549A966, 0x1654AE6B, 0xAE, 0xE0, 0xE1, 0x1F43B675, 0x1C53BB6B,
           0xBB, 0xB7, 0x1043A770, 0x45B9, 0xB6, 0x80, 0x1254C367, 0x7373, 0x63C0, 0x67C8, 0x4DBB, 0xA0}


def elements(b, start=0, end=None):
    """(id, data) of each element from start to end, masters as lists."""
    end = len(b) if end is None else end
    out, i = [], start
    while i < end:
        eid, i = vint(b, i, True)
        size, i = vint(b, i)
        out.append((eid, elements(b, i, i + size) if eid in MASTERS else b[i:i + size]))
        i += size
    return out


def find(tree, *path):
    found = [tree]
    for eid in path:
        found = [d for node in found for (i, d) in node if i == eid]
    return found


def uint(b):
    return int.from_bytes(b, "big")


def laced_frames(block):
    """A SimpleBlock's frames (no lacing, Xiph, fixed or EBML lacing)."""
    _, i = vint(block, 0)
    flags = block[i + 2]
    data = block[i + 3:]
    lacing = (flags >> 1) & 3
    if not lacing:
        return [data]
    n, i = data[0] + 1, 1
    if lacing == 2:
        size = (len(data) - 1) // n
        return [data[1 + k * size:1 + (k + 1) * size] for k in range(n)]
    sizes = []
    if lacing == 1:
        for _ in range(n - 1):
            s = 0
            while data[i] == 255:
                s += 255
                i += 1
            s += data[i]
            i += 1
            sizes.append(s)
    else:
        first, i = vint(data, i)
        sizes.append(first)
        for _ in range(n - 2):
            j = i
            v, i = vint(data, i)
            bits = 7 * (i - j)
            sizes.append(sizes[-1] + v - ((1 << (bits - 1)) - 1))
    sizes.append(len(data) - i - sum(sizes))
    out = []
    for s in sizes:
        out.append(data[i:i + s])
        i += s
    return out


def test_h264_and_lpcm_go_into_one_file_as_mkvmerge_writes_them(tmp_path):
    disc, raw = avc_lpcm_disc(tmp_path)
    data, log = demux_mkv(disc, tmp_path / "out")
    top = elements(data)
    head = dict(find(top, 0x1A45DFA3)[0])
    assert head[0x4282] == b"matroska"
    assert (uint(head[0x4287]), uint(head[0x4285])) == (4, 2), "DocTypeVersion 4 (IETF languages), read version 2"

    seg = find(top, 0x18538067)[0]
    info = dict(find(seg, 0x1549A966)[0])
    assert uint(info[0x2AD7B1]) == 1000000, "1 ms timestamp scale"
    assert struct.unpack(">f", info[0x4489])[0] == 2000.0, "the duration in ms (a 4-byte float with video)"
    assert info[0x5741].startswith(b"disc-remuxer ")

    video, audio = find(seg, 0x1654AE6B, 0xAE)
    v, a = dict(video), dict(audio)
    assert v[0x86] == b"V_MPEG4/ISO/AVC" and uint(v[0x83]) == 1 and uint(v[0x9C]) == 0, "video: lacing off"
    s, p = (x[x.index(b"\0\0\1") + 3:] for x in (SPS, PPS))   # the SPS / PPS after their start codes
    assert v[0x63A2] == bytes([1, s[1], s[2], s[3], 0xFF, 0xE1]) + len(s).to_bytes(2, "big") + s + \
        b"\x01" + len(p).to_bytes(2, "big") + p, "the configuration record (ISO/IEC 14496-15 5.3.3.1)"
    assert uint(v[0x23E383]) == 40000000, "default duration: one frame at 25 fps"
    vv = dict(v[0xE0])
    assert [uint(vv[k]) for k in (0xB0, 0xBA, 0x54B0, 0x54BA)] == [64, 48, 64, 48]
    assert len(vv[0x54B0]) == 4 and len(vv[0x54BA]) == 4, "the display size written in 4 bytes, as mkvmerge does"
    assert a[0x86] == b"A_PCM/INT/LIT" and uint(a[0x23E383]) == 40000000, "PCM: 40 ms blocks"
    assert (a[0x22B59C], a[0x22B59D]) == (b"jpn", b"ja"), "ISO 639-2 and its BCP 47 code"
    aa = dict(a[0xE1])
    assert (struct.unpack(">f", aa[0xB5])[0], uint(aa[0x9F]), uint(aa[0x6264])) == (48000.0, 2, 16)

    clusters = find(seg, 0x1F43B675)
    vblocks, audio_bytes, atimes = [], b"", []
    for c in clusters:
        ct = uint(dict(c)[0xE7])
        for eid, d in c:
            if eid == 0xA3:
                track, _ = vint(d, 0)
                t = ct + struct.unpack(">h", d[1:3])[0]
                if track == 1:
                    vblocks.append((t, d[3], laced_frames(d)[0]))
                else:
                    atimes.append(t)
                    audio_bytes += b"".join(laced_frames(d))
            elif eid == 0xA0:                             # a BlockGroup: the last audio block, its duration
                block = dict(d)[0xA1]
                audio_bytes += b"".join(laced_frames(block))
    idr = IDR[IDR.index(b"\0\0\1") + 3:]
    frame = b"".join(len(x).to_bytes(4, "big") + x for x in (s, p, idr))
    assert [x[0] for x in vblocks] == [40 * i for i in range(FRAMES)], "a frame every 40 ms"
    assert all(x[1] & 0x80 for x in vblocks), "key frames"
    assert all(x[2] == frame for x in vblocks), "NAL units with 4-byte sizes, the delimiter left out"
    assert audio_bytes == b"".join(struct.pack("<h", x) for x in raw), "every sample, little-endian"
    assert len(find(seg, 0x1C53BB6B, 0xBB)) == FRAMES, "a cue point per video key frame"
    assert len(clusters) == FRAMES, "a cluster per video key frame"

    tags = {uint(dict(dict(t)[0x63C0])[0x63C5]): {dict(st)[0x45A3]: dict(st)[0x4487] for st in find(t, 0x67C8)}
            for t in find(seg, 0x1254C367, 0x7373)}
    by_uid = {uint(dict(x)[0x73C5]): uint(dict(x)[0xD7]) for x in (video, audio)}
    frames = {by_uid[u]: t[b"NUMBER_OF_FRAMES"] for u, t in tags.items()}
    assert frames == {1: str(FRAMES).encode(), 2: b"50"}, "statistics tags: 50 frames, 50 PCM blocks of 40 ms"
    assert "Title #0: done, all checks passed" in log, log


def test_a_track_the_writer_does_not_take_yet_is_left_out_and_said(tmp_path):
    # MPEG-2 video (not written into MKV files yet) and 16-bit stereo LPCM
    disc, raw = lpcm_disc(tmp_path, 2, 3, 1, 2)
    data, log = demux_mkv(disc, tmp_path / "out")
    tracks = find(elements(data), 0x18538067, 0x1654AE6B, 0xAE)
    assert [dict(t)[0x86] for t in tracks] == [b"A_PCM/INT/LIT"]
    assert "Track 0 (mpeg2video): not written into the MKV file: the MKV writer takes H.264 video and LPCM audio " \
           "so far" in log, log
