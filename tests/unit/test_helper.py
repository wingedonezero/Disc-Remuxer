"""The built CCExtractor helper (dist/<mode>/ccextractor): CEA-608 caption
blocks in its raw caption format (RCWT: millisecond time, cc_data triplets)
on stdin, SRT cues on stdout, run with the options the program uses."""

import subprocess

from helpers import DIST

ARGS = ["-in=bin", "-out=srt", "-stdin", "-stdout", "-utf8", "-nobi", "-ff"]
# magic, creator, version, format revision 1 (CEA-608 blocks)
RCWT_HEADER = bytes([0xCC, 0xCC, 0xED, 0xCC, 0x00, 0x50, 0x00, 0x01, 0x00, 0x00, 0x00])


def parity(b):
    """A CEA-608 byte with odd parity."""
    return b | 0x80 if bin(b).count("1") % 2 == 0 else b


def triplets(pairs):
    """Triplets: each field-1 pair followed by a field-2 null pair."""
    return b"".join(bytes([0x04, parity(a), parity(b), 0x05, 0x80, 0x80]) for a, b in pairs)


def load(text):
    """Erase non-displayed memory, resume caption loading, row 15, the text."""
    v = [(0x14, 0x2E), (0x14, 0x2E), (0x14, 0x20), (0x14, 0x20), (0x14, 0x70), (0x14, 0x70)]
    t = text + (b"\0" if len(text) % 2 else b"")
    return v + [(t[i], t[i + 1]) for i in range(0, len(t), 2)]


EOC = [(0x14, 0x2F), (0x14, 0x2F)]
EDM = [(0x14, 0x2C), (0x14, 0x2C)]


def srt_ms(s):
    return ((int(s[0:2]) * 3600 + int(s[3:5]) * 60 + int(s[6:8])) * 1000) + int(s[9:12])


def convert(blocks):
    """The cues (start ms, end ms, text) the helper writes for blocks [(ms,
    pairs)]; a block without entries is not written."""
    data = bytearray()
    for ms, pairs in blocks:
        t = triplets(pairs)
        if not t:
            continue
        if not data:
            data += RCWT_HEADER
        data += ms.to_bytes(8, "little") + (len(t) // 3).to_bytes(2, "little") + t
    r = subprocess.run([str(DIST / "ccextractor")] + ARGS, input=bytes(data), capture_output=True, check=True)
    if not data:
        return []
    text = r.stdout.decode("utf-8").lstrip("﻿").replace("\r", "")
    cues = []
    for block in text.split("\n\n"):
        lines = [x for x in block.split("\n") if x != ""]
        if not lines:
            continue
        assert lines[0] == str(len(cues) + 1)
        start, end = lines[1].split(" --> ")
        cues.append((srt_ms(start), srt_ms(end), "".join(x + "\n" for x in lines[2:])))
    return cues


def test_pop_on_captions_become_srt_cues():
    cues = convert([
        (500, load(b"HELLO WORLD")),
        (1000, EOC),
        (3000, EDM),
        (4000, load(b"SECOND")),
        (4500, EOC),
        (6000, EDM),
    ])
    # CCExtractor keeps 1 ms off both ends; lines are padded to 32 columns
    assert cues == [
        (1001, 2999, "HELLO WORLD                     \n"),
        (4501, 5999, "SECOND                          \n"),
    ]


def test_no_blocks_no_cues():
    assert convert([]) == []
