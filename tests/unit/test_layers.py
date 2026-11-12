"""The one rule's guard (docs/ARCHITECTURE.md): no file of ours but those
of src/analyze/ (it answers questions) and src/convert/ (the one
conversion: lossless audio at cuts as FLAC) includes a header of the
processing library. The build leaves FFmpeg's include path out of every
other module, but a compiler also finds a system-wide FFmpeg's headers, so
the sources are checked."""

import re

from helpers import ROOT

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](libav[a-z]*|libsw[a-z]*|libpostproc)/', re.M)
MAY = ("analyze", "convert")


def test_only_analyze_and_convert_include_the_processing_library():
    src = ROOT / "src"
    found = [str(p.relative_to(src)) for p in sorted(src.rglob("*"))
             if p.suffix in (".c", ".h", ".cpp") and p.relative_to(src).parts[0] not in MAY
             and INCLUDE.search(p.read_text(errors="replace"))]
    assert not found, found
    # the guard sees them: both modules do include it
    for module in MAY:
        assert any(INCLUDE.search(p.read_text(errors="replace")) for p in (src / module).glob("*.c")), module
