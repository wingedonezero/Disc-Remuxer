"""The program's own message numbers (src/base/msg.h): each used once, each
in its area (1000 program and settings, 2000 sources and discs, 3000
titles, 5000 streams and checks, 6000 output), so a number keeps one
meaning and says its area."""

import re

from helpers import ROOT

AREAS = (1000, 2000, 3000, 5000, 6000)


def numbers():
    text = (ROOT / "src" / "base" / "msg.h").read_text()
    return re.findall(r"^\s*(MSG_[A-Z0-9_]+)\s*=\s*(\d+)\s*,", text, re.M)


def test_every_message_number_is_unique_and_in_its_area():
    found = numbers()
    assert len(found) >= 30
    values = [int(v) for _, v in found]
    assert len(set(values)) == len(values), [n for n, v in found if values.count(int(v)) > 1]
    assert len({n for n, _ in found}) == len(found)
    for name, v in found:
        assert int(v) // 1000 * 1000 in AREAS, name
