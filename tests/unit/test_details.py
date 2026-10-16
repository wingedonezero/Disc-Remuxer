"""What the GUI shows, on the command line (src/disc-remuxer/views.c,
info --details): the queue's lines for the disc, its titles, chapters and
tracks, and the details panel's rows for the disc and each title with its
tracks — the same words the GUI lays out, so a test here is a test of the
GUI's text. A built Blu-ray (test_stream_names.disc): MPEG-2 video, AC-3 in
Spanish, AC-3 with no language, PGS in English with a forced-only copy."""

import subprocess

from helpers import PROGRAM
from test_stream_names import disc


def details(tmp_path):
    p = subprocess.run([*PROGRAM, "--details", "info", str(disc(tmp_path))], capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    out = p.stdout.splitlines()
    return out[out.index("=== Details (what the GUI shows) ===") + 1:]


def test_details_give_the_queue_lines_and_the_details_rows(tmp_path):
    out = details(tmp_path)
    assert out[0] == "Queue: Blu-ray   | 1 title"
    assert "[Disc information]" in out and "[Protection]" in out
    assert "  AACS: no" in out                              # a protection property under its own label
    assert "Queue:   Video #0  Mpeg2  64x48 4:3 29.97 (30000/1001)  English" in out
    assert "Queue:   Subtitle #4  HDMV PGS Subtitles    forced only  English" in out
    assert "[Title information: #0]" in out


def test_a_track_s_rows_say_what_its_file_becomes_and_how_it_is_linked(tmp_path):
    out = details(tmp_path)
    at = out.index("  -- Track #4: Subtitle HDMV PGS Subtitles  (forced subtitles of track 3)")
    rows = out[at + 1:at + 9]
    assert "    Codec ID: S_HDMV/PGS" in rows
    assert "    Language: eng (English)" in rows
    assert "    Flags: the forced subtitles of track 3" in rows
    assert "    Output: .sup: as on the disc (copied, lossless)" in rows
    audio = out.index("  -- Track #1: Audio Dolby Digital  stereo 48000 Hz 192 Kb/s")
    assert "    Name: Stereo" in out[audio + 1:audio + 12]
    assert "    Language: spa (Spanish; Castilian)" in out[audio + 1:audio + 12]
