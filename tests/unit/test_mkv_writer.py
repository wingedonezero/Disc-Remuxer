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

from helpers import DATA, PROGRAM, lib
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
    return bd.write(tmp_path / "disc", bdmv_of(bd.source_units(packets), 1)), raw


VATTR, AATTR = b"\x31", bytes([3 << 4 | 1]) + b"jpn"


def bdmv_of(data, nb_clips, marks=()):
    """A playlist playing nb_clips clips of data back to back."""
    streams = ((0x1011, 0x1b, VATTR), (0x1100, 0x80, AATTR))
    clips = {n: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)
             for n in range(1, nb_clips + 1)}
    items = [Item(f"0000{n}", S, E, videos=[Stream(0x1011, 0x1b, VATTR)], audio=[Stream(0x1100, 0x80, AATTR)])
             for n in range(1, nb_clips + 1)]
    files = bd.bdmv(clips, {1: bd.mpls(items, marks=marks)})
    for n in range(1, nb_clips + 1):
        files[f"BDMV/STREAM/0000{n}.m2ts"] = data
    return files


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
           0xBB, 0xB7, 0x1043A770, 0x45B9, 0xB6, 0x80, 0x1254C367, 0x7373, 0x63C0, 0x67C8, 0x4DBB, 0xA0,
           0x6D80, 0x6240, 0x5034}


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
    assert "Track 0 (mpeg2video): not written into the MKV file: the MKV writer takes H.264 video, LPCM, " \
           "AC-3, E-AC-3 and DTS audio and PGS subtitles so far" in log, log


# ---- the writer on its own (through the bridge) ----

def write_mkv(path, tracks, frames, stats=None):
    """frames: (track, bytes, time ns, duration ns, key) in order; the file's bytes (stats: a dict given
    the writer's MkvStats)."""
    from helpers import ffi, lib
    specs = ffi.new("MkvTrackSpec[]", len(tracks))
    keep = []
    for spec, t in zip(specs, tracks):
        for k, v in t.items():
            if isinstance(v, str):
                v = ffi.new("char[]", v.encode())
                keep.append(v)
            setattr(spec, k, v)
    fs = ffi.new("MkvFileSpec *")
    app = ffi.new("char[]", b"no_variable_data")
    fs.muxing_app = fs.writing_app = app
    fs.fixed_ids = 1
    w = ffi.new("MkvWriter **")
    name = ffi.new("char[]", str(path).encode())
    assert lib.mkv_open(w, name, fs, specs, len(tracks)) == 0
    for track, data, t, d, key in frames:
        buf = ffi.from_buffer(data)
        assert lib.mkv_frame(w[0], track, buf, len(data), t, d, key) == 0
    st = ffi.new("MkvStats *")
    assert lib.mkv_close(w, ffi.NULL, 0, 0, st) == 0
    if stats is not None:
        stats.update(late=st.late, cues=st.cues, removed=[st.removed[i] for i in range(st.nb_removed)])
    return path.read_bytes()


def test_a_frame_of_two_field_pictures_goes_into_a_block_per_field(tmp_path):
    # as mkvmerge (and the reference rips) write field-coded H.264: each field
    # picture its own block, at the frame's time and one field later, its own
    # key flag (the IDR field) and discardable flag (nal_ref_idc 0)
    sps = h264.sps(frame_mbs_only=0, timing=(1, 50), hrd=False, pic_struct=False)
    pps = h264.pps()
    frames = []
    for i in range(3):
        top = h264.aud(0 if i == 0 else 1) + (sps + pps if i == 0 else b"") + \
            h264.slice_(h264.SLICE_I if i == 0 else h264.SLICE_P, i, 4 * i, idr=i == 0, field="top")
        bottom = h264.aud(1) + h264.slice_(h264.SLICE_P, i, 4 * i + 1, ref=i != 2, field="bottom")
        frames.append((0, top + bottom, 40000000 * i, 40000000, i == 0))
    data = write_mkv(tmp_path / "f.mkv", [{"codec": 1, "language": "jpn", "width": 64, "height": 48}], frames)
    seg = find(elements(data), 0x18538067)[0]
    v = dict(find(seg, 0x1654AE6B, 0xAE)[0])
    assert uint(v[0x23E383]) == 20000000, "default duration: one field"
    blocks = []
    for c in find(seg, 0x1F43B675):
        ct = uint(dict(c)[0xE7])
        blocks += [(ct + struct.unpack(">h", d[1:3])[0], d[3]) for eid, d in c if eid == 0xA3]
    assert [b[0] for b in blocks] == [0, 20, 40, 60, 80, 100], "a block per field, one field apart"
    assert [bool(b[1] & 0x80) for b in blocks] == [True, False, False, False, False, False], "the IDR field is key"
    assert [bool(b[1] & 0x01) for b in blocks] == [False] * 5 + [True], "the last field's nal_ref_idc is 0"


def test_the_parts_of_a_joined_title_are_files_of_their_own_with_their_own_chapters(tmp_path):
    # two clips played back to back, two chapters in each (at its start and
    # 1 s in): --split writes each clip as an MKV of its own, its chapters
    # from its own start, in the room kept for them before the clusters; the
    # same clip twice gives the same file twice
    import os
    disc, _ = avc_lpcm_disc(tmp_path)
    data = (disc / "BDMV/STREAM/00001.m2ts").read_bytes()
    marks = [(1, 0, S), (1, 0, S + 45000), (1, 1, S), (1, 1, S + 45000)]
    joined = bd.write(tmp_path / "joined", bdmv_of(data, 2, marks))
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "--set", "output.format=mkv", "--set", "titles.min_length=0", "--split", "0",
                        "demux", str(joined), "0", str(out)], capture_output=True, text=True,
                       env={**os.environ, "DR_MKV_NO_VARIABLE_DATA": "1"})
    files = sorted(out.rglob("*.mkv"))
    assert p.returncode == 0 and [f.name for f in files] == ["title_t00_p1.mkv", "title_t00_p2.mkv"], \
        p.stdout + p.stderr
    parts = [f.read_bytes() for f in files]
    assert parts[0] == parts[1], "the same clip: the same file"
    seg = find(elements(parts[0]), 0x18538067)[0]
    ids = [eid for eid, _ in seg]
    assert ids.index(0x1043A770) < ids.index(0x1F43B675), "the chapters in their room before the clusters"
    starts = [uint(dict(a)[0x91]) for a in find(seg, 0x1043A770, 0x45B9, 0xB6)]
    # from its own start; every picture is a key frame, so each chapter is at its mark
    assert starts == [0, 1000000000], "each part's chapters from its own start"


def audio_blocks(seg, track):
    """(time ms, frames) of each block of a track, laced frames split."""
    out = []
    for c in find(seg, 0x1F43B675):
        ct = uint(dict(c)[0xE7])
        for eid, d in c:
            if eid == 0xA0:
                d = dict(d)[0xA1]
            elif eid != 0xA3:
                continue
            tr, k = vint(d, 0)
            if tr == track:
                out.append((ct + struct.unpack(">h", d[k:k + 2])[0], laced_frames(d)))
    return out


def test_ac3_frames_are_packets_of_1536_samples_counted_from_the_first_time(tmp_path):
    ac3 = (DATA / "sine_7680.ac3").read_bytes()
    frames = [ac3[i:i + 768] for i in range(0, len(ac3), 768)]          # 192 kb/s stereo, 48 kHz
    data = write_mkv(tmp_path / "a.mkv", [{"codec": lib.MKV_CODEC_AC3, "language": "eng"}],
                     [(0, f, 32000000 * i, 32000000, 1) for i, f in enumerate(frames)])
    seg = find(elements(data), 0x18538067)[0]
    a = dict(find(seg, 0x1654AE6B, 0xAE)[0])
    aa = dict(a[0xE1])
    assert a[0x86] == b"A_AC3" and uint(a[0x23E383]) == 32000000
    assert (struct.unpack(">f", aa[0xB5])[0], uint(aa[0x9F])) == (48000.0, 2)
    assert 0x6264 not in aa, "no bit depth for AC-3 (as mkvmerge)"
    blocks = audio_blocks(seg, 1)
    assert b"".join(f for _, fs in blocks for f in fs) == ac3, "every frame as it is"
    assert blocks[0][0] == 0 and sum(len(fs) for _, fs in blocks) == len(frames)


def test_dts_takes_its_rate_channels_and_bits_from_the_core_header(tmp_path):
    dts = (DATA / "sine_5120.dts").read_bytes()
    size = ((dts[5] & 3) << 12 | dts[6] << 4 | dts[7] >> 4) + 1        # the core's frame size
    blocks_ = ((dts[4] & 1) << 6 | dts[5] >> 2) + 1                    # its PCM sample blocks
    frames = [dts[i:i + size] for i in range(0, len(dts), size)]
    # rate from the core header's sampling frequency code (core_samplefreqs)
    rate_ = [0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0][(dts[8] >> 2) & 15]
    dur = blocks_ * 32 * 10 ** 9 // rate_
    gap = 10 ** 9                                                      # 1 s missing before frame 5
    data = write_mkv(tmp_path / "d.mkv", [{"codec": lib.MKV_CODEC_DTS, "language": "eng"}],
                     [(0, f, i * dur + (gap if i >= 5 else 0), -1, 1) for i, f in enumerate(frames)])
    seg = find(elements(data), 0x18538067)[0]
    a = dict(find(seg, 0x1654AE6B, 0xAE)[0])
    aa = dict(a[0xE1])
    rate = int(struct.unpack(">f", aa[0xB5])[0])
    assert a[0x86] == b"A_DTS"
    assert uint(a[0x23E383]) == (blocks_ * 32 * 10 ** 10 // rate + 5) // 10, "one frame, to the nearest ns"
    assert uint(aa[0x6264]) in (16, 20, 24)
    got = audio_blocks(seg, 1)
    assert b"".join(f for _, fs in got for f in fs) == dts
    # times counted in samples from the first frame's; a frame more than 1 ms off the count starts it again
    starts, n = [], 0
    for t, fs in got:
        starts.append((n, t))
        n += len(fs)
    assert starts[0] == (0, 0)
    assert (5, round((5 * dur + gap) / 10 ** 6)) in starts, "the gap starts a block at the frame's own time"


def pgs_set(n):
    """A display set: a composition segment (its number n) and the end segment."""
    pcs = bytes([0x07, 0x80, 0x04, 0x38, 0x10, 0, n, 0x80, 0, 0, 0])
    return bytes([0x16, 0, len(pcs)]) + pcs + bytes([0x80, 0, 0])


def pgs_file(tmp_path, zlib, sets, stats=None):
    """AC-3 (2 s, 32 ms frames, given first up to each set's time + 0.5 s) and PGS sets ((time ns, set))."""
    ac3 = (DATA / "sine_7680.ac3").read_bytes()
    one = ac3[:768]
    frames, i, pending = [], 0, list(sets)
    while i * 32000000 < 2 * 10 ** 9 or pending:
        t = i * 32000000
        while pending and (t >= pending[0][0] + 500000000 or t >= 2 * 10 ** 9):
            frames.append((1, pending[0][1], pending[0][0], -1, 1))
            pending.pop(0)
        if t < 2 * 10 ** 9:
            frames.append((0, one, t, 32000000, 1))
        i += 1
    return write_mkv(tmp_path / ("z.mkv" if zlib else "p.mkv"),
                     [{"codec": lib.MKV_CODEC_AC3, "language": "eng"},
                      {"codec": lib.MKV_CODEC_PGS, "language": "fra", "forced": 1, "zlib": zlib}], frames, stats)


def test_pgs_display_sets_are_blocks_at_their_times_compressed_when_asked(tmp_path):
    import zlib
    sets = [(400000000, pgs_set(1)), (1200000000, pgs_set(2))]
    for comp in (0, 1):
        st = {}
        data = pgs_file(tmp_path, comp, sets, st)
        seg = find(elements(data), 0x18538067)[0]
        s = dict(find(seg, 0x1654AE6B, 0xAE)[1])
        assert s[0x86] == b"S_HDMV/PGS" and uint(s[0x83]) == 0x11 and uint(s[0x55AA]) == 1
        assert uint(s[0x9C]) == 0, "no lacing"
        assert (s[0x22B59C], s[0x22B59D]) == (b"fre", b"fr"), "the bibliographic code, the IETF one"
        assert (0x6D80 in s) == bool(comp), "a ContentEncoding (zlib, defaults left out) only when compressed"
        got = audio_blocks(seg, 2)
        assert [t for t, _ in got] == [400, 1200]
        sets_out = [zlib.decompress(fs[0]) if comp else fs[0] for _, fs in got]
        assert sets_out == [p for _, p in sets], "each display set whole, as given"
        # in time order with the audio: each set's block comes after the audio before its time
        order = []
        for c in find(seg, 0x1F43B675):
            ct = uint(dict(c)[0xE7])
            order += [(ct + struct.unpack(">h", d[1:3])[0], d[0] & 0x7F) for eid, d in c if eid == 0xA3]
        assert order == sorted(order, key=lambda x: x[0]), "blocks in time order"
        assert [b for b in order if b[1] == 2] == [(400, 2), (1200, 2)]
        cue_tracks = [uint(dict(dict(p)[0xB7])[0xF7]) for p in find(seg, 0x1C53BB6B, 0xBB)]
        assert cue_tracks.count(2) == 2, "a cue point for every subtitle block"
        assert st["late"] == 0
        # the statistics count the sets' own bytes
        tags = {uint(dict(dict(t)[0x63C0])[0x63C5]): {dict(x)[0x45A3]: dict(x)[0x4487] for x in find(t, 0x67C8)}
                for t in find(seg, 0x1254C367, 0x7373)}
        assert tags[2][b"NUMBER_OF_BYTES"] == str(sum(len(p) for _, p in sets)).encode()


def test_a_display_set_given_long_after_later_audio_is_written_and_counted_late(tmp_path):
    st = {}
    ac3 = (DATA / "sine_7680.ac3").read_bytes()[:768]
    frames = [(0, ac3, i * 32000000, 32000000, 1) for i in range(500)]       # 16 s of audio first
    frames.append((1, pgs_set(1), 1000000000, -1, 1))
    write_mkv(tmp_path / "l.mkv", [{"codec": lib.MKV_CODEC_AC3, "language": "eng"},
                                   {"codec": lib.MKV_CODEC_PGS, "language": "eng"}], frames, st)
    assert st["late"] == 1


def set_bits(frame, pos, n, v):
    b = bytearray(frame)
    for i in range(n):
        bit, p = (v >> (n - 1 - i)) & 1, pos + i
        b[p >> 3] = b[p >> 3] | (0x80 >> (p & 7)) if bit else b[p >> 3] & ~(0x80 >> (p & 7))
    return bytes(b)


def test_dialogue_normalization_removed_only_when_asked_with_the_checksums_made_again(tmp_path):
    # the sample frames say -31 dB (AC-3, stereo: dialnorm at bit 54) and 0 dB (DTS core, encoder version 7:
    # dialnorm at bit 100, no header CRC); set to -27 dB / -5 dB (checksums left wrong), removal writes the
    # "none" level back and makes the AC-3 CRCs again: the frames as they were
    ac3 = (DATA / "sine_7680.ac3").read_bytes()
    ac3_frames = [ac3[i:i + 768] for i in range(0, len(ac3), 768)]
    dts = (DATA / "sine_5120.dts").read_bytes()
    size = ((dts[5] & 3) << 12 | dts[6] << 4 | dts[7] >> 4) + 1
    dts_frames = [dts[i:i + size] for i in range(0, len(dts), size)]
    cases = [(lib.MKV_CODEC_AC3, ac3_frames, 54, 5, 27, 32000000), (lib.MKV_CODEC_DTS, dts_frames, 100, 4, 5, -1)]
    for codec, frames, pos, n, level, dur in cases:
        changed = [set_bits(f, pos, n, level) for f in frames]
        for remove in (0, 1):
            data = write_mkv(tmp_path / f"{codec}_{remove}.mkv",
                             [{"codec": codec, "language": "eng", "remove_dialnorm": remove}],
                             [(0, f, i * 32000000, dur, 1) for i, f in enumerate(changed)])
            got = [f for _, fs in audio_blocks(find(elements(data), 0x18538067)[0], 1) for f in fs]
            assert got == (frames if remove else changed), (codec, remove)


def test_a_track_that_gets_no_frame_leaves_the_track_list_the_others_keep_their_numbers(tmp_path):
    # AC-3, an empty PGS track (a forced-only copy without forced subtitles), a PGS track with two sets: the
    # track list is written again without the empty one, the room after it grows, the file reads to its end
    st = {}
    ac3 = (DATA / "sine_7680.ac3").read_bytes()[:768]
    frames = [(0, ac3, i * 32000000, 32000000, 1) for i in range(40)]
    frames.insert(10, (2, pgs_set(1), 300000000, -1, 1))
    frames.insert(30, (2, pgs_set(2), 900000000, -1, 1))
    for name, tracks_ in (("e.mkv", [{"codec": lib.MKV_CODEC_AC3, "language": "eng"},
                                     {"codec": lib.MKV_CODEC_PGS, "language": "eng", "forced": 1},
                                     {"codec": lib.MKV_CODEC_PGS, "language": "eng"}]),):
        data = write_mkv(tmp_path / name, tracks_, frames, st)
        top = elements(data)                                   # reads every element to the end
        seg = find(top, 0x18538067)[0]
        entries = [dict(e) for e in find(seg, 0x1654AE6B, 0xAE)]
        assert [uint(e[0xD7]) for e in entries] == [1, 3], "the empty track's entry gone, numbers kept"
        ids = [eid for eid, _ in seg]
        assert ids[ids.index(0x1654AE6B) + 1] == 0xEC, "the room after the track list"
        tag_tracks = [uint(dict(dict(t)[0x63C0])[0x63C5]) for t in find(seg, 0x1254C367, 0x7373)]
        assert tag_tracks == [1, 3]
        assert [t for t, _ in audio_blocks(seg, 3)] == [300, 900]
        assert st["removed"] == [1]


def test_an_access_unit_of_two_field_pictures_counts_two():
    sps = h264.sps(frame_mbs_only=0, timing=(1, 50), hrd=False, pic_struct=False)
    pair = h264.aud(0) + sps + h264.pps() + h264.slice_(h264.SLICE_I, 0, 0, idr=True, field="top") + \
        h264.aud(1) + h264.slice_(h264.SLICE_P, 0, 1, field="bottom")
    assert lib.mkv_h264_pictures(pair, len(pair)) == 2
    assert lib.mkv_h264_pictures(h264.aud(0) + SPS + PPS + IDR, len(h264.aud(0) + SPS + PPS + IDR)) == 1


def test_the_timestamp_file_lists_each_picture_s_time_to_the_ns_in_every_mode(tmp_path):
    # 25 fps: picture k at k x 40 ms; each line read back by mkvmerge (cut to the ns) gives the exact ns
    disc, _ = avc_lpcm_disc(tmp_path)
    for fmt in ("streams", "mkv"):
        out = tmp_path / fmt
        p = subprocess.run([*PROGRAM, "--set", f"output.format={fmt}", "--set", "titles.min_length=0", "--set",
                            "output.timestamps_file=true", "demux", str(disc), "0", str(out)],
                           capture_output=True, text=True)
        files = list(out.rglob("*_timestamps.txt"))
        assert p.returncode == 0 and len(files) == 1, p.stdout + p.stderr
        name = files[0].name
        assert name.endswith("_Mpeg4_timestamps.txt") if fmt == "streams" else name == "title_t00_timestamps.txt"
        lines = files[0].read_text().splitlines()
        assert lines[0] == "# timestamp format v4" and len(lines) == FRAMES + 1
        assert [int(float(v) * 1000000) for v in lines[1:]] == [k * 40000000 for k in range(FRAMES)]
        assert f"timestamp file {files[0]} ({FRAMES} pictures)" in p.stdout


def test_the_mkvmerge_mode_muxes_the_stream_files_into_the_same_file_and_deletes_them(tmp_path):
    # the stream files and an options file; mkvmerge @options makes <out>/<title>.mkv with the same frames at the
    # same times as the MKV the writer makes itself (no hole: no timestamp files, so mkvmerge places the video's
    # default duration later in its header); the title's folder is gone afterwards
    import os
    import shutil
    import pytest
    if not shutil.which("mkvmerge"):
        pytest.skip("mkvmerge is not installed")
    disc, _ = avc_lpcm_disc(tmp_path)
    env = {**os.environ, "DR_MKV_NO_VARIABLE_DATA": "1"}
    made = {}
    for fmt in ("mkv", "mkvmerge"):
        out = tmp_path / fmt
        p = subprocess.run([*PROGRAM, "--set", f"output.format={fmt}", "--set", "titles.min_length=0", "demux",
                            str(disc), "0", str(out)], capture_output=True, text=True, env=env)
        assert p.returncode == 0, p.stdout + p.stderr
        made[fmt] = (sorted(f.relative_to(out) for f in out.rglob("*") if f.is_file()), p.stdout)
    files, log = made["mkvmerge"]
    assert [str(f) for f in files if f.suffix != ".log"] == ["title_t00.mkv"], files
    assert "mkvmerge: 2 files into" in log and "the stream files are deleted" in log, log
    assert "mkvmerge: no hole in this title: no timestamp files, the delays as --sync" in log, log
    assert "no timestamp file (" not in log, "LPCM keeps gaps as silence: nothing to say about it"
    a = find(elements((tmp_path / "mkvmerge/title_t00.mkv").read_bytes()), 0x18538067)[0]
    b = find(elements((tmp_path / "mkv/title_t00/title_t00.mkv").read_bytes()), 0x18538067)[0]
    for track in (1, 2):
        assert audio_blocks(a, track) == audio_blocks(b, track), f"track {track}: the same frames at the same times"


def test_truehd_data_counts_its_access_units_as_mkvmerge_makes_packets_of_them():
    # each access unit starts with a check nibble and its length in 16-bit words (12 bits)
    def au(words):
        return bytes([0xF0 | (words >> 8), words & 0xFF]) + bytes(2 * words - 2)
    data = au(10) + au(40) + au(7)
    assert lib.mkv_audio_packets(lib.MKV_CODEC_TRUEHD, data, len(data)) == 3
    assert lib.mkv_audio_packets(lib.MKV_CODEC_TRUEHD, data[:-1], len(data) - 1) == 2, "a cut-off unit is no packet"


def fake_mkvmerge(tmp_path, script):
    """A stand-in mkvmerge (mkv.mkvmerge_path): a shell script."""
    import os
    f = tmp_path / "fake-mkvmerge"
    f.write_text("#!/bin/sh\n" + script)
    os.chmod(f, 0o755)
    return f


def test_mkvmerge_lines_split_at_carriage_returns(tmp_path):
    # mkvmerge separates its progress updates with \r inside one line; a
    # warning after an update must still reach the log
    disc, _ = avc_lpcm_disc(tmp_path)
    fake = fake_mkvmerge(tmp_path, "printf 'Progress: 50%%\\rWarning: after an update\\n'\nexit 1\n")
    p = subprocess.run([*PROGRAM, "--set", "output.format=mkvmerge", "--set", f"mkv.mkvmerge_path={fake}",
                        "--set", "titles.min_length=0", "demux", str(disc), "0", str(tmp_path / "out")],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    assert "mkvmerge: Warning: after an update" in p.stdout, p.stdout
    assert "mkvmerge finished with warnings" in p.stdout, p.stdout
