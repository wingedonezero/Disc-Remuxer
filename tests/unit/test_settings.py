"""The settings are one system (src/base/settings.c): every setting of
the registry is read by the program (the engine, or the GUI for its own),
and every option of the disc layer (DrDiscOptions, src/disc/disc.h) is set
from a setting or is a choice of the run, so nothing the user may want to
change sits outside the settings file."""

import re

from helpers import ROOT

SRC = ROOT / "src"
SETTINGS = (SRC / "base" / "settings.c").read_text()
DISC_H = (SRC / "disc" / "disc.h").read_text()
WRITER = (SRC / "disc-remuxer" / "writer.c").read_text()

# options of the disc layer that are a run's choice, not a setting
RUN_CHOICES = {
    "title": "the disc info only here: the titles are opened one by one (dr_disc_title_open())",
    "part": "a part of a title: dr_disc_title_open() (--split, the GUI's row menu)",
    "second_view": "a 3-D title's second view: dr_disc_title_open(), from the tracks chosen",
    "details": "the title list reads each track's details: always on for the program",
    "cc_helper": "the caption converter: the one next to the program",
    "nb_keydb": "the key file count, with keydb",
}


def registry():
    block = SETTINGS[SETTINGS.index("static const Setting registry[]"):]
    block = block[:block.index("\n};")]
    entries = block.split('\n    { "')[1:]      # one entry each: "group", "key", kind, ..., kept, use }
    out = []
    for e in entries:
        m = re.match(r'(\w+)", "(\w+)", SET_\w+,', e)
        use = re.search(r',\s+[01],\s+(SET_\w+) \},\s*$', e)
        assert m and use, e[:80]
        out.append((m.group(1), m.group(2), use.group(1)))
    return out


def program_sources():
    files = [p for p in SRC.rglob("*") if p.suffix in (".c", ".cpp") and p.name != "settings.c"]
    return {p: p.read_text() for p in files}


def test_the_registry_is_read_whole():
    names = registry()
    assert len(names) >= 18 and len({(g, k) for g, k, _ in names}) == len(names)


def test_every_setting_is_read_by_the_program():
    sources = program_sources()
    for group, key, use in registry():
        users = [p.name for p, text in sources.items() if f'"{group}.{key}"' in text]
        assert users, f"{group}.{key} is in the settings file but nothing reads it"
        if use == "SET_IN_GUI":
            gui = {p.name for p in (SRC / "gui").glob("*.cpp")}
            assert all(p in gui for p in users), (group, key, users)
        else:
            assert any(not p.endswith(".cpp") for p in users), f"{group}.{key}: the engine does not read it"


def test_every_option_of_the_disc_layer_is_a_setting_or_a_run_choice():
    block = DISC_H[DISC_H.index("typedef struct DrDiscOptions"):]
    block = block[:block.index("} DrDiscOptions;")]
    options = re.findall(r'^\s+(?:const char \*const \*|const char \*|int64_t|int)\s*(\w+);', block, re.M)
    opened = WRITER[WRITER.index("int open_disc("):]
    opened = opened[:opened.index("\n}\n")]
    set_there = set(re.findall(r'\bo\.(\w+)\s*=', opened))
    assert "title" in options and "udf_reader" in options and len(options) >= 12
    for o in options:
        assert o in set_there or o in RUN_CHOICES, f"disc layer option {o}: no setting sets it"
    # and what open_disc sets comes from a setting (or is a run's choice)
    for o in set_there - {"keydb", "nb_keydb"} - set(RUN_CHOICES):
        line = next(l for l in opened.splitlines() if re.search(rf"\bo\.{o}\s*=", l))
        assert "setting_" in line, line
    assert "key_files(&keydb" in opened     # keydb: setting aacs.key_files, or the keydb folder
