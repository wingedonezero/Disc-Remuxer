"""Blu-ray titles (src/disc/bluray/bluray_titles.c) on disc folders built here
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
    files = bd.bdmv(clips, {1: pl})
    files["BDMV/STREAM/00001.m2ts"] = files["BDMV/STREAM/00002.m2ts"] = bytes(1024 * 192)  # read by the probes
    ts = titles(build(tmp_path, files))
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


# ---- parts: whole stream files played back to back (option part) ----

def parts(line):
    """The dump's parts: [(first segment, duration, [marks])], [] when the title is one piece."""
    if " parts " not in line:
        return []
    out = []
    for p in field(line, "parts").split(","):
        first, dur, marks = p.split(":")
        out.append((int(first), int(dur), [int(m) for m in marks.split("+")] if marks else []))
    return out


def whole(clip, cc=1):
    """An item that covers its whole STC sequence: connection 1 joins it to the item before."""
    return Item(clip, S, E, connection=cc)


M = 24000                                   # 45 kHz ticks -> chapter mark units


def test_a_title_of_whole_files_back_to_back_has_a_part_per_file(tmp_path):
    pl = bd.mpls([whole("00001"), whole("00002"), whole("00003")],
                 marks=[(1, 0, S), (1, 0, S + 20000), (1, 1, S), (1, 1, S + 30000), (1, 2, S)])
    ts = titles(disc(tmp_path, [1, 2, 3], {1: pl}))
    assert names(ts) == ["00001.mpls"], "one title: the items join"
    assert parts(ts[0]) == [(0, 90000, [0, 20000 * M]), (1, 90000, [0, 30000 * M]), (2, 90000, [0])], \
        "each part from its own start, with the marks of its own item"


def test_a_part_drops_the_marks_in_its_own_last_second(tmp_path):
    pl = bd.mpls([whole("00001"), whole("00002")], marks=[(1, 0, S), (1, 0, S + 60000), (1, 1, S)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    assert field(ts[0], "marks") == "0,%d,%d" % (60000 * M, 90000 * M), "the title keeps it (not its last second)"
    assert parts(ts[0])[0][2] == [0], "as a title of its own the first file would not have it"


def test_a_mark_at_the_end_of_a_file_is_the_next_part_s_start_once(tmp_path):
    pl = bd.mpls([whole("00001"), whole("00002")], marks=[(1, 0, S), (1, 0, E), (1, 1, S), (1, 1, S + 10000)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    assert parts(ts[0])[1][2] == [0, 10000 * M]


def test_seamless_items_stay_in_their_part(tmp_path):
    pl = bd.mpls([whole("00001"), whole("00002"), whole("00003", 6), whole("00004", 5)])
    ts = titles(disc(tmp_path, [1, 2, 3, 4], {1: pl}))
    assert field(ts[0], "segments") == "1,(2,3),4"
    assert [(f, d) for f, d, _ in parts(ts[0])] == [(0, 90000), (1, 3 * 90000)], \
        "connection 6 and 5 are no back-to-back join: only the joined connection 1 starts a part"


def test_a_title_without_back_to_back_files_has_no_parts(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)], marks=[(1, 0, S + 45000)])
    ts = titles(disc(tmp_path, [1, 2], {1: pl}))
    assert parts(ts[0]) == []


# ---- the title list's decisions in the standard log (info level) ----

def said(tmp_path, clips, playlists):
    from helpers import LOG, install_log
    install_log(lib.AV_LOG_INFO)
    dump(disc(tmp_path, clips, playlists), min_length=0)
    return "\n".join(LOG)


def test_title_list_decisions_are_said(tmp_path):
    log = said(tmp_path, [1], {1: bd.mpls([part_item("00001")])})
    assert ("Blu-ray: 1 playlist(s) under 1 min are each one piece of a clip with at most one chapter: no playlist "
            "titles; their clips are listed as titles of their own instead: 00001") in log, log


def test_joins_merges_trims_and_runs_are_said(tmp_path):
    log = said(tmp_path, [1, 2], {1: bd.mpls([Item("00001", S, E), Item("00002", S, E, connection=1)])})
    assert ("Blu-ray: playlist 00001: items 0-1 are whole clips with matching streams: kept as one title across "
            "their non-seamless breaks") in log, log
    log = said(tmp_path / "m", [1], {1: bd.mpls([Item("00001", S, S + 40000), Item("00001", S + 40100, E,
                                                                                    connection=1)])})
    assert ("Blu-ray: playlist 00001: items 0 and 1 play one stretch of clip 00001 (the second goes on where the "
            "first ends): merged into one item") in log, log
    items = [part_item("00001")] + [part_item(c, 5) for c in ("00002", "00001", "00002", "00001", "00002")]
    log = said(tmp_path / "t", [1, 2], {1: bd.mpls(items)})
    assert "Blu-ray: playlist 00001 part 0: its last 4 item(s) (2-5) repeat the ones before them: left out" in log, log
    items = [part_item("00002")] + [part_item("00001", 5)] * 8 + [part_item("00003", 5)]
    log = said(tmp_path / "r", [1, 2, 3], {1: bd.mpls(items)})
    assert ("Blu-ray: playlist 00001 part 0: items 1-8 play the same piece of clip 00001 8 times: only the first "
            "and the last two are read") in log, log


def test_items_that_play_part_of_their_stc_sequence_are_said(tmp_path):
    pl = bd.mpls([part_item("00001"), part_item("00002", 6)])
    log = said(tmp_path, [1, 2], {1: pl})
    assert ("Blu-ray: title 00001.mpls: 2 of its items play only part of their clip's STC sequence (2.000 s before "
            "their IN times, 0.000 s after their OUT times): the title holds that too (whole STC sequences are read, "
            "the reference program's way; LOOK INTO AGAIN LATER)") in log, log


def test_a_part_starting_with_another_connection_condition_is_said(tmp_path):
    pl = bd.mpls([part_item("00001", 5), part_item("00002", 6)])
    log = said(tmp_path, [1, 2], {1: pl})
    assert ("Blu-ray: playlist 00001 part 0: item 0, its first, has connection condition 5: taken as 1 (a part "
            "starts on its own)") in log, log


def test_clip_information_corrections_are_said(tmp_path):
    clips = {1: bd.clpi(stcs=((32, 0x1001, S, E),))}
    pl = bd.mpls([part_item("00001"), part_item("00001", 5)], marks=[(1, 0, S + 1000), (1, 0, S + 2000)])
    from helpers import LOG, install_log
    install_log(lib.AV_LOG_INFO)
    dump(build(tmp_path, bd.bdmv(clips, {1: pl})), min_length=0)
    log = "\n".join(LOG)
    assert ("00001.clpi: its first STC sequence starts at source packet 32: taken as 0 (the clip's start)") in log, log
