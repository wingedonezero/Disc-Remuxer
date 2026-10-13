"""The stream files' names (src/disc-remuxer/writer.c) in the shape the
MKVToolNix GUI reads a file name in (its merge/source_file.cpp, track.cpp and
util/settings.cpp): the name is split at [ ] ( ) { } . + - = # (not at _ or
a space), the last piece (the extension) is dropped and the pieces are tried
from the right for a language code; "delay <number>" is the audio delay;
forced / commentary between those separators set the flags. The language
stands between brackets, the disc's marks in parentheses; the video, which
no disc names a language for, takes setting output.video_language. A built
Blu-ray (tests/synth/bluray.py): MPEG-2 video, AC-3 in Spanish, AC-3 with no
language, PGS in English with a forced object (its forced-only copy is a
track of its own)."""

import json
import re
import subprocess

from helpers import DATA, PROGRAM, ffi, lib
from synth import bluray as bd
from synth.bluray import Item, Stream
from test_disctl_pgs import display_set, obj

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
S, E = 27000000, 27000000 + 90000 * 4

SEPARATORS = r"[\[\](){}.+=#-]"


def packets(pid, chunks, stream_id, step, start=900, ext=None):
    out = []
    for i, c in enumerate(chunks):
        out += bd.ts_packets(pid, bd.pes(stream_id, c, ext=ext, pts=start + step * i))
    return out


def disc(tmp_path, meta=()):
    """meta: the languages of disc library files (BDMV/META/DL/bdmt_<lang>.xml) to put on it."""
    pgs = [display_set([obj(0, forced=True), obj(1)]), display_set([obj(2)], 2)]
    data = bd.source_units(packets(0x1011, [M2V[i:i + 60000] for i in range(0, len(M2V), 60000)], 0xe0, 3003) +
                           packets(0x1100, [AC3[i:i + 768] for i in range(0, len(AC3), 768)], 0xfd, 2880, ext=0x71) +
                           packets(0x1101, [AC3[i:i + 768] for i in range(0, len(AC3), 768)], 0xfd, 2880, ext=0x71) +
                           packets(0x1200, pgs, 0xbd, 30030, start=900 + 3003 * 5))
    streams = ((0x1011, 0x02, b"\x31"), (0x1100, 0x81, b"\x31spa"), (0x1101, 0x81, b"\x31und"),
               (0x1200, 0x90, b"eng"))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=streams)}
    item = Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")],
                audio=[Stream(0x1100, 0x81, b"\x31spa"), Stream(0x1101, 0x81, b"\x31und")],
                pg=[Stream(0x1200, 0x90, b"eng")])
    files = bd.bdmv(clips, {1: bd.mpls([item])})
    files["BDMV/STREAM/00001.m2ts"] = data
    for lang in meta:
        files[f"BDMV/META/DL/bdmt_{lang}.xml"] = bd.bdmt("A name", lang)
    return bd.write(tmp_path / "disc", files)


def demux(tmp_path, *opts):
    out = tmp_path / "out"
    p = subprocess.run([*PROGRAM, *opts, "--json", "demux", str(disc(tmp_path)), "0", str(out)],
                       capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    assert p.returncode == 0, p.stdout + p.stderr
    return sorted(f.name for f in out.rglob("*") if f.is_file() and not f.name.endswith((".log", ".xml"))), recs


def gui_language(name):
    """The language the MKVToolNix GUI takes from a file name: the pieces
    between separators, the extension dropped, tried from the right (any
    ISO 639 code here; the GUI only knows the ones with a two-letter code)."""
    pieces = [p for p in re.split(SEPARATORS + "+", name) if p][:-1]
    for p in reversed(pieces):
        r = lib.dr_disc_lang_code(p.encode())
        if r != ffi.NULL and ffi.string(r).decode() != "und":
            return ffi.string(r).decode()
    return None


def gui_delay(name):
    m = re.search(r"delay\s+(-?\d+)", name, re.IGNORECASE)
    return int(m.group(1)) if m else None


def gui_flag(word, name):
    return bool(re.search(f"(^|{SEPARATORS})({word})({SEPARATORS}|$)", name, re.IGNORECASE))


def named(files, part):
    found = [f for f in files if part in f]
    assert len(found) == 1, (part, files)
    return found[0]


def test_the_names_carry_language_flags_and_delay_as_the_mkvtoolnix_gui_reads_them(tmp_path):
    files, _ = demux(tmp_path)
    video = named(files, "_00_")
    spa = named(files, "_01_")
    und = named(files, "_02_")
    pgs = named(files, "_03_")
    forced = named(files, "_04_")
    assert "_00_[eng]_" in video, video
    assert "_01_[spa]_DD_" in spa and "DELAY " in spa, spa
    assert "_02_[und]_DD_" in und, und
    assert "_03_[eng]_PGS" in pgs and "(forced)" not in pgs, pgs
    assert "_04_[eng]_PGS (forced)" in forced, forced
    # what the GUI makes of them
    assert gui_language(spa) == "spa" and gui_delay(spa) is not None
    assert gui_language(und) is None, "no language: the GUI keeps its own default"
    assert gui_language(pgs) == "eng" and not gui_flag("forced|signs", pgs)
    assert gui_language(forced) == "eng" and gui_flag("forced|signs", forced)
    assert gui_language(video) == "eng", "the name holds it (the GUI's default takes no video language from names)"


def test_the_video_language_is_a_setting_and_shows_in_the_title_list(tmp_path):
    files, _ = demux(tmp_path, "--set", "output.video_language=fra")
    assert "_00_[fre]_" in named(files, "_00_"), "as the disc info writes codes (ISO 639-2/B)"
    files, _ = demux(tmp_path / "none", "--set", "output.video_language=")
    assert "_00_[und]_" in named(files, "_00_")
    p = subprocess.run([*PROGRAM, "--json", "info", str(disc(tmp_path / "info"))], capture_output=True, text=True)
    title = next(json.loads(l) for l in p.stdout.splitlines() if l.startswith('{"type":"title"'))
    assert [(k["kind"], k["language"]) for k in title["tracks"]][:2] == [("video", "eng"), ("audio", "spa")]


def test_a_video_language_that_is_no_language_code_stops_the_run(tmp_path):
    p = subprocess.run([*PROGRAM, "--set", "output.video_language=klingon", "info", str(disc(tmp_path))],
                       capture_output=True, text=True)
    assert p.returncode != 0 and "video_language 'klingon' is no ISO 639 language code" in p.stdout + p.stderr


def video_language(tmp_path, meta, *opts):
    p = subprocess.run([*PROGRAM, *opts, "--json", "info", str(disc(tmp_path, meta))], capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    d = next(r for r in recs if r.get("type") == "disc")
    t = next(r for r in recs if r.get("type") == "title")
    return t["tracks"][0]["language"], d["properties"]["Market language"]


def test_the_video_takes_the_disc_s_market_language_else_english(tmp_path):
    # market (the default): the languages of the disc library files; one: that
    # one; several in no order: English when it is among them; none: English
    assert video_language(tmp_path / "jpn", ["jpn"]) == ("jpn", "jpn (its disc library files)")
    assert video_language(tmp_path / "both", ["jpn", "eng"])[0] == "eng"
    assert video_language(tmp_path / "none", []) == ("eng", "none named")
    # a code is every disc's
    assert video_language(tmp_path / "set", ["jpn"], "--set", "output.video_language=fra")[0] == "fre"
