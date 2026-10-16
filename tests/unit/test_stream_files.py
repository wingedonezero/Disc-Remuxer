"""The stream files a rip writes (src/disc-remuxer/writer.c), byte for
byte: LPCM as RIFF WAVE (16-bit stereo: WAVE_FORMAT_PCM, the 44-byte
header; more channels or bits: WAVE_FORMAT_EXTENSIBLE with the channel mask,
the 68-byte header), the samples little-endian in the WAVE order; DVD
sub-pictures as a VobSub pair (.sub: each unit in 2048-byte packs, the PTS
in its first; .idx: the header, the language, a timestamp and file position
per unit). Built discs (tests/synth)."""

import json
import struct
import subprocess

from helpers import DATA, PROGRAM
from helpers.disctl import pictures
from helpers.dvdnav import Pgc, build, cell
from synth import bluray as bd
from synth import dvd
from synth.bluray import Item, Stream
from test_dvd_streams import put
from test_dvd_titles import ac3, subp

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
S, E = 27000000, 27000000 + 90000 * 4


def le(b, at, n):
    return int.from_bytes(b[at:at + n], "little")


def demux(disc, out):
    p = subprocess.run([*PROGRAM, "--json", "demux", str(disc), "0", str(out)], capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    result = [r for r in recs if r.get("type") == "title_result"]
    assert p.returncode == 0 and result and result[0]["status"] == "ok", p.stdout + p.stderr
    return recs


# ---- LPCM into WAVE (a built Blu-ray: MPEG-2 video and one LPCM track) ----

def lpcm_disc(tmp_path, channels, assignment, bits_code, sample_bytes, units=800, samples=240):
    """Blu-ray LPCM payloads (the 4-byte header: size, channel assignment +
    48 kHz, bits) of samples x channels big-endian words; returns the disc and
    every sample in disc order."""
    pk, raw = [], []
    for k in range(units):
        frame = []
        for i in range(samples):
            frame.append([((i * 37 + k * 11 + c * 101) % 3000 - 1500) for c in range(channels)])
        raw += frame
        body = b"".join(v.to_bytes(sample_bytes, "big", signed=True) for f in frame for v in f)
        pk += bd.ts_packets(0x1100, bd.pes(0xbd, struct.pack(">H", len(body)) + bytes([assignment << 4 | 1,
                                                                                        bits_code << 6]) + body,
                                           pts=900 + 450 * k))
    video = []
    for i, c in enumerate([M2V[j:j + 60000] for j in range(0, len(M2V), 60000)]):
        video += bd.ts_packets(0x1011, bd.pes(0xe0, c, pts=900 + 3003 * i))
    data = bd.source_units(video + pk)
    attr = bytes([(3 if channels == 2 else 6) << 4 | 1]) + b"eng"
    streams = ((0x1011, 0x02, b"\x31"), (0x1100, 0x80, attr))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")], audio=[Stream(0x1100, 0x80, attr)])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files), raw


def wav_of(out):
    wav = list(out.rglob("*.wav"))
    assert len(wav) == 1, list(out.rglob("*"))
    return wav[0].read_bytes()


def test_16_bit_stereo_pcm_goes_into_a_plain_wave_file(tmp_path):
    disc, raw = lpcm_disc(tmp_path, 2, 3, 1, 2)
    demux(disc, tmp_path / "out")
    w = wav_of(tmp_path / "out")
    data = b"".join(struct.pack("<h", v) for f in raw for v in f)
    assert w[0:4] == b"RIFF" and le(w, 4, 4) == 36 + len(data) and w[8:16] == b"WAVEfmt "
    assert (le(w, 16, 4), le(w, 20, 2), le(w, 22, 2)) == (16, 1, 2)
    assert (le(w, 24, 4), le(w, 28, 4), le(w, 32, 2), le(w, 34, 2)) == (48000, 192000, 4, 16)
    assert w[36:40] == b"data" and le(w, 40, 4) == len(data)
    assert w[44:] == data


def test_24_bit_5_1_pcm_takes_the_extensible_header_and_the_wave_order(tmp_path):
    # channel assignment 9: L R C Ls Rs LFE on the disc; the WAVE order puts LFE fourth
    disc, raw = lpcm_disc(tmp_path, 6, 9, 3, 3, units=400)
    demux(disc, tmp_path / "out")
    w = wav_of(tmp_path / "out")
    order = [0, 1, 2, 5, 3, 4]
    data = b"".join(f[c].to_bytes(3, "little", signed=True) for f in raw for c in order)
    assert (le(w, 16, 4), le(w, 20, 2), le(w, 22, 2)) == (40, 0xFFFE, 6)
    assert (le(w, 24, 4), le(w, 28, 4), le(w, 32, 2), le(w, 34, 2)) == (48000, 48000 * 18, 18, 24)
    assert (le(w, 36, 2), le(w, 38, 2)) == (22, 24)
    assert le(w, 40, 4) == 0x60F                                  # 5.1(side): FL FR FC LFE SL SR
    assert w[44:60] == bytes([1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71])
    assert w[60:64] == b"data" and le(w, 64, 4) == len(data) and len(w) == 68 + len(data)
    assert w[68:] == data


# ---- sub-pictures into a VobSub pair (a built DVD) ----

def spu(pad):
    """A 2 x 2 sub-picture unit shown from its PTS for about 1 s (its second
    control sequence's delay, 88 x 1024 / 90000 s), pad bytes between the
    run-length data and the control sequences."""
    rle = bytes([0x80, 0x80]) + bytes(pad)              # one line per field: 2 pixels of colour 0
    ctrl = 4 + len(rle)
    second = ctrl + 4 + 3 + 3 + 7 + 5 + 1 + 1
    seq1 = struct.pack(">HH", 0, second) + bytes([0x03, 0x32, 0x10, 0x04, 0xff, 0xf0,
                                                  0x05, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01,
                                                  0x06]) + struct.pack(">HH", 4, 5) + bytes([0x01, 0xff])
    seq2 = struct.pack(">HH", 88, second) + bytes([0x02, 0xff])
    body = rle + seq1 + seq2
    return struct.pack(">HH", 4 + len(body), ctrl) + body


def test_sub_pictures_go_into_a_vobsub_pair(tmp_path):
    base = 90000
    pics = pictures(M2V)
    timed = [(a, b, disp, typ, base + disp * 3003) for a, b, disp, typ in pics]
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    audio = [(frames[k % len(frames)], base + k * 2880) for k in range(len(pics) * 3003 // 2880)]
    units = [spu(0), spu(3000)]                          # the second over two packs on the disc and in .sub
    subs = [(units[0], base + 90000), (units[1], base + 270000)]
    sectors, navs = dvd.mux_vobus(M2V, timed, 0, audio, base - 3003, subs=subs)
    total = len(sectors)
    p = Pgc(cells=[cell(0, total - 1, len(pics) * 1001 // 30000, last_vobu=navs[-1].lbn)],
            audio_control=bytes([0x80, 0]) + bytes(14), subp_control=bytes([0x80, 0, 0, 0]) + bytes(124))
    folder = build(tmp_path, [p], [[(1, 1)]], navs, total, video_attr=bytes([0x40, 0]), audio_attrs=[ac3(b"en")],
                   subp_attrs=[subp(b"en")])
    for k, sec in enumerate(sectors):
        put(folder, k, sec)
    demux(folder, tmp_path / "out")
    sub = list((tmp_path / "out").rglob("*.sub"))
    idx = list((tmp_path / "out").rglob("*.idx"))
    assert len(sub) == 1 and len(idx) == 1, list((tmp_path / "out").rglob("*"))
    data = sub[0].read_bytes()
    assert len(data) == 3 * 2048                         # 1 + 2 packs
    packs = [data[i:i + 2048] for i in range(0, len(data), 2048)]
    assert all(pk[:4] == b"\x00\x00\x01\xba" and pk[14:18] == b"\x00\x00\x01\xbd" for pk in packs)
    assert [pk[21] & 0x80 for pk in packs] == [0x80, 0x80, 0]            # a PTS in each unit's first pack
    got = b""
    for pk in packs:                                     # the payload after the PES header and sub-stream 0x20
        n, h = int.from_bytes(pk[18:20], "big"), pk[22]
        assert pk[23 + h] == 0x20
        got += pk[24 + h:20 + n]
    assert got == units[0] + units[1]
    text = idx[0].read_text()
    assert text.startswith("# VobSub index file, v7 (do not modify this line!)\n")
    lines = [l for l in text.splitlines() if l.startswith(("id:", "timestamp:"))]
    assert lines[0].startswith("id: ") and lines[0].endswith(", index: 0"), text
    # placed on the title timeline (1 s and 3 s of disc time after the first picture)
    assert lines[1:] == ["timestamp: 00:00:01:001, filepos: 000000000",
                         "timestamp: 00:00:03:003, filepos: 000000800"], text
