"""The cut editor's analysis modes (src/disc-remuxer/cut_modes.c): the MKV
splitter's modes (Remux-Toolkit), the same log and split points. The cases
in tests/data/cut_modes_cases.json carry what the splitter's Python gave
for each (made once with it): real chapter lists (a joined title of seven
episodes, two episodes in one stream, a DVD episode), equal episodes (the
equal-division fit, gaps of exactly the same length), every mode's own
refusals and Python's own error."""

import json

import pytest

from helpers import DATA, ffi, lib

CASES = json.loads((DATA / "cut_modes_cases.json").read_text())


def run(case):
    ch = ffi.new("int64_t[]", case["chapters"] or [0])
    names = [ffi.new("char[]", n.encode()) for n in case["names"]]
    na = ffi.new("const char *[]", names or [ffi.NULL])
    ticked = ffi.new("int[]", case["ticked"] or [0])
    stamps = ffi.new("char[]", case["stamps"].encode())
    inp = ffi.new("CutModeInput *", {"mode": case["mode"], "chapters": ch, "names": na,
                                     "nb_chapters": len(case["chapters"]), "duration": case["duration"],
                                     "target": case["target"], "min_minutes": case["min"],
                                     "episodes": case["episodes"], "from_end": case["from_end"],
                                     "ticked": ticked, "nb_ticked": len(case["ticked"]), "timestamps": stamps})
    out = ffi.new("CutModeResult *")
    assert lib.cut_mode_run(inp, out) == 0
    log = ffi.string(out.log).decode()
    points = [str(out.chapters[i]) for i in range(out.nb_chapters)] + \
             [ffi.string(out.stamps[i]).decode() for i in range(out.nb_stamps)]
    lib.cut_mode_result_free(out)
    return log, points


@pytest.mark.parametrize("case", CASES, ids=[c["name"] for c in CASES])
def test_mode_as_the_splitter(case):
    log, points = run(case)
    assert points == case["points"]
    assert log.split("\n") == case["log"].split("\n")


def test_modes_names_and_keys():
    names = [ffi.string(lib.cut_mode_names[m]).decode() for m in range(lib.CUT_MODE_COUNT)]
    assert names == ["Time-based Grouping", "Pattern Recognition", "Statistical Gap Analysis",
                     "Shortest Chapter Analysis", "Manual Episode Count", "Remove Chapters from End",
                     "Before Chapters (Manual)", "After Timestamps (Manual)"]
    for m in range(lib.CUT_MODE_COUNT):
        assert lib.cut_mode_of_key(lib.cut_mode_keys[m]) == m
    assert lib.cut_mode_of_key(b"nope") == -1


def target(text):
    v = ffi.new("double *")
    err = ffi.new("char[]", 256)
    if lib.cut_mode_target(text.encode(), v, err, 256) < 0:
        return ffi.string(err).decode()
    return v[0]


def test_target_as_the_splitters_field():
    # mm:ss or minutes, as its parse_target_duration() reads them
    assert target("23:40") == 23 + 40 / 60.0
    assert target(" 23 ") == 23.0
    assert target("23.5") == 23.5
    assert target("") == "Target episode duration is empty. Use mm:ss, e.g. 23:40."
    assert target("23:") == "Invalid target duration '23:'. Use mm:ss, e.g. 23:40."
    assert target("abc") == "Invalid target duration 'abc'. Use mm:ss, e.g. 23:40."
    assert target("0:00") == "Target episode duration must be greater than zero."


def test_target_text_and_chapter_start_text():
    buf = ffi.new("char[]", 40)
    for minutes, text in ((23 + 40 / 60.0, "23:40"), (23.0, "23:00"), (59.999, "60:00"), (22.5, "22:30")):
        lib.cut_mode_target_text(minutes, buf, 40)
        assert ffi.string(buf).decode() == text
    # a chapter's start as the splitter writes it: whole microseconds, then ms
    lib.cut_mode_start_text(1372204166667, buf, 40)
    assert ffi.string(buf).decode() == "0:22:52.204"
