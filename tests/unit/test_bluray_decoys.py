"""Blu-ray fake playlist check (libavformat/bluray_decoys.c): discs built
byte by byte (tests/synth/bluray.py), read with ff_bluray_disc_open, the
title list and the check, through the structure dump and the log: copies of
one clip played more than once (from different stream files) mark a long
playlist likely fake; with fakes, the long playlists without any repeat that
are at least as long as the shortest fake are likely real; a playlist that
plays the same stream file again (a menu loop) is neither; without fakes,
ten or more different long clip orders are said as possibly fakes of another
kind; only on discs with BD-J titles."""

import itertools

from helpers import ffi, install_log, lib, logged
from synth import bluray as bd
from synth.bluray import Item

MIN = 45000 * 60          # a minute at 45 kHz


def dump(path):
    err = ffi.new("int *")
    p = lib.tb_bluray_dump(str(path).encode(), ffi.NULL, 0, err)
    assert p != ffi.NULL, err[0]
    try:
        return ffi.string(p).decode()
    finally:
        lib.free(p)


def checks(text):
    """The check's dump lines, the playlists in number order (the dump has the disc's listing order)."""
    return sorted(l for l in text.splitlines() if l.startswith(("fakes ", "check ")))


def stream(n, size=192 * 8):
    """A stream file of its own content."""
    return bytes([n]) * size


def disc(tmp_path, playlists, clips=(1, 2, 3, 4, 5), copies=((2, 3),), bdj=True, sizes=None):
    """clips with stream files of their own content; each set in copies
    shares the content of its first clip."""
    sizes = sizes or {}
    extra = {"BDMV/index.bdmv": bd.index(((2, 1),) if bdj else ((1, 0),))}
    for n in clips:
        src = next((c[0] for c in copies if n in c), n)
        extra[f"BDMV/STREAM/{n:05d}.m2ts"] = stream(src, sizes.get(n, 192 * 8))
    files = bd.bdmv({n: bd.clpi() for n in clips}, {n: bd.mpls(items) for n, items in playlists.items()}, extra)
    return bd.write(tmp_path / "disc", files)


def items(*spec):
    """(clip, minutes) pairs."""
    return [Item(f"{c:05d}", 0, m * MIN) for c, m in spec]


FAKES = {
    800: items((1, 30), (2, 1), (4, 30)),            # real: 61 min, no repeat
    801: items((1, 30), (2, 1), (3, 1), (4, 30)),    # fake: clips 2 and 3 are copies of one clip
    802: items((1, 30), (3, 1), (2, 1), (4, 30)),    # fake
    803: items(*[(5, 1)] * 30),                     # a loop: the same file 30 times (not a fake)
    804: items((1, 30)),                             # long, no repeat, but shorter than the fakes
    805: items((1, 30), (2, 1), (4, 30)),            # the real one again
    806: items((2, 1), (3, 1)),                      # copies, but not long
}


def test_copies_played_twice_make_a_fake_and_the_long_clean_ones_are_likely_real(tmp_path):
    install_log(lib.AV_LOG_INFO)
    t = dump(disc(tmp_path, FAKES))
    assert checks(t) == ["check 00800 real repeats 0", "check 00801 fake repeats 1",
                         "check 00802 fake repeats 1", "check 00805 real repeats 0",
                         "fakes 2 real 2 suspected 0"]
    assert logged("Blu-ray: clips 00002, 00003 have byte-identical stream files (1536 bytes each)")
    assert logged("Blu-ray: fake playlists: 2 long playlist(s) play copies of one clip more than once, a way to "
                  "hide the main feature among fakes; likely real (no clip played twice, at least as long as the "
                  "shortest fake without its repeated copies, 1:01:00): 00800, 00805")
    assert logged("Blu-ray: playlist 00801 (1:02:00): likely fake: plays 2 times from 2 copies of one clip "
                  "(00002, 00003)")
    assert logged("Blu-ray: playlist 00800 (1:01:00): likely real")


def test_without_bdj_titles_there_is_no_check(tmp_path):
    install_log(lib.AV_LOG_INFO)
    t = dump(disc(tmp_path, FAKES, bdj=False))
    assert checks(t) == []
    assert not logged("fake")


def test_files_of_equal_size_but_other_bytes_are_not_copies(tmp_path):
    t = dump(disc(tmp_path, FAKES, copies=()))
    assert checks(t) == ["fakes 0 real 0 suspected 0"]


def test_files_of_other_sizes_are_not_compared(tmp_path):
    install_log(lib.AV_LOG_VERBOSE)
    sizes = {1: 192 * 10, 2: 192 * 8, 3: 192 * 9, 4: 192 * 11, 5: 192 * 12}
    t = dump(disc(tmp_path, FAKES, copies=((2, 3),), sizes=sizes))
    assert checks(t) == ["fakes 0 real 0 suspected 0"]
    assert logged("fake playlist check: 0 pair(s) of stream files of equal size compared")


def test_many_long_clip_orders_without_fakes_are_said_as_possibly_fakes(tmp_path):
    install_log(lib.AV_LOG_INFO)
    orders = list(itertools.permutations([1, 2, 4, 5]))[:10]
    pls = {900 + i: items(*[(c, 20) for c in o]) for i, o in enumerate(orders)}
    t = dump(disc(tmp_path, pls, copies=()))
    assert checks(t) == ["fakes 0 real 0 suspected 1"]
    assert logged("Blu-ray: 10 different clip orders among the playlists of an hour or more, but none plays a copy "
                  "of a clip twice: possibly fake playlists of a kind not recognised (LOOK INTO AGAIN LATER)")
    # nine orders (and a copy of one of them) are not enough
    pls = {900 + i: items(*[(c, 20) for c in o]) for i, o in enumerate(orders[:9])}
    pls[950] = pls[900]
    assert checks(dump(disc(tmp_path / "b", pls, copies=()))) == ["fakes 0 real 0 suspected 0"]
