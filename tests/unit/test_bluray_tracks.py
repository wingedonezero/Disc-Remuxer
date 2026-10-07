"""Blu-ray tracks (libavformat/bluray_tracks.c): disc folders built here
(tests/synth/bluray.py) and the tracks of their titles read through the
structure dump: each stream's tracks by its coding type (cores, forced-only
subtitle copies), languages, the title's PIDs only, video first, then audio,
then subtitles, identical streams left out. With the 7 corpus discs the
tracks equal the reference program's info (implementation notes)."""

from helpers import ffi, install_log, lib, logged
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


def tracks(tmp_path, clip_streams, item, name="disc"):
    """The tracks of a playlist title of two clips (both with clip_streams,
    both items with item's stream table): a playlist title keeps only the
    PIDs its items name (a one-clip playlist would give a clip title)."""
    clips = {1: bd.clpi(streams=clip_streams), 2: bd.clpi(streams=clip_streams)}
    first = Item("00001", S, E, videos=item.videos, audio=item.audio, pg=item.pg)
    second = Item("00002", S, E, connection=1, videos=item.videos, audio=item.audio, pg=item.pg)
    t = dump(bd.write(tmp_path / name, bd.bdmv(clips, {1: bd.mpls([first, second])})), min_length=0)
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
    install_log()
    clip = [VIDEO, (0x1100, 0x81, b"\x61eng"), (0x1100, 0x81, b"\x61fra"),
            (0x1200, 0x90, b"eng"), (0x1200, 0x90, b"fra")]
    item = Item(audio=[Stream(0x1100, 0x81, b"\x61eng")], pg=[Stream(0x1200, 0x90, b"eng")])
    got = [short(l)[:2] + (short(l)[4],) for l in tracks(tmp_path, clip, item)]
    assert got == [("0x03", "0x1011", "''"), ("0x41", "0x1100", "'eng'"), ("0xc1", "0x1200", "'eng'"),
                   ("0xc2", "0x1200", "'eng'")]
    assert logged("Audio stream #2 is identical to stream #1 and was skipped")
    assert logged("Subtitle stream #4 is identical to stream #3 and was skipped")


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
