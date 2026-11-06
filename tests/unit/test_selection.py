"""Title and track selection (src/disc-remuxer/select.c, main.c; the disc
layer's disc_title_open() keep list): every title and track to start with,
--tracks steps take tracks out or put them back, a track left out is not
read or written, track 0 (the video) always stays, demux all skips titles
shorter than titles.min_length. A built DVD title: MPEG-2 video and an
AC-3 track (test_disc_filler.py's)."""

import json
import subprocess

from helpers import PROGRAM
from test_disc_filler import two_segments


def run(*args):
    p = subprocess.run([*PROGRAM, *map(str, args)], capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def tracks(folder, *opts):
    code, out = run(*opts, "--json", "info", folder)
    assert code == 0, out
    title = next(json.loads(line) for line in out.splitlines() if line.startswith("{") and '"type":"title"' in line)
    return [(k["kind"], k["selected"]) for k in title["tracks"]]


def test_every_track_is_selected_to_start_with(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    assert tracks(folder) == [("video", True), ("audio", True)]


def test_steps_take_tracks_out_and_put_them_back_left_to_right(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    assert tracks(folder, "--tracks", "-audio") == [("video", True), ("audio", False)]
    assert tracks(folder, "--tracks", "-all,+eng") == [("video", True), ("audio", True)]
    assert tracks(folder, "--tracks", "-1") == [("video", True), ("audio", False)]
    assert tracks(folder, "--tracks", "0:-1", "--tracks", "+1") == [("video", True), ("audio", True)]
    # a value for another title leaves this one as it is (the disc has one)
    code, out = run("--tracks", "1:-1", "info", folder)
    assert code == 1 and "title #1 does not exist" in out, out


def test_track_0_the_video_always_stays(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    code, out = run("--tracks", "-all", "info", folder)
    assert code == 0 and "track 0, the video, is always ripped" in out, out
    assert tracks(folder, "--tracks", "-video") == [("video", True), ("audio", True)]


def test_a_step_that_is_no_step_stops_the_run(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    for bad in ("audio", "+klingon", "-", "+all,,-audio", "3:"):
        code, out = run("--tracks", bad, "info", folder)
        assert code == 2 and "--tracks" in out, (bad, out)


def test_a_track_left_out_is_not_written_and_the_others_do_not_change(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    code, out = run("demux", folder, "0", tmp_path / "all")
    assert code == 0, out
    code, out = run("--tracks", "-audio", "demux", folder, "0", tmp_path / "video")
    assert code == 0, out
    assert "track 1 (audio ac3, eng) is not selected: left out" in out, out
    assert not list((tmp_path / "video").rglob("*.ac3"))
    assert next((tmp_path / "video").rglob("*.m2v")).read_bytes() == next((tmp_path / "all").rglob("*.m2v")).read_bytes()


def test_titles_shorter_than_the_minimum_length_are_left_out_of_the_title_list(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    code, out = run("--set", "titles.min_length=3600", "info", folder)
    assert "less than the minimum length of 3600 s" in out and "left out" in out, out
    assert ", 0 titles" in out, out
    # its number no longer names it: the list has no title 0
    code, out = run("--set", "titles.min_length=3600", "demux", folder, "0", tmp_path / "named")
    assert code != 0 and not list((tmp_path / "named").rglob("*.m2v")), out
    # without a minimum length it is title 0
    code, out = run("--set", "titles.min_length=0", "demux", folder, "0", tmp_path / "all")
    assert code == 0 and list((tmp_path / "all").rglob("*.m2v")), out


def test_an_option_after_the_command_or_an_extra_argument_stops_the_run(tmp_path):
    # options go before the command: one after it was ignored before (a demux
    # with "--split 0" at the end ripped the title whole); an extra argument
    # likewise; the arguments are checked before a disc is opened
    disc = tmp_path / "disc"
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, "demux", str(disc), "0", str(out), "--split", "0"], capture_output=True, text=True)
    assert p.returncode == 2 and not out.exists(), p.stdout + p.stderr
    assert "option --split after the command demux: options go before the command" in p.stderr, p.stderr
    p = subprocess.run([*PROGRAM, "info", str(disc), "extra"], capture_output=True, text=True)
    assert p.returncode == 2 and "info takes at most 1 argument: extra is one too many" in p.stderr, p.stderr


def test_a_track_number_takes_its_forced_subtitles_along(tmp_path):
    # test_stream_names.disc: video, two AC-3, PGS (3) with its forced-only copy (4)
    from test_stream_names import disc
    folder = disc(tmp_path)
    keep = lambda *opts: [k for _, k in tracks(folder, *opts)]
    assert keep() == [True] * 5
    assert keep("--tracks", "-3") == [True, True, True, False, False]
    assert keep("--tracks", "-3,+4") == [True, True, True, False, True]    # the forced copy alone
    assert keep("--tracks", "-3", "--tracks", "+3") == [True] * 5
    # a step by kind or language names both on its own
    assert keep("--tracks", "0:-subtitle") == [True, True, True, False, False]
    assert keep("--tracks", "-forced") == [True, True, True, True, False]


def audio_disc(tmp_path, ac3, dts, thd):
    """test_bluray_details.disc with these audio languages: AC-3, DTS (lossy:
    no DTS-HD extension), TrueHD (lossless) with its AC-3 core."""
    from test_bluray_details import AC3, DTS, M2V, S, E, THD, frames, stream_packets
    from synth import bluray as bd
    from synth.bluray import Item, Stream
    packets = (stream_packets(0x1011, frames(M2V, 60000), 0xe0) +
               stream_packets(0x1100, frames(AC3, 768), ext=0x71) +
               stream_packets(0x1101, frames(DTS, 1884), ext=0x71) +
               stream_packets(0x1102, frames(THD, 600), ext=0x72) +
               stream_packets(0x1102, frames(AC3, 768), ext=0x76))
    data = bd.source_units(packets)
    audio = [Stream(0x1100, 0x81, b"\x31" + ac3), Stream(0x1101, 0x82, b"\x31" + dts),
             Stream(0x1102, 0x83, b"\x31" + thd)]
    streams = ((0x1011, 0x02, b"\x31"),) + tuple((a.pid, a.coding, a.attr) for a in audio)
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")], audio=audio)
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def kept(folder, *opts):
    code, out = run(*opts, "--json", "info", folder)
    assert code == 0, out
    title = next(json.loads(line) for line in out.splitlines() if line.startswith('{"type":"title"'))
    return [(k["description"], k["language"], k["selected"]) for k in title["tracks"]]


def test_lossless_is_the_lossless_audio_without_its_cores(tmp_path):
    folder = audio_disc(tmp_path, b"eng", b"fre", b"eng")
    assert [(d, l) for d, l, _ in kept(folder)] == [
        ("Mpeg2", "eng"), ("Dolby Digital", "eng"), ("DTS", "fre"), ("TrueHD", "eng"), ("Dolby Digital", "eng")]
    keep = lambda *opts: [k for _, _, k in kept(folder, *opts)]
    assert keep("--tracks", "-audio,+lossless") == [True, False, False, True, False], "TrueHD alone, not its core"
    assert keep("--tracks", "-lossless") == [True, True, True, False, True]
    assert keep("--tracks", "-audio,+lossless=eng") == [True, False, False, True, False]


def test_lossless_without_lossless_audio_in_its_languages_is_all_their_audio(tmp_path):
    folder = audio_disc(tmp_path, b"eng", b"fre", b"eng")
    keep = lambda *opts: [k for _, _, k in kept(folder, *opts)]
    # the French audio is lossy only: all of it
    assert keep("--tracks", "-audio,+lossless=fre") == [True, False, True, False, False]
    # a title without lossless audio (test_stream_names.disc: two AC-3 and a
    # PGS with its forced copy): every audio track
    from test_stream_names import disc
    lossy = disc(tmp_path / "lossy")
    assert [k for _, k in tracks(lossy, "--tracks", "-audio,+lossless")] == [True] * 5


def test_lossless_in_languages_the_title_has_no_audio_in_is_as_if_not_limited(tmp_path):
    folder = audio_disc(tmp_path, b"eng", b"fre", b"eng")
    keep = lambda *opts: [k for _, _, k in kept(folder, *opts)]
    assert keep("--tracks", "-audio,+lossless=ger") == [True, False, False, True, False]
    # and without lossless audio: every audio track (Spanish and undetermined AC-3)
    from test_stream_names import disc
    lossy = disc(tmp_path / "lossy")
    assert [k for _, k in tracks(lossy, "--tracks", "-audio,+lossless=eng")] == [True] * 5
    # a kind of track limited so: none of it
    assert [k for _, k in tracks(lossy, "--tracks", "-audio,+audio=eng")] == [True, False, False, True, True]


def test_a_kind_of_track_limited_to_languages(tmp_path):
    folder = audio_disc(tmp_path, b"eng", b"fre", b"spa")
    keep = lambda *opts: [k for _, _, k in kept(folder, *opts)]
    assert keep("--tracks", "-audio,+audio=eng") == [True, True, False, False, False]
    assert keep("--tracks", "-audio,+audio=eng/fre") == [True, True, True, False, False]
    # codes in either form and English names, in any case
    for same in ("en", "ENG", "English", "english", "Castilian/en"):
        assert keep("--tracks", "-audio,+audio=" + same)[1] is True, same
    assert keep("--tracks", "-audio,+audio=Castilian") == [True, False, False, True, True]
    assert keep("--tracks", "-all=spa") == [True, True, True, False, False]


def test_a_language_limit_that_is_no_list_of_languages_stops_the_run(tmp_path):
    folder = audio_disc(tmp_path, b"eng", b"fre", b"eng")
    for bad, why in (("+audio=elvish", "'elvish' is no language"), ("+audio=", "is no list of languages"),
                     ("+audio=eng//fre", "is no list of languages"), ("+eng=fre", "is no step"),
                     ("+2=eng", "is no step"), ("+audio=Greek", "'Greek' is no language")):
        code, out = run("--tracks", bad, "info", folder)
        assert code == 2 and why in out, (bad, out)


def test_subtitles_limited_to_languages_take_their_forced_copies_along(tmp_path):
    from test_stream_names import disc
    folder = disc(tmp_path)
    keep = lambda *opts: [k for _, k in tracks(folder, *opts)]
    assert keep("--tracks", "-subtitle,+subtitle=English") == [True] * 5
    assert keep("--tracks", "-subtitle,+subtitle=fre") == [True, True, True, False, False]
