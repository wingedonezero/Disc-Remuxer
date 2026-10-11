"""libavformat/dvd_titles.c: the title list (step D2, part 3a). Built disc
folders (one title set; helpers.dvdnav.build adds a last one-cell title the
first-play chain jumps to): the navigation scan, or given scan results, then
the title list: each title's cells, length, segments (extents of the title
VOBs), chapters and angles, and the titles left out. With
DVD_TITLES_REFERENCE (a folder of <disc>.titles files: the title list as
ff_dvd_titles_dump() gives it) and DVD_SCAN_DISCS (a list of
"<disc>\\t<path>" lines) every listed disc's title list must be its
expected one line for line."""

import dataclasses
import json
import pathlib
import subprocess

from helpers import LOG, PROGRAM, install_log, lib
from helpers.disc import env_or_skip
from helpers.dvdnav import Pgc, build, cell, titles, titles_from, titles_text, vobus
from synth import dvd
from synth.dvd import flags
from test_dvd_disc import Disc
from test_dvd_nav import open_disc

S = 2048


def c(first, last, step, secs, **kw):
    """A cell of sectors first..last with a VOBU every step sectors."""
    return cell(first, last, secs, last_vobu=first + step * ((last - first) // step), **kw)


def run(folder, results=None, **kw):
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(folder)
    if results is None:
        ret, _, ts = titles(d.d, **kw)
    else:
        ret, ts = titles_from(d.d, results, **kw)
    d.close()
    assert ret == 0
    return ts


def logged(part):
    return any(part in line for line in LOG)


def test_a_title_is_its_cells_extents_and_chapters(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1)], programs=[1, 2])
    folder = build(tmp_path, [p], [[(1, 1), (1, 2)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    ts = run(folder)
    t = ts[0]
    assert (t["name"], t["cells"], t["chapters"], t["segments"], t["map"]) == ("1", "2", "2", "1", "1-2")
    assert int(t["size"]) == 20 * S
    assert (t["length"], t["real"]) == ("2", "2")   # declared: the cells' times; real: their VOBUs' 0.5 s each
    assert t["scan"] != "-"
    assert logged("title 1 added (2 cell(s), 0:00:02)")


def test_a_cell_starting_a_new_timeline_starts_a_new_segment(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1, flags=flags.STC_DISCONTINUITY)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    t = run(folder)[0]
    assert (t["segments"], t["map"]) == ("2", "1,2")
    assert int(t["size"]) == 20 * S


def six_cells(tmp_path, secs):
    cells = [c(10 * k, 10 * k + 9, 5, s) for k, s in enumerate(secs)]
    navs = [n for k in range(len(secs)) for n in vobus(10 * k, 10 * k + 9, 5)]
    return build(tmp_path, [Pgc(cells=cells)], [[(1, 1)]], navs, 10 * len(secs))


def test_cellwalk_drops_a_short_run_at_the_start(tmp_path):
    folder = six_cells(tmp_path, [2, 10, 10, 10, 10, 10])
    t = run(folder, [("A1", 1, 1, [1, 3, 4, 5, 6])])[0]
    assert (t["cells"], t["map"]) == ("4", "3-6")
    assert logged("title 1: cells 1-2 left out at the start")


def test_cellwalk_keeps_the_longer_end_run(tmp_path):
    folder = six_cells(tmp_path, [10, 10, 10, 10, 10, 10])
    t = run(folder, [("A1", 1, 1, [1, 2, 4, 5, 6])])[0]
    assert t["map"] == "4-6"
    assert logged("title 1: cells 1-3 left out at the start")


def test_cellwalk_keeps_the_first_run_when_both_ends_are_as_long(tmp_path):
    folder = six_cells(tmp_path, [10, 10, 10, 10, 10, 10])
    t = run(folder, [("A1", 1, 1, [1, 2, 5, 6])])[0]
    assert t["map"] == "1-2"
    assert logged("title 1: cells 3-6 left out at the end")


def test_a_title_whose_real_length_is_far_from_its_declared_one_is_left_out(tmp_path):
    # declared 1000 s, its VOBUs play 1 s: a fake
    p = Pgc(cells=[c(0, 9, 5, 1000)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10)
    ts = run(folder, [("A1", 1, 1, [1])])
    assert not [t for t in ts if t["name"] == "1"]
    assert logged("title 1 left out: its declared length is 0:16:40, its real length 0:00:01 (a fake title)")


def test_a_title_the_navigation_did_not_reach_is_left_out(tmp_path):
    pgcs = [Pgc(cells=[c(0, 9, 5, 1)]), Pgc(cells=[c(10, 19, 5, 1)])]
    folder = build(tmp_path, pgcs, [[(1, 1)], [(2, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    ts = run(folder, [("A1", 1, 1, [1])])
    assert [t["name"] for t in ts] == ["1"]
    assert logged("title 2 left out: not reached by the navigation")


def test_an_angle_block_gives_a_title_per_angle(tmp_path):
    first = flags.ANGLE_BLOCK | flags.FIRST_IN_BLOCK
    last = flags.ANGLE_BLOCK | flags.LAST_IN_BLOCK
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1, flags=first), c(20, 29, 5, 1, flags=last)])
    navs = vobus(0, 9, 5) + vobus(10, 19, 5) + vobus(20, 29, 5)
    folder = build(tmp_path, [p], [[(1, 1)]], navs, 30, angles=[2])
    ts = [t for t in run(folder, [("A1", 1, 1, [1, 2])]) if t["name"] == "1"]
    assert [(t["angle"], t["map"], int(t["size"]) // S) for t in ts] == [("1/2", "1-2", 20), ("2/2", "(1,3)", 20)]
    assert logged("title 1: angle 2 added")


def test_a_program_chain_failing_the_check_without_the_navigation_s_cells_is_left_out(tmp_path):
    # title 1 plays chain 1, then chain 2: four cells without playback time
    # (reason 3); the navigation recorded chain 1 only
    dead = [dataclasses.replace(c(10 + 5 * k, 14 + 5 * k, 5, 0), playback_time=bytes(4)) for k in range(4)]
    pgcs = [Pgc(cells=[c(0, 9, 5, 1)]), Pgc(cells=dead)]
    navs = vobus(0, 9, 5) + [n for k in range(4) for n in vobus(10 + 5 * k, 14 + 5 * k, 5)]
    folder = build(tmp_path, pgcs, [[(1, 1), (2, 1)]], navs, 30)
    ts = run(folder, [("A1", 1, 1, [1])])
    assert [t["name"] for t in ts] == ["1"]
    assert logged("title 1: program chain 2 left out: it fails the program chain check (reason 3)")


def test_a_chapter_before_the_previous_one_is_dropped(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1)], programs=[1, 2])
    folder = build(tmp_path, [p], [[(1, 2), (1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    t = run(folder, [("A1", 1, 1, [1, 2])])[0]
    assert t["chapters"] == "1"


def test_a_title_shorter_than_the_minimum_length_is_listed_not_selected(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    assert run(folder, [("A1", 1, 1, [1, 2])], min_length=5)[0]["short"]
    assert not run(folder, [("A1", 1, 1, [1, 2])], min_length=2)[0]["short"]


def test_a_cell_s_last_vobu_ending_early_is_told_and_the_walk_goes_on(tmp_path):
    # the cell runs to sector 19; its last VOBU (15) ends the cell after one
    # sector: invalid navigation, the VOBU at 16 follows
    p = Pgc(cells=[c(0, 19, 5, 2)])
    navs = vobus(0, 15, 5) + [dvd.Nav(lbn=16, vobu_ea=3, next_vobu=dvd.END_OF_CELL, vobu_s_ptm=180000,
                                      vobu_e_ptm=225000)]
    folder = build(tmp_path, [p], [[(1, 1)]], navs, 20)
    t = run(folder, [("A1", 1, 1, [1])])[0]
    assert int(t["size"]) == 20 * S
    assert logged("title 1: the navigation information is invalid around")


# ---- tracks and duplicates ----

def ac3(lang=b"en", ext=0):
    """vts_audio_attr: AC-3, language present (lang_type 1), code_extension."""
    return bytes([0x04, 0x00]) + lang + bytes([0, ext, 0, 0])


def subp(lang=b"en", ext=0):
    """vts_subp_attr: language present (type 1), code_extension."""
    return bytes([0x01, 0]) + lang + bytes([0, ext])


def tracks(t):
    return t["tracks"].split(",")


def test_audio_tracks_follow_audio_control_then_the_title_set_s_others(tmp_path):
    # audio_control lists stream 1, then stream 0; the title set declares 3
    p = Pgc(cells=[c(0, 9, 5, 1)], audio_control=bytes([0x81, 0, 0x80, 0]) + bytes(12))
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10, audio_attrs=[ac3(), ac3(b"fr"), ac3(b"de")])
    t = run(folder, [("A1", 1, 1, [1])])[0]
    assert tracks(t) == ["V", "A1.0", "A0.1", "A2.2"]   # stream.attribute


def test_subtitles_use_the_wide_stream_on_16_9_and_the_4_3_one_on_4_3(tmp_path):
    ctl = bytes([0x80 | 2, 5, 7, 9]) + bytes([0x80 | 1, 3, 4, 6]) + bytes(120)
    p = Pgc(cells=[c(0, 9, 5, 1)], subp_control=ctl)
    for aspect, want in [(0x00, ["S2.0", "S1.1"]), (0x0c, ["S5.0", "S3.1"])]:
        folder = build(tmp_path / str(aspect), [p], [[(1, 1)]], vobus(0, 9, 5), 10, video_attr=bytes([0x40 | aspect, 0]),
                       subp_attrs=[subp(), subp(b"fr")])
        assert tracks(run(folder, [("A1", 1, 1, [1])])[0])[1:] == want


def test_one_wide_subtitle_stream_with_a_letterbox_stream_0_is_listed_twice(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1)], subp_control=bytes([0x80, 1, 0, 0]) + bytes(124))
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10, video_attr=bytes([0x4c, 0]), subp_attrs=[subp()])
    assert tracks(run(folder, [("A1", 1, 1, [1])])[0])[1:] == ["S1.0", "S0.0"]


def test_a_title_playing_an_earlier_one_s_sectors_is_left_out(tmp_path):
    # titles 1 and 2 play the same chain's cells; title 2 has no more chapters
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1)], programs=[1, 2])
    folder = build(tmp_path, [p], [[(1, 1), (1, 2)], [(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    ts = run(folder, [("A1", 1, 1, [1, 2]), ("A2", 2, 1, [1, 2])])
    assert [t["name"] for t in ts] == ["1"]
    assert logged("title 2 left out: it is the same as title 1 (VTS_01)")


def test_of_two_titles_playing_the_same_sectors_the_one_with_fewer_chapters_goes(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1)], programs=[1, 2])
    folder = build(tmp_path, [p], [[(1, 1)], [(1, 1), (1, 2)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    ts = run(folder, [("A1", 1, 1, [1, 2]), ("A2", 2, 1, [1, 2])])
    # title 1 (1 chapter) is the same as title 2 (2 chapters): title 1 goes
    assert [t["name"] for t in ts] == ["2"]


def test_info_lists_a_dvd_title_s_tracks(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1)], audio_control=bytes([0x80, 0]) + bytes(14),
            subp_control=bytes([0x80, 0, 0, 0]) + bytes(124))
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10, video_attr=bytes([0x40, 0]),
                   audio_attrs=[ac3(b"zh", 3)], subp_attrs=[subp(b"en", 9)])
    from test_dvd_streams import audio_pack, put, video
    put(folder, 1, audio_pack(0))       # the audio stream has data (the stream pass keeps it)
    put(folder, 2, video())             # and the video (a title without video data is left out)
    ts, _ = info(folder)
    got = [(k["kind"], k["codec"], k["language"], k["width"], k["height"], k["roles"]) for k in ts[0]["tracks"]]
    assert got == [("video", "Mpeg2", "eng", None, None, []), ("audio", "DD", "chi", None, None, ["commentary"]),
                   ("subtitle", "VobSub", "eng", 720, 480, ["forced"])]


# ---- the cell list without the navigation (trim) ----

TRIM, WALK_TRIM = 2, 3


def filler(first, last, **kw):
    """A cell without playback time: always rejected."""
    return dataclasses.replace(c(first, last, 5, 0, **kw), playback_time=bytes(4))


def test_trim_keeps_the_cells_between_filler_cells_of_other_vobs(tmp_path):
    cells = [filler(0, 9, vob_id_nr=1, cell_nr=1), c(10, 19, 5, 10, vob_id_nr=2, cell_nr=1),
             filler(20, 29, vob_id_nr=3, cell_nr=1)]
    folder = build(tmp_path, [Pgc(cells=cells)], [[(1, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5) + vobus(20, 29, 5), 30)
    t = run(folder, [], cell_mode=TRIM)[0]
    assert (t["map"], t["scan"]) == ("2", "-")
    assert logged("title 1: cells 1-1 left out at the start") and logged("title 1: cells 3-3 left out at the end")


def test_trim_drops_rejected_cells_at_both_ends(tmp_path):
    # one VOB: not the first way; the cells that pass the checks from both ends
    # in (60 sectors a cell: 50 sectors a second or more, else the chain is a decoy)
    cells = [dataclasses.replace(c(0, 59, 30, 0), playback_time=bytes(4)), c(60, 119, 30, 1), c(120, 179, 30, 1),
             dataclasses.replace(c(180, 239, 30, 0), playback_time=bytes(4))]
    navs = [n for k in range(4) for n in vobus(60 * k, 60 * k + 59, 30)]
    folder = build(tmp_path, [Pgc(cells=cells)], [[(1, 1)]], navs, 240)
    t = run(folder, [], cell_mode=TRIM)[0]
    assert t["map"] == "2-3"


def test_walk_trim_lists_a_title_the_navigation_did_not_reach(tmp_path):
    pgcs = [Pgc(cells=[c(0, 9, 5, 1)]), Pgc(cells=[c(10, 19, 5, 1)])]
    folder = build(tmp_path, pgcs, [[(1, 1)], [(2, 1)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    walk = run(folder, [("A1", 1, 1, [1])])
    both = run(folder, [("A1", 1, 1, [1])], cell_mode=WALK_TRIM)
    assert [t["name"] for t in walk] == ["1"]
    assert [(t["name"], t["scan"]) for t in both][:2] == [("1", "A1"), ("2", "-")]


# ---- the cell check (DVD_CELL_CHECK_*: every VOBU, 50 evenly spread, 50 at random) ----

ALL, SPREAD, RANDOM = 0, 1, 2


def one_long_cell(tmp_path, broken):
    """A cell of 70 one-sector VOBUs (sectors 0..69); the VOBUs at broken
    hold no NAV pack (the VOBU address map still lists them)."""
    p = Pgc(cells=[c(0, 69, 1, 35)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 69, 1), 70)
    vob = folder / "VIDEO_TS" / "VTS_01_1.VOB"
    data = bytearray(vob.read_bytes())
    for b in broken:
        data[b * S:(b + 1) * S] = bytes(S)
    vob.write_bytes(data)
    return folder


def check(folder, mode):
    d = Disc.folder(folder)
    r = lib.ff_dvd_cell_check(d.d, 1, 1, 1, mode)
    d.close()
    return r


def test_the_cell_check_reads_every_vobu_or_50_evenly_spread(tmp_path):
    # the VOBUs before the last one: 69; 50 evenly spread leave some out
    left_out = sorted(set(range(69)) - {k * 69 // 50 for k in range(50)})
    folder = one_long_cell(tmp_path, [left_out[0]])
    assert check(folder, ALL) == 1
    assert check(folder, SPREAD) == 0


def test_a_broken_first_vobu_fails_the_cell_check_in_every_mode(tmp_path):
    folder = one_long_cell(tmp_path, [0])
    assert [check(folder, m) for m in (ALL, SPREAD, RANDOM)] == [1, 1, 1]


def test_the_cell_check_passes_a_whole_cell(tmp_path):
    folder = one_long_cell(tmp_path, [])
    assert [check(folder, m) for m in (ALL, SPREAD, RANDOM)] == [0, 0, 0]


def test_libdvdread_runs_the_cell_check_and_says_which_vobus_it_read(tmp_path):
    folder = one_long_cell(tmp_path, [])
    for mode, what in ((ALL, "all read"), (SPREAD, "50 evenly spread"), (RANDOM, "50 at random")):
        install_log(lib.AV_LOG_DEBUG)
        assert check(folder, mode) == 0
        assert logged(f"libdvdread: cell check, VTS_01 program chain 1 cell 1 (69 VOBUs, {what}): passed"), what


# ---- the program ----

def info(path, *sets, detail=False):
    """detail: the log's detail lines too (-v)."""
    args = [*PROGRAM, "--json"] + (["-v"] if detail else []) + [a for x in sets for a in ("--set", x)] + \
        ["info", str(path)]
    out = subprocess.run(args, capture_output=True, text=True)
    lines = [json.loads(line) for line in out.stdout.splitlines() if line.startswith("{")]
    return [x for x in lines if x.get("type") == "title"], lines


def test_info_lists_a_dvd_s_titles(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1), c(10, 19, 5, 1, flags=flags.STC_DISCONTINUITY)], programs=[1, 2])
    folder = build(tmp_path, [p], [[(1, 1), (1, 2)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)
    from test_dvd_streams import put, video
    put(folder, 1, video())
    ts, _ = info(folder)
    assert [(t["duration"], t["size"], t["chapters"], t["files"], t["selected"]) for t in ts] == \
        [(2.0, 20 * S, 2, ["1", "2"], True)]
    assert ts[0]["source"].startswith("VTS_01 program chain 1 (navigation ")


def test_the_cell_mode_setting_reaches_the_title_list(tmp_path):
    p = Pgc(cells=[c(0, 9, 5, 1)])
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10)
    from test_dvd_streams import put, video
    put(folder, 1, video())
    auto, _ = info(folder)
    trim, _ = info(folder, "dvd.cell_mode=trim", "dvd.cell_check=spread")
    assert "navigation" in auto[0]["source"]
    assert trim[0]["source"] == "VTS_01 program chain 1, cells"  # trimmed: the navigation is not played


def test_corpus_title_lists_are_the_expected_ones():
    ref, listing = env_or_skip("DVD_TITLES_REFERENCE", "DVD_SCAN_DISCS")
    differ, done = [], 0
    for line in pathlib.Path(listing).read_text().splitlines():
        disc, path = line.split("\t")[:2]
        expected = pathlib.Path(ref) / f"{disc}.titles"
        if not expected.exists():
            continue
        d = open_disc(pathlib.Path(path))
        ret, text = titles_text(d.d)
        d.close()
        done += 1
        if ret != 0 or text != expected.read_text():
            differ.append(disc)
    assert done and not differ
