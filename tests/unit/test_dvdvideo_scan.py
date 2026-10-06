"""libavformat/dvdvideo_scan.c: the navigation scan of the DVD-Video
demuxer. Its random numbers are glibc's rand() sequence, produced on its own
(one sequence per scan, untouched by anything else calling rand()). With
DVDVIDEO_SCAN_REFERENCE (recorded results of corpus discs) the scan of each
disc gives them again; with DVDVIDEO_SCAN_FOLDER (a DVD folder) a read past
the end of the title VOBs fails the whole scan."""

import os
import pathlib

import pytest

from helpers import ffi, lib
from helpers.disc import env_or_skip
from helpers.dvd import scan


def sequence(seed, n):
    r = ffi.new("DVDVideoRand *")
    lib.ff_dvdvideo_rand_init(r, seed)
    return [lib.ff_dvdvideo_rand_next(r) for _ in range(n)]


def test_the_random_numbers_are_glibcs_rand_sequence():
    # the first values of rand() in a fresh glibc process (seed 1)
    assert sequence(1, 6) == [1804289383, 846930886, 1681692777, 1714636915, 1957747793, 424238335]
    # and the C library's own rand() after srand(seed), far into the sequence
    for seed in [1, 2, 12345, 0x7fffffff, 0xffffffff]:
        ours = sequence(seed, 100000)
        lib.srand(seed)
        libc = [lib.rand() for _ in range(100000)]
        assert ours == libc, f"seed {seed}: the sequences differ"


def test_seed_0_is_seed_1():
    assert sequence(0, 1000) == sequence(1, 1000)


# ---- the scan on real discs ----

def scan_lines(path):
    """The scan's results as `debug dvd-scan` prints them."""
    s = scan(path)
    assert not isinstance(s, int), "the scan runs"
    out = []
    if s.failure is not None:
        out.append(f"scan failed: {s.failure}")
    for r in s.results:
        out.append(f"result {r.name} title {r.title} pgc {r.pgcn} cells {','.join(map(str, r.cells))}")
    out += [f"entered {t}" for t in s.entered if t != 0]
    return out


@pytest.mark.disc
def test_corpus_scans_give_the_recorded_results():
    """With DVDVIDEO_SCAN_REFERENCE=<file>: every disc of the file scans to
    the lines recorded for it. The file holds `disc <path>` lines, each
    followed by its result lines."""
    path = env_or_skip("DVDVIDEO_SCAN_REFERENCE")
    discs = []
    for line in open(path).read().splitlines():
        if line.startswith("disc "):
            discs.append((line[5:], []))
        elif discs:
            discs[-1][1].append(line)
    assert discs, "no disc in the reference file"
    bad = [p for p, want in discs if scan_lines(p) != want]
    assert not bad, f"{len(bad)} of {len(discs)} discs scan differently: {bad}"


@pytest.mark.disc
def test_a_refused_read_fails_the_scan(tmp_path):
    """With DVDVIDEO_SCAN_FOLDER=<a DVD folder>: a copy whose title VOBs end
    after a few blocks makes the navigation read past their end; that read
    is refused and the whole scan fails, with no results."""
    src = pathlib.Path(env_or_skip("DVDVIDEO_SCAN_FOLDER"))
    video_ts = src / "VIDEO_TS" if (src / "VIDEO_TS").is_dir() else src
    dst = tmp_path / "VIDEO_TS"
    dst.mkdir()
    for e in video_ts.iterdir():
        name = e.name.upper()
        to = dst / name
        # VTS_nn_k.VOB with k > 0: a title VOB
        title_vob = len(name) == 12 and name.startswith("VTS_") and name[6:8] != "_0" and name[8:] == ".VOB"
        if not title_vob:
            os.symlink(e, to)
        elif name[6:8] == "_1":
            # the first title VOB: its first 16 blocks (at most) only; the
            # other title VOBs are left out, so the title VOBs end there
            with open(e, "rb") as f:
                to.write_bytes(f.read(16 * 2048))
    s = scan(tmp_path)
    assert not isinstance(s, int), "the scan runs"
    assert s.failure is not None, "the scan failed"
    assert "past the end of the VOB" in s.failure, s.failure
    assert s.results == []
