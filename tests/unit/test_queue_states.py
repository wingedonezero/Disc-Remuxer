"""The queue's states while a disc is ripped (src/disc-remuxer/views.c), on
the command line: demux --details prints, at the moments the GUI's queue
changes them, the same words as "Queue:" lines — the disc's ("Ripping…",
"Title #0 (1 of 1)", "Done") and the title's ("Writing…", "Done, 2
warning(s)"), so a test here is a test of the GUI's Status column. A built
Blu-ray (test_stream_names.disc), its one title ripped."""

import subprocess

from helpers import PROGRAM
from test_stream_names import disc


def demux(tmp_path, *opts):
    p = subprocess.run([*PROGRAM, *opts, "demux", str(disc(tmp_path)), "0", str(tmp_path / "out")],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stderr
    return p.stdout.splitlines()


def test_demux_details_give_the_queue_s_states_in_order(tmp_path):
    out = [l for l in demux(tmp_path, "--details") if l.startswith("Queue:")]
    assert out == ["Queue: disc | Ripping…",
                   "Queue: disc | Title #0 (1 of 1)",
                   "Queue:   Title #0 | Writing…",
                   "Queue:   Title #0 | Done, 2 warning(s)",
                   "Queue: disc | Done"]


def test_demux_without_details_has_no_queue_lines(tmp_path):
    assert not [l for l in demux(tmp_path) if l.startswith("Queue:")]
