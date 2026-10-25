"""dvd.clean_break_parts (src/disc/dvd/dvd_title_list.c find_parts(),
dvd_part.c): a DVD title's parts at its clean breaks, a cell starting
another VOB with the clock restarting and a chapter starting there. A built
DVD title of two separately authored pieces, the MPEG-2 test stream with
AC-3 audio cut at a GOP: the second piece in a VOB of its own, its clock
restarting (its times from the start again), a chapter of its own. Off: no
parts; on: two, each written from its own start with --split (a folder each,
the piece's pictures byte for byte), never by output.split_joined_titles."""

import subprocess

from helpers import DATA, PROGRAM
from helpers.disctl import pictures
from helpers.dvdnav import Pgc, build, cell
from synth import dvd
from synth.dvd import flags
from test_dvd_streams import put
from test_dvd_titles import ac3, info

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
BASE = 90000


def two_pieces(tmp_path, vob2=2, stc=True, chapter=True):
    """The title (two cells, one per piece) and each piece's pictures."""
    pics = pictures(M2V)
    gops = [k for k, (a, b, _, _) in enumerate(pics) if b"\x00\x00\x01\xb8" in M2V[a:b][:64]]
    cut = gops[len(gops) // 2]
    frames = [AC3[i:i + 768] for i in range(0, len(AC3), 768)]
    pieces, sectors, navs = [], [], []
    for part in (pics[:cut], pics[cut:]):
        d0 = part[0][2]
        timed = [(a, b, disp - d0, typ, BASE + (disp - d0) * 3003) for a, b, disp, typ in part]
        n = (len(part) * 3003) // 2880
        audio = [(frames[k % len(frames)], BASE + k * 2880) for k in range(n)]
        s, nv = dvd.mux_vobus(M2V, timed, len(sectors), audio, BASE - 3003)
        pieces.append((len(sectors), len(sectors) + len(s) - 1, nv[-1].lbn, len(part)))
        sectors += s
        navs += nv
    secs = [n * 1001 // 30000 for *_, n in pieces]
    p = Pgc(cells=[cell(pieces[0][0], pieces[0][1], secs[0], last_vobu=pieces[0][2], vob_id_nr=1, cell_nr=1),
                   cell(pieces[1][0], pieces[1][1], secs[1], last_vobu=pieces[1][2], vob_id_nr=vob2,
                        cell_nr=1 if vob2 != 1 else 2, flags=flags.STC_DISCONTINUITY if stc else 0)],
            programs=[1, 2] if chapter else [1], audio_control=bytes([0x80, 0]) + bytes(14))
    ptts = [[(1, 1), (1, 2)]] if chapter else [[(1, 1)]]
    folder = build(tmp_path, [p], ptts, navs, len(sectors), video_attr=bytes([0x40, 0]), audio_attrs=[ac3(b"en")])
    for k, sec in enumerate(sectors):
        put(folder, k, sec)
    return folder, [M2V[pics[0][0]:pics[cut][0]], M2V[pics[cut][0]:]]


ON = "dvd.clean_break_parts=true"


def demux(folder, out, *opts):
    p = subprocess.run([*PROGRAM, *opts, "demux", str(folder), "0", str(out)], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    return p.stdout + p.stderr


def m2v_files(out):
    return {f.parent.name: f.read_bytes() for f in out.rglob("*.m2v")}


def test_without_the_setting_a_dvd_title_has_no_parts(tmp_path):
    folder, _ = two_pieces(tmp_path)
    ts, _ = info(folder)
    assert "parts" not in ts[0]


def test_a_clean_break_gives_a_part_from_it(tmp_path):
    folder, _ = two_pieces(tmp_path)
    ts, lines = info(folder, ON)
    parts = ts[0]["parts"]
    assert [(q["part"], q["chapters"], q["files"]) for q in parts] == [(1, 1, ["1"]), (2, 1, ["2"])]
    assert parts[0]["start"] == 0 and abs(parts[1]["start"] - parts[0]["duration"]) < 0.001
    assert any("title 1: dvd.clean_break_parts: 2 parts, from its start and each clean break (another VOB, the "
               "clock restarting and a chapter starting), at 0:00:00, 0:00:" in x.get("text", "") for x in lines)


def test_a_break_needs_another_vob_the_clock_restarting_and_a_chapter(tmp_path):
    for k, kw in enumerate([dict(vob2=1), dict(chapter=False)]):
        folder, _ = two_pieces(tmp_path / str(k), **kw)
        ts, _ = info(folder, ON)
        assert "parts" not in ts[0], kw


def test_split_writes_each_part_from_its_own_start(tmp_path):
    folder, pieces = two_pieces(tmp_path)
    log = demux(folder, tmp_path / "out", "--set", ON, "--set", "output.format=streams", "--split", "0")
    got = m2v_files(tmp_path / "out")
    assert sorted(got) == ["title_t00_p1", "title_t00_p2"], log
    assert got["title_t00_p1"].startswith(pieces[0][:4096]) and got["title_t00_p2"].startswith(pieces[1][:4096])
    assert pieces[1] in got["title_t00_p2"] and pieces[0] in got["title_t00_p1"], "each piece's pictures whole"
    assert pieces[1] not in got["title_t00_p1"]


def test_the_always_on_split_setting_leaves_dvd_parts_whole(tmp_path):
    folder, _ = two_pieces(tmp_path)
    demux(folder, tmp_path / "out", "--set", ON, "--set", "output.format=streams", "--set",
          "output.split_joined_titles=true")
    assert sorted(m2v_files(tmp_path / "out")) == ["title_t00"]
