"""Blu-ray titles (libavformat/bluray_titles.c) on disc folders built here
(tests/synth/bluray.py): parts, seamless segments, merged and joined play
items, trimmed repeats and runs, clip titles, duplicates, the minimum
length, angles, refused titles, BDAV titles and stereoscopic (3-D)
segments. The real-disc check is by hand: the titles of 7 discs equal the
reference program's info output (implementation notes)."""

from helpers import ffi, lib
from synth import bluray as bd
from synth.bluray import Item, SubPath, be16, be32


def build(tmp_path, files):
    return bd.write(tmp_path / "disc", files)


def dump(path, min_length=-1):
    err = ffi.new("int *")
    p = lib.tb_bluray_dump(str(path).encode(), ffi.NULL, min_length, err)
    if p == ffi.NULL:
        return err[0]
    try:
        return ffi.string(p).decode()
    finally:
        lib.free(p)

S, E = 27000000, 27000000 + 90000          # the built clips' STC sequence (2 s)


def titles(path, min_length=0):
    t = dump(path, min_length=min_length)
    assert not isinstance(t, int), t
    return [l for l in t.splitlines() if l.startswith("title ")]


def names(ts):
    return [l.split()[2] for l in ts]


def field(line, key):
    w = line.split()
    return w[w.index(key) + 1]


def part_item(clip, cc=1):
    """An item that does not cover its whole STC sequence (no join by fit)."""
    return Item(clip, S + 45000, E, connection=cc)


def disc(tmp_path, clips, playlists):
    return build(tmp_path, bd.bdmv({n: bd.clpi() for n in clips}, playlists))


def test_a_playlist_is_split_into_parts_at_non_seamless_connections(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002"), part_item("00003", 5)],
                 marks=[(1, 0, S + 1000), (1, 0, S + 2000), (1, 1, S + 45000)])
    ts = titles(disc(tmp_path, [1, 2, 3], {1: pl}))
    assert names(ts) == ["00001.mpls", "00001.mpls(1)"]
    assert field(ts[0], "segments") == "1"
    assert field(ts[1], "segments") == "2,3", "connection 5 keeps the part, but not one segment"


def test_seamless_items_are_one_segment(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    assert names(ts) == ["00001.mpls"]
    assert field(ts[0], "segments") == "(1,2)"
    assert field(ts[0], "size") == str(2 * 192), "each STC sequence to the end of its (192-byte) stream file"
    assert field(ts[0], "duration") == str(2 * 90000), "whole STC sequences count"


def test_a_part_of_one_clip_without_chapters_leaves_a_clip_title(tmp_path):
    ts = titles(disc(tmp_path, [1], {1: bd.mpls([part_item("00001")])}))
    assert names(ts) == ["00001.m2ts"]


def test_items_that_cover_their_whole_stc_sequences_join(tmp_path):
    pl = bd.mpls([Item("00001", S, E), Item("00002", S, E, connection=1)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    assert names(ts) == ["00001.mpls"], "the second item's connection 1 joins (no second part)"
    assert field(ts[0], "segments") == "1,2"


def test_neighbouring_items_of_one_clip_are_merged(tmp_path):
    pl = bd.mpls([Item("00001", S, S + 40000), Item("00001", S + 40100, E, connection=1)])
    t = dump(disc(tmp_path, [1], {1: pl}), min_length=0)
    assert "playlist 00001 version 0200 playback 1 items 1" in t
    assert names([l for l in t.splitlines() if l.startswith("title ")]) == ["00001.m2ts"]


def test_an_equal_playlist_is_left_out(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl, 2: pl}))
    assert len(ts) == 1 and names(ts)[0] in ("00001.mpls", "00002.mpls")


def test_titles_shorter_than_the_minimum_length_are_left_out(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)])
    root = disc(tmp_path, [1, 2], {1: pl})
    assert names(titles(root, 4)) == ["00001.mpls"]
    assert names(titles(root, 5)) == []


def test_repeated_blocks_at_the_end_are_trimmed(tmp_path):
    items = [part_item("00001")] + [part_item(c, 5) for c in ("00002", "00001", "00002", "00001", "00002")]
    ts = titles(disc(tmp_path, [1, 2], {1: bd.mpls(items)}))
    assert field(ts[0], "segments") == "1,2"


def test_of_a_long_run_only_the_first_and_last_two_items_are_read(tmp_path):
    items = [part_item("00002")] + [part_item("00001", 5)] * 8 + [part_item("00003", 5)]
    ts = titles(disc(tmp_path, [1, 2, 3], {1: bd.mpls(items)}))
    assert field(ts[0], "segments") == "2,1,1,1,3"


def test_each_angle_is_a_title(tmp_path):
    item = Item("00001", S + 45000, E, angles=[("00002", 0)])
    pl = bd.mpls([item, part_item("00003", 6)])
    ts = titles(disc(tmp_path, [1, 2, 3], {1: pl}))
    assert [(field(l, "segments"), field(l, "angle")) for l in ts if "mpls" in l] == [("(1,3)", "1"), ("(2,3)", "2")]


def test_a_title_whose_clip_starts_with_a_dependent_view_is_refused(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)])
    root = build(tmp_path, bd.bdmv({1: bd.clpi(), 2: bd.clpi(streams=((0x1012, 0x20, b"\x61"),))}, {1: pl}))
    assert "00001.mpls" not in names(titles(root))


def test_a_playlist_that_is_not_played_in_order_gives_no_title(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)], playback_type=2)
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    assert sorted(names(ts)) == ["00001.m2ts", "00002.m2ts"]


def test_chapter_marks_and_the_last_second(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)],
                 marks=[(1, 0, S + 45000), (1, 1, S + 40000), (1, 1, E - 1000), (2, 0, S + 50000)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    # (time - STC start + earlier items) x 24000; marks within the last second are dropped; type 2 is no chapter
    assert field(ts[0], "marks") == "%d,%d" % (45000 * 24000, (90000 + 40000) * 24000)


def test_a_stereoscopic_title_reads_base_and_dependent_view_interleaved(tmp_path):
    ext = [(2, 4, be32(4 * 2 + 4) + be32(2) + be32(0) + be32(512))]     # extent start points 0, 512
    clips = {1: bd.clpi(packets=1024, ext=ext),
             2: bd.clpi(packets=1024, ext=ext, app_type=8, streams=((0x1012, 0x20, b"\x61"),))}
    sp = SubPath(type=8, clips=["00002"]).bytes()
    pl = bd.mpls([Item("00001", S, E)], marks=[(1, 0, S), (1, 0, S + 1000)], ext=[(2, 2, bytes(4) + be16(1) + sp)])
    ts = titles(build(tmp_path, bd.bdmv(clips, {1: pl})))
    t = [l for l in ts if "00001.mpls" in l][0]
    assert field(t, "segments") == "1/2*"
    assert field(t, "size") == str(2 * 1024 * 192)


# ---- BDAV ----

def bdav(tmp_path, items, ptype=1, name=b""):
    files = {"BDAV/info.bdav": bd.info_bdav(b"Disc")}
    for c in sorted({i[0] for i in items}):
        files[f"BDAV/CLIPINF/{c}.clpi"] = bd.clpi(kind="M2TS")
        files[f"BDAV/STREAM/{c}.m2ts"] = bytes(1024 * 192)
    pl = bytearray(bd.rpls([(c, 0, S, E) for c, _ in items], ptype, name))
    p = 0x158 + 10
    for k, (c, cc) in enumerate(items):      # connection conditions
        pl[p + 0xc] = cc
        p += 2 + int.from_bytes(pl[p:p + 2], "big")
    pl[0x2e] = 0x10                           # names as recorded
    pl[0x32:0x39] = bytes([0x20, 0x24, 0x05, 0x17, 0x21, 0x30, 0x00])
    return build(tmp_path, {**files, "BDAV/PLAYLIST/01001.rpls": bytes(pl)})


def test_a_recorded_playlist_is_one_title_with_its_name_and_time(tmp_path):
    ts = titles(bdav(tmp_path, [("00001", 1), ("00002", 1)], name=b"Evening News"))
    t = [l for l in ts if "rpls" in l][0]
    assert t.split()[2] == "01001.rpls"
    assert "name 'Evening News'" in t and "recorded 2024-05-17 21:30:00" in t


def test_seamless_runs_of_a_recorded_playlist_are_titles_of_their_own(tmp_path):
    ts = titles(bdav(tmp_path, [("00001", 1), ("00002", 6), ("00003", 1), ("00004", 6)]))
    assert [l.split()[2] for l in ts if "rpls" in l] == ["01001.rpls(1)", "01001.rpls(2)"]
