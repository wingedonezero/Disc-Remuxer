"""Blu-ray tracks (libavformat/bluray_tracks.c): disc folders built here
(tests/synth/bluray.py) and the tracks of their titles read through the
structure dump: each stream's tracks by its coding type (cores, forced-only
subtitle copies), languages, the title's PIDs only, video first, then audio,
then subtitles (identical streams: the disc layer's stream pass, tested
here through the program's info). With the 7 corpus discs the
tracks equal the reference program's info (implementation notes)."""

import json
import subprocess

from helpers import PROGRAM, ffi, install_log, lib, logged
from synth import bluray as bd
from synth.bluray import Item, Stream


def dump(path, min_length=0):
    """The structure dump with the titles and their tracks, or the error."""
    err = ffi.new("int *")
    p = lib.tb_bluray_dump(str(path).encode(), ffi.NULL, min_length, err)
    if p == ffi.NULL:
        return err[0]
    try:
        return ffi.string(p).decode()
    finally:
        lib.free(p)


def lines(text, prefix):
    return [l for l in text.splitlines() if l.startswith(prefix)]

VIDEO = (0x1011, 0x1b, b"\x61")


S, E = 27000000, 27000000 + 90000          # the built clips' STC sequence (2 s)


def core_units(clip_streams):
    """Stream data in which every TrueHD / E-AC-3 / DTS-HD stream shows its
    core substream (the probe then keeps the core track)."""
    packets = []
    for pid, coding, _ in clip_streams:
        exts = (0x72, 0x76) if coding == 0x83 else (0x71, 0x72) if coding in (0x84, 0xa1, 0x85, 0x86, 0xa2) else ()
        packets += [bd.ts_packet(pid, bd.pes(0xfd, bytes(16), ext=x, pts=900 + i), pusi=True)
                    for i in range(3) for x in exts]
    return bd.source_units(packets or [bd.ts_packet(0x1fff, bytes(184))])


def tracks(tmp_path, clip_streams, item, name="disc"):
    """The tracks of a playlist title of two clips (both with clip_streams,
    both items with item's stream table): a playlist title keeps only the
    PIDs its items name (a one-clip playlist would give a clip title)."""
    data = core_units(clip_streams)
    clips = {1: bd.clpi(packets=len(data) // 192, streams=clip_streams),
             2: bd.clpi(packets=len(data) // 192, streams=clip_streams)}
    first = Item("00001", S, E, videos=item.videos, audio=item.audio, pg=item.pg)
    second = Item("00002", S, E, connection=1, videos=item.videos, audio=item.audio, pg=item.pg)
    files = bd.bdmv(clips, {1: bd.mpls([first, second])})
    files["BDMV/STREAM/00001.m2ts"] = files["BDMV/STREAM/00002.m2ts"] = data
    t = dump(bd.write(tmp_path / name, files), min_length=0)
    assert isinstance(t, str), f"open failed: {t}"
    out, mine = [], False
    for l in t.splitlines():
        if l.startswith("title "):
            mine = l.split()[2] == "00001.mpls"
        elif l.startswith("  track ") and mine:
            out.append(l.strip())
    return out


def short(line):
    """'track N kind 0x.. ... lang 'x' ... from F inner I' -> the fields compared."""
    f = line.split()
    return f[3], f[5], f[7] + " " + f[8], f[10], f[12], int(f[16]), int(f[18])


def test_each_stream_gives_its_tracks_video_first_then_audio_then_subtitles(tmp_path):
    clip = [VIDEO,
            (0x1100, 0x83, b"\x61eng"),      # TrueHD + AC-3
            (0x1101, 0x86, b"\x61fra"),      # DTS-HD MA + DTS core
            (0x1200, 0x90, b"eng"),          # PGS + forced copy
            (0x1102, 0x81, b"\x61deu"),      # AC-3
            (0x1103, 0x84, b"\x61spa"),      # E-AC-3 + AC-3
            (0x1104, 0x80, b"\x61ita")]      # LPCM, not in the playlist
    item = Item(audio=[Stream(p, c, a) for p, c, a in clip[1:3] + clip[4:6]], pg=[Stream(0x1200, 0x90, b"eng")])
    got = [short(l) for l in tracks(tmp_path, clip, item)]
    assert got == [
        ("0x03", "0x1011", "0x0 0x0", "0x0", "''", -1, -1),
        ("0x4b", "0x1100", "0x72 0x0", "0x400", "'eng'", -1, 2),
        ("0x41", "0x1100", "0x76 0x0", "0x900", "'eng'", 1, -1),
        ("0x49", "0x1101", "0x71 0x72", "0x400", "'fra'", -1, 4),
        ("0x43", "0x1101", "0x71 0x0", "0x900", "'fra'", 3, -1),
        ("0x41", "0x1102", "0x71 0x0", "0x0", "'deu'", -1, -1),
        ("0x4e", "0x1103", "0x71 0x72", "0x400", "'spa'", -1, 7),
        ("0x41", "0x1103", "0x71 0x0", "0x900", "'spa'", 6, -1),
        ("0xc1", "0x1200", "0x0 0x0", "0x0", "'eng'", -1, 9),
        ("0xc2", "0x1200", "0x0 0x0", "0x1800", "'eng'", 8, -1),
    ]


def test_the_video_is_the_first_stream_that_describes_one(tmp_path):
    clip = [(0x1100, 0x81, b"\x61eng"), (0x1012, 0xea, b"\x61"), VIDEO]
    item = Item(videos=[Stream(0x1012, 0xea, b"\x61")], audio=[Stream(0x1100, 0x81, b"\x61eng")])
    got = [short(l)[:2] for l in tracks(tmp_path, clip, item)]
    assert got == [("0x02", "0x1012"), ("0x41", "0x1100")], "VC-1 (the title's video PID); the AVC PID is not kept"


def test_a_title_without_a_video_stream_has_no_tracks(tmp_path):
    install_log()
    clip = [(0x1100, 0x81, b"\x61eng")]
    item = Item(videos=[Stream(0x1011, 0x1b, b"\x61")], audio=[Stream(0x1100, 0x81, b"\x61eng")])
    assert tracks(tmp_path, clip, item) == []
    assert logged("has no video stream")


def test_an_identical_audio_or_subtitle_stream_is_left_out(tmp_path):
    # both streams of a PID are described; the disc layer's stream pass
    # (discpass.c) leaves out the later one of the same codec, PID and keys
    clip = [VIDEO, (0x1100, 0x81, b"\x61eng"), (0x1100, 0x81, b"\x61fra"),
            (0x1200, 0x90, b"eng"), (0x1200, 0x90, b"fra")]
    item = Item(audio=[Stream(0x1100, 0x81, b"\x61eng")], pg=[Stream(0x1200, 0x90, b"eng")])
    got = [short(l)[:2] + (short(l)[4],) for l in tracks(tmp_path, clip, item)]
    assert got == [("0x03", "0x1011", "''"), ("0x41", "0x1100", "'eng'"), ("0x41", "0x1100", "'fra'"),
                   ("0xc1", "0x1200", "'eng'"), ("0xc2", "0x1200", "'eng'"), ("0xc1", "0x1200", "'fra'"),
                   ("0xc2", "0x1200", "'fra'")], "the format part describes both"
    packets = [bd.ts_packet(0x1011, bd.pes(0xe0, bytes(16), pts=900), pusi=True),
               bd.ts_packet(0x1100, bd.pes(0xfd, bytes(16), ext=0x71, pts=900), pusi=True),
               bd.ts_packet(0x1200, bd.pes(0xbd, bytes(16), pts=900), pusi=True)]
    data = bd.source_units(packets)
    clips = {1: bd.clpi(packets=len(data) // 192, streams=clip)}
    files = bd.bdmv(clips, {1: bd.mpls([Item("00001", S, E, videos=item.videos, audio=item.audio, pg=item.pg)])})
    files["BDMV/STREAM/00001.m2ts"] = data
    out = subprocess.run([*PROGRAM, "--json", "info", str(bd.write(tmp_path / "pass", files))],
                         capture_output=True, text=True)
    recs = [json.loads(l) for l in out.stdout.splitlines() if l.startswith("{")]
    t = [r for r in recs if r.get("type") == "title"][0]
    assert [(k["kind"], k["codec"], k["language"], k["forced"]) for k in t["tracks"]] == [
        ("video", "Mpeg4", "", False), ("audio", "DD", "eng", False), ("subtitle", "PGS", "eng", False),
        ("subtitle", "PGS", "eng", True)]
    texts = [r.get("text", "") for r in recs if r.get("type") == "message"]
    assert "Blu-ray: Audio stream #2 is identical to stream #1 and was skipped" in texts
    assert "Blu-ray: Subtitle stream #4 is identical to stream #3 and was skipped" in texts


def test_streams_without_a_track_kind(tmp_path):
    # private data without a kind from the program map, an unknown coding type
    clip = [VIDEO, (0x1100, 0x06, b"\x61eng"), (0x1101, 0x77, b"\x61eng"), (0x1102, 0x82, b"\x61")]
    item = Item(audio=[Stream(p, c, a) for p, c, a in clip[1:]])
    got = [short(l)[:2] + (short(l)[4],) for l in tracks(tmp_path, clip, item)]
    assert got == [("0x03", "0x1011", "''"), ("0x43", "0x1102", "''")], "DTS without a language"


def test_audio_kinds_and_their_codecs(tmp_path):
    kinds = {0x03: "0x40", 0x04: "0x40", 0x0f: "0x45", 0x11: "0x45", 0x80: "0x44", 0x81: "0x41", 0x82: "0x43",
             0xa1: "0x4e", 0xa2: "0x49"}
    clip = [VIDEO] + [(0x1100 + i, c, b"\x61eng") for i, c in enumerate(kinds)]
    item = Item(audio=[Stream(p, c, a) for p, c, a in clip[1:]])
    got = [short(l) for l in tracks(tmp_path, clip, item)][1:]
    main = [g for g in got if g[5] < 0]
    assert [g[0] for g in main] == list(kinds.values())
    # secondary E-AC-3 / DTS-HD: their tracks carry the secondary-audio mark (0x200)
    assert [g[3] for g in got if g[1] in ("0x1107", "0x1108")] == ["0x600", "0xb00", "0x600", "0xb00"]


# ---- the stream probes (stream data built as TS packets) ----

def probed(tmp_path, clip_streams, packets, name="probe"):
    """The tracks of the clip title of one clip whose stream file holds packets."""
    data = bd.source_units(packets)
    files = bd.bdmv({1: bd.clpi(packets=len(data) // 192, streams=clip_streams)}, {})
    files["BDMV/STREAM/00001.m2ts"] = data
    t = dump(bd.write(tmp_path / name, files), min_length=0)
    assert isinstance(t, str), f"open failed: {t}"
    return [short(l.strip()) for l in lines(t, "  track ")]


def video_packets():
    return [bd.ts_packet(0x1011, bd.pes(0xe0, bytes(20), pts=900), pusi=True)]


def test_e_ac3_and_dts_hd_without_a_core_substream_give_one_track(tmp_path):
    clip = [VIDEO, (0x1100, 0x84, b"\x61eng"), (0x1101, 0x86, b"\x61fra")]
    # only the main substreams (stream_id_extension 0x72), three times each
    packets = video_packets() + [bd.ts_packet(pid, bd.pes(0xfd, bytes(16), ext=0x72, pts=900 + i), pusi=True)
                                 for i in range(3) for pid in (0x1100, 0x1101)]
    got = [g[:2] + (g[3],) for g in probed(tmp_path, clip, packets)]
    assert got == [("0x03", "0x1011", "0x0"), ("0x4d", "0x1100", "0x0"), ("0x48", "0x1101", "0x0")]
    # with the core substreams (0x71): main + core
    packets = video_packets() + [bd.ts_packet(pid, bd.pes(0xfd, bytes(16), ext=x, pts=900 + i), pusi=True)
                                 for i in range(3) for pid in (0x1100, 0x1101) for x in (0x71, 0x72)]
    got = [g[0] for g in probed(tmp_path, clip, packets, "core")]
    assert got == ["0x03", "0x4e", "0x41", "0x49", "0x43"]


def test_avc_video_on_another_pes_stream_id(tmp_path):
    clip = [VIDEO, (0x1100, 0x81, b"\x61eng")]
    packets = [bd.ts_packet(0x1011, bd.pes(0xe1, bytes(20), pts=900 + i), pusi=True) for i in range(3)]
    got = probed(tmp_path, clip, packets)
    assert got[0][:3] == ("0x03", "0x1011", "0x1 0x0"), "PES stream id 0xE1: the video takes stream id offset 1"
    packets = [bd.ts_packet(0x1011, bd.pes(0xe0, bytes(20), pts=900), pusi=True)]
    assert probed(tmp_path, clip, packets, "e0")[0][2] == "0x0 0x0"


def hevc_es(nal_types):
    return b"".join(b"\x00\x00\x00\x01" + bytes([t << 1, 1]) + bytes(8) for t in nal_types)


def test_hevc_with_a_dolby_vision_second_layer(tmp_path):
    clip = [(0x1011, 0x24, b"\x61"), (0x1015, 0x24, b"\x61")]
    base = [bd.ts_packet(0x1011, bd.pes(0xe0, hevc_es([32, 33, 34, 19]), pts=900), pusi=True)]
    el = hevc_es([32, 33, 34, 62, 19] + [33] * 5 + [34])
    got = probed(tmp_path, clip, base + [bd.ts_packet(0x1015, bd.pes(0xe1, el, pts=900), pusi=True)])
    assert got == [("0x05", "0x1011", "0x1015 0x0", "0x10000", "''", -1, -1)]
    # the parameter sets are complete (6 SPS, a PPS, a VPS) before the RPU: not seen, no second layer
    el = hevc_es([32] + [33] * 6 + [34, 62, 19])
    got = probed(tmp_path, clip, base + [bd.ts_packet(0x1015, bd.pes(0xe1, el, pts=900), pusi=True)], "late_rpu")
    assert got == [("0x05", "0x1011", "0x0 0x0", "0x0", "''", -1, -1)]


def test_a_title_whose_streams_cannot_be_probed_is_skipped(tmp_path):
    install_log()
    clip = [VIDEO, (0x1100, 0x83, b"\x61eng")]
    # a PES header that says it has a PTS in a 12-byte payload: too short to read
    bad = bd.ts_packet(0x1100, bytes([0, 0, 1, 0xfd, 0, 0, 0x80, 0x80, 0, 0, 0, 0]), pusi=True)
    assert probed(tmp_path, clip, video_packets() + [bad]) == []
    assert logged("a PES header cannot be read")
    assert logged("title 00001.m2ts was skipped")


# ---- the repair of a clip's stream table ----

def repaired(tmp_path, packets, name):
    """A playlist part of one PlayItem (with chapters, so it is a playlist
    title) naming an AC-3 stream (PID 0x1100) its clip's information lacks;
    the tracks of the playlist title."""
    data = bd.source_units(packets)
    clips = {1: bd.clpi(packets=len(data) // 192, streams=[VIDEO])}
    item = Item("00001", S, E, audio=[Stream(0x1100, 0x81, b"\x61deu")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=[(1, 0, S), (1, 0, S + 1000)])})
    files["BDMV/STREAM/00001.m2ts"] = data
    t = dump(bd.write(tmp_path / name, files), min_length=0)
    assert isinstance(t, str), t
    out, mine = [], False
    for l in t.splitlines():
        if l.startswith("title "):
            mine = l.split()[2] == "00001.mpls"
        elif l.startswith("  track ") and mine:
            out.append(short(l.strip())[:2] + (short(l.strip())[4],))
    return out


def test_a_stream_the_playlist_names_and_the_clip_lacks_is_added_when_its_data_is_there(tmp_path):
    install_log()
    audio = [bd.ts_packet(0x1100, bd.pes(0xbd, bytes(16), pts=900 + i), pusi=True) for i in range(5)]
    assert repaired(tmp_path, video_packets() + audio, "data") == [("0x03", "0x1011", "''"),
                                                                     ("0x41", "0x1100", "'deu'")]
    assert logged("clip 00001 lacks the stream on PID 0x1100 its playlist names")
    install_log()
    assert repaired(tmp_path, video_packets(), "nodata") == [("0x03", "0x1011", "''")]
    assert not logged("lacks the stream")
