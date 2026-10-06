"""libavformat/hddvd_titles.c and disclang.c: the HD DVD title plan. With
HDDVD_CORPUS (a folder of HD DVD images) and HDDVD_TITLES_REFERENCE (one
<image name>.titles.txt per image, every character but A-Z a-z 0-9 _ of the
name replaced by _; one line per title: index|name|language|H:MM:SS|bytes|
first EVO file|EVOBs|EVOB names|chapters) the plan of every image must give
the reference's lines. Titles the reference program drops after its title
plan (when it builds their streams) are listed, by first EVO file, in
<image name>.titles.dropped.txt (lines starting with # are comments): they
are taken out of our list and the rest renumbered before the comparison."""

import pathlib
import re

import pytest

from helpers import ffi, lib
from helpers.disc import env_or_skip
from helpers.hddvd import plan, take_text
from synth.hddvd import clip, evob, image, playlist, tmap, vti

INVALIDDATA = -0x41444E49


def lang(code):
    r = lib.ff_disc_lang_code(code.encode())
    return None if r == ffi.NULL else ffi.string(r).decode()


def test_language_codes_follow_iso_639_with_three_extras():
    for code, want in [
        ("en", "eng"), ("eng", "eng"), ("EN", "eng"), ("Fre", "fre"),
        ("fra", "fre"), ("fr", "fre"), ("zho", "chi"), ("jp", "jpn"),
        ("iw", "heb"), ("ptb", "ptb"), ("PTB", "ptb"),
        ("in", None), ("ji", None), ("jw", None), ("mo", None), ("sh", None),
        ("", None), ("e", None), ("engl", None), ("xx", None), ("ené", None),
    ]:
        assert lang(code) == want, code


def plan_lines(img):
    """Our plan of an image as reference lines."""
    src = ffi.new("DiscIOSource **")
    fs = ffi.new("DiscIOFS **")
    vti_ = ffi.new("HDDVDVTI **")
    xs = ffi.new("HDDVDXpl ***")
    nb = ffi.new("int *")
    p = ffi.new("HDDVDTitlePlan **")
    assert lib.ff_discio_source_open_file(ffi.NULL, str(img).encode(), src) == 0
    assert lib.ff_discio_mount_image(src[0], ffi.NULL, fs) == 0
    assert lib.ff_hddvd_vti_open(ffi.NULL, fs[0], vti_) == 0
    assert lib.ff_hddvd_xpl_load(ffi.NULL, fs[0], xs, nb) == 0
    assert lib.ff_hddvd_titles_plan(ffi.NULL, fs[0], vti_[0], xs[0], nb[0], 0, p) == 0
    dump = take_text(lib.ff_hddvd_titles_dump(p[0]))
    lib.ff_hddvd_titles_free(p)
    lib.ff_hddvd_xpl_free_all(xs, nb[0])
    lib.ff_hddvd_vti_free(vti_)
    lib.ff_discio_fs_close(fs)
    lib.ff_discio_source_free(src)
    out = []
    for line in dump.splitlines():
        if not line.startswith("title "):
            continue
        # title i|name|lang|kind|selected|duration|secs|size|file|clips|map|chapters
        f = line.split("|")
        secs = int(f[6])
        lng = "" if f[2] == "(null)" else f[2]
        out.append(f"{f[0][6:]}|{f[1]}|{lng}|{secs // 3600}:{secs // 60 % 60:02}:{secs % 60:02}|"
                   f"{f[7]}|{f[8]}|{f[9]}|{f[10]}|{f[11]}")
    return out


@pytest.mark.disc
def test_corpus_every_title_list_equals_the_reference():
    folder, refdir = env_or_skip("HDDVD_CORPUS", "HDDVD_TITLES_REFERENCE")
    images = sorted(p for p in pathlib.Path(folder).iterdir() if p.suffix.lower() == ".iso")
    bad = 0
    for img in images:
        stem = re.sub(r"[^A-Za-z0-9_]", "_", img.stem)
        want = (pathlib.Path(refdir) / f"{stem}.titles.txt").read_text().splitlines()
        dropped_file = pathlib.Path(refdir) / f"{stem}.titles.dropped.txt"
        dropped = [ln for ln in (dropped_file.read_text().splitlines() if dropped_file.exists() else [])
                   if ln and not ln.startswith("#")]
        kept = [ln for ln in plan_lines(img) if ln.split("|")[5] not in dropped]
        got = [f"{i}|{ln.split('|', 1)[1]}" for i, ln in enumerate(kept)]
        if got == want:
            print(f"{stem}: {len(got)} titles equal ({len(dropped)} dropped later by the reference)")
            continue
        bad += 1
        print(f"{stem}: DIFFERS ({len(got)} titles, reference {len(want)})")
        for i in range(max(len(got), len(want))):
            g = got[i] if i < len(got) else "-"
            w = want[i] if i < len(want) else "-"
            if g != w:
                print(f"  ours {g}\n  ref  {w}")
    assert bad == 0, "title lists differ"


# ---- rules on built images ----

def disc(evobs, xpl, extra):
    return image([("ADV_OBJ", "VPLST000.XPL", xpl), ("HVDVD_TS", "HVA00001.VTI", vti(evobs))] + list(extra))


def files(names):
    """MAP + EVO files for EVOBs named names (each map: one entry of 1 block)."""
    f = []
    for n in names:
        base = n[:-len(".EVO")]
        f.append(("HVDVD_TS", f"{base}.MAP", tmap([1], 0, 0, False)))
        f.append(("HVDVD_TS", n, bytes(16)))
    return f


def titles(dump):
    return [ln for ln in dump.splitlines() if not ln.startswith("clip ")]


T = "00:00:00:00"


def ok(im, min_length):
    d = plan(im, min_length)
    assert not isinstance(d, int), f"plan failed: {d}"
    return d


def test_titles_come_from_seamless_runs_in_reverse_candidate_order():
    d_, a, b, c = (clip("D.MAP", T, T, False), clip("A.MAP", T, T, False), clip("B.MAP", T, T, True),
                   clip("C.MAP", T, T, False))
    body = (f'<FirstPlayTitle>{d_}</FirstPlayTitle><Title displayName="Main">{a}{b}{c}</Title>'
            f'<Title id="extra">{d_}</Title>')
    im = disc([evob("A.EVO", 1, 10), evob("B.EVO", 2, 20), evob("C.EVO", 3, 30), evob("D.EVO", 4, 40)],
              playlist('defaultLanguage="en"', body), files(["A.EVO", "B.EVO", "C.EVO", "D.EVO"]))
    # candidates sorted: [C], [D] (the FirstPlayTitle's; the Title's is the same), [A,B]
    assert titles(ok(im, 0)) == [
        "title 0|Main|eng|playlist|1|2700000|30|4096|A.EVO|2|A,B|0",
        "title 1|extra|eng|playlist|1|3600000|40|2048|D.EVO|1|D|0",
        "title 2|Main|eng|playlist|1|2700000|30|2048|C.EVO|1|C|0",
    ]


def test_an_evob_no_title_plays_becomes_a_title_only_when_there_is_a_title():
    im = disc([evob("A.EVO", 1, 10), evob("E.EVO", 5, 5)],
              playlist("", f"<Title>{clip('A.MAP', T, T, False)}</Title>"), files(["A.EVO", "E.EVO"]))
    assert titles(ok(im, 0)) == ["title 0|A||playlist|1|900000|10|2048|A.EVO|1|A|0",
                                 "title 1|E|(null)|evob|1|450000|5|2048|E.EVO|1|E|0"]
    # the only playlist title names an EVOB that does not exist: no title at all
    im = disc([evob("E.EVO", 5, 5)], playlist("", f"<Title>{clip('Z.MAP', T, T, False)}</Title>"), files(["E.EVO"]))
    assert ok(im, 0) == ""


def test_clip_names_match_the_first_evob_up_to_the_first_dot_without_case():
    body = ('<Title displayName="Feature"><PrimaryAudioVideoClip src="file:///dvddisc/hvdvd_ts/feature_1.map"/>'
            f'</Title><Title>{clip("B.EVO", T, T, False)}</Title><Title><PrimaryAudioVideoClip '
            'src="file:///other/B.MAP"/></Title>')
    f = files(["FEATURE_1.EVO", "B.EVO"]) + [("HVDVD_TS", "FEATURE_1.ALT", bytes(16))]
    im = disc([evob("FEATURE_1.ALT", 1, 10), evob("FEATURE_1.EVO", 2, 20), evob("B.EVO", 3, 1)],
              playlist("", body), f)
    # feature_1.map -> slot 1 FEATURE_1.ALT (the first match); its name is not "*.evo", so
    # no playlist Title names it: its base name. FEATURE_1.EVO (same time map) is played
    # by no title: a title of its own, named by the Title whose clip names it. The clips
    # "B.EVO" (no .map) and file:///other/ (other place) give no name: those titles are
    # left out; B.EVO becomes a title of its own.
    assert titles(ok(im, 0)) == [
        "title 0|FEATURE_1||playlist|1|900000|10|2048|FEATURE_1.ALT|1|FEATURE_1|0",
        "title 1|Feature|(null)|evob|1|1800000|20|2048|FEATURE_1.EVO|1|FEATURE_1|0",
        "title 2|B|(null)|evob|1|90000|1|2048|B.EVO|1|B|0",
    ]


def test_an_evob_whose_time_map_cannot_be_used_loses_its_titles():
    bad_magic = bytearray(tmap([1], 0, 0, False))
    bad_magic[:12] = b"HDDVD_TMAP01"
    no_table = bytearray(tmap([1], 0, 0, False))
    no_table[0x37:0x39] = bytes(2)
    names = ["G1", "G2", "G3", "G4", "G5", "H"]
    body = "".join(f"<Title>{clip(f'{n}.MAP', T, T, False)}</Title>" for n in names)
    evobs = [evob(f"{n}.EVO", i + 1, 1) for i, n in enumerate(names)]
    f = [
        ("HVDVD_TS", "G1.MAP", bytes(bad_magic)),
        ("HVDVD_TS", "G2.MAP", tmap([1], 0, 0x02, False)),
        ("HVDVD_TS", "G3.MAP", bytes(no_table)),
        ("HVDVD_TS", "G5.MAP", tmap([1], 0, 0, False)),
    ]
    f += [("HVDVD_TS", n, bytes(16)) for n in ["G1.EVO", "G2.EVO", "G3.EVO", "G4.EVO"]]
    f += files(["H.EVO"])
    d = ok(disc(evobs, playlist("", body), f), 0)
    assert titles(d) == ["title 0|H||playlist|1|90000|1|2048|H.EVO|1|H|0"]
    assert [ln for ln in d.splitlines() if ln.startswith("clip ")] == [
        "clip 1|G1.EVO|not usable|0|",
        "clip 2|G2.EVO|not usable|0|",
        "clip 3|G3.EVO|not usable|0|",
        "clip 4|G4.EVO|not usable|0|",
        "clip 5|G5.EVO|not usable|0|",
        "clip 6|H.EVO|usable|2048|0:0:1",
    ]


def test_the_size_counts_every_entry_and_an_entry_at_a_taken_block_is_not_mapped():
    # entries of 5, 0 and 3 blocks: the 3-block entry starts at block 5, taken by the
    # 0-block entry; the first table's value 7 is reported, a second table is read, the
    # top 3 bits of an entry are not part of the count
    im = disc([evob("A.EVO", 1, 10)], playlist("", f"<Title>{clip('A.MAP', T, T, False)}</Title>"),
              [("HVDVD_TS", "A.MAP", tmap([5, 0, 3], 7, 0x20, True)), ("HVDVD_TS", "A.EVO", bytes(16))])
    d = ok(im, 0)
    assert "title 0|A||playlist|1|900000|10|16384|A.EVO|1|A|0\n" in d, d
    assert "clip 1|A.EVO|usable|16384|0:0:5,5:5:0\n" in d, d


def chapter_disc(time_base):
    chapters = "".join(f'<Chapter titleTimeBegin="{t}" displayName="{n}"/>' for t, n in [
        ("00:00:00:00", "One"), ("00:00:30:30", "Two"), ("00:01:00:00", "Three"), ("00:01:30:00", "Four"),
        ("00:02:59:59", "Five")])
    body = (f"<Title>{clip('C1.MAP', '00:00:00:00', '00:01:00:00', False)}"
            f"{clip('C2.MAP', '00:01:00:00', '00:03:00:00', False)}"
            f"<ChapterList>{chapters}</ChapterList><ChapterList><Chapter/></ChapterList></Title>")
    return disc([evob("C1.EVO", 1, 60), evob("C2.EVO", 2, 120)], playlist(f'timeBase="{time_base}"', body),
                files(["C1.EVO", "C2.EVO"]))


def test_chapters_are_those_of_the_first_chapter_list_in_the_runs_window():
    # C1 (run 0): window 0..60000 ms. C2 (run 1): the window starts at 1 x the duration
    # of C2 itself (120000, not C1's 60000) and lasts 120000: only "Five" (179983) is in it
    assert titles(ok(chapter_disc("60fps"), 0)) == [
        "title 0|C2||playlist|1|10800000|120|2048|C2.EVO|1|C2|1",
        "  chapter 1|59983|Five",
        "title 1|C1||playlist|1|5400000|60|2048|C1.EVO|1|C1|3",
        "  chapter 1|0|One",
        "  chapter 2|30500|Two",
        "  chapter 3|60000|Three",
    ]
    # 50 frames per second: frame 30 is 600 ms
    assert "  chapter 2|30600|Two\n" in ok(chapter_disc("50fps"), 0)
    # a timeBase that is not 5 characters long: every time is 0, every chapter kept at 0
    d = ok(chapter_disc("60"), 0)
    assert d.count("|0|") == 10, d


def test_the_language_is_the_title_sets_default_as_an_iso_639_2_code_when_known():
    for attr, want in [("fr", "fre"), ("FRE", "fre"), ("xx", "xx"), ("", "")]:
        im = disc([evob("A.EVO", 1, 10)],
                  playlist(f'defaultLanguage="{attr}"', f"<Title>{clip('A.MAP', T, T, False)}</Title>"),
                  files(["A.EVO"]))
        d = ok(im, 0)
        assert d.startswith(f"title 0|A|{want}|"), f"{attr}: {d}"


def test_titles_shorter_than_the_minimum_are_not_selected_and_do_not_count_as_playing():
    # the only title is short: not selected, and no EVOB gets a title of its own
    im = disc([evob("A.EVO", 1, 5), evob("E.EVO", 2, 50)],
              playlist("", f"<Title>{clip('A.MAP', T, T, False)}</Title>"), files(["A.EVO", "E.EVO"]))
    assert titles(ok(im, 10)) == ["title 0|A||playlist|0|450000|5|2048|A.EVO|1|A|0"]
    # with a selected title, the short title's EVOB is played by no selected title
    body = f"<Title>{clip('A.MAP', T, T, False)}</Title><Title>{clip('B.MAP', T, T, False)}</Title>"
    im = disc([evob("A.EVO", 1, 5), evob("B.EVO", 2, 50)], playlist("", body), files(["A.EVO", "B.EVO"]))
    assert titles(ok(im, 10)) == [
        "title 0|B||playlist|1|4500000|50|2048|B.EVO|1|B|0",
        "title 1|A||playlist|0|450000|5|2048|A.EVO|1|A|0",
        "title 2|A|(null)|evob|0|450000|5|2048|A.EVO|1|A|0",
    ]


def test_every_playlist_needs_exactly_one_title_set():
    xpl = b"<Playlist><TitleSet/><TitleSet/></Playlist>"
    assert plan(disc([evob("A.EVO", 1, 1)], xpl, files(["A.EVO"])), 0) == INVALIDDATA
    assert plan(disc([evob("A.EVO", 1, 1)], b"<Playlist/>", files(["A.EVO"])), 0) == INVALIDDATA
