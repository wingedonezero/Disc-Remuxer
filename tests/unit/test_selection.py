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


def test_all_skips_titles_shorter_than_the_minimum_length_a_number_does_not(tmp_path):
    folder, _ = two_segments(tmp_path, True)
    code, out = run("--set", "titles.min_length=3600", "demux", folder, "all", tmp_path / "all")
    assert code == 2 and "Title #0 is shorter than the minimum length: not ripped with all" in out, out
    assert "No title selected" in out, out
    code, out = run("--set", "titles.min_length=3600", "demux", folder, "0", tmp_path / "named")
    assert code == 0, out
    assert list((tmp_path / "named").rglob("*.m2v"))
