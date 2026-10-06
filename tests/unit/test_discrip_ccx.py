"""libavformat/discrip_ccx.c: closed captions to text. The CCExtractor
helper (dist/<mode>/ccextractor) takes the caption blocks in its raw caption
format on stdin and writes SRT, which is read back strictly into cues."""

from helpers import DIST, ffi, lib
from test_helper import EDM, EOC, load, triplets

HELPER = str(DIST / "ccextractor").encode()


def parse(srt):
    """(return code, [(start ms, end ms, text)])."""
    cues, n = ffi.new("DRCue **"), ffi.new("int *")
    data = srt.encode() if isinstance(srt, str) else srt
    r = lib.ff_discrip_srt_parse(ffi.NULL, data, len(data), cues, n)
    out = [(cues[0][i].start_ms, cues[0][i].end_ms, ffi.string(cues[0][i].text).decode()) for i in range(n[0])]
    lib.ff_discrip_cues_free(cues, n[0])
    return r, out


def convert(blocks, program=HELPER):
    """The cues for blocks [(ms, pairs)] through the converter: (return code
    of start, of finish, cues)."""
    c = ffi.new("DRCcx **")
    r = lib.ff_discrip_ccx_start(c, ffi.NULL, program)
    if r < 0:
        return r, None, []
    for ms, pairs in blocks:
        t = triplets(pairs)
        assert lib.ff_discrip_ccx_block(c[0], ms, t, len(t) // 3) == 0
    cues, n = ffi.new("DRCue **"), ffi.new("int *")
    f = lib.ff_discrip_ccx_finish(c[0], cues, n)
    out = [(cues[0][i].start_ms, cues[0][i].end_ms, ffi.string(cues[0][i].text).decode()) for i in range(n[0])]
    lib.ff_discrip_cues_free(cues, n[0])
    lib.ff_discrip_ccx_close(c)
    return r, f, out


def test_the_srt_reader_takes_cues_as_the_helper_writes_them():
    srt = ("\ufeff1\r\n00:00:03,604 --> 00:00:05,371\r\nLACROIX:\r\n<i> Do you remember?</i>\r\n\r\n2\r\n"
           "01:02:03,004 --> 01:02:05,000\r\nX\r\n\r\n")
    assert parse(srt) == (0, [(3604, 5371, "LACROIX:\n<i> Do you remember?</i>"), (3723004, 3725000, "X")])
    # LF only, no BOM, no empty line at the end
    assert parse("1\n00:00:01,000 --> 00:00:02,000\nA\nB") == (0, [(1000, 2000, "A\nB")])


def test_the_srt_reader_refuses_what_the_helper_does_not_write():
    bad = [
        "2\r\n00:00:01,000 --> 00:00:02,000\r\nX\r\n",    # numbers start at 1
        "1\r\n00:00:01,000 -> 00:00:02,000\r\nX\r\n",     # timing line
        "1\r\n0:00:01,000 --> 00:00:02,000\r\nX\r\n",     # timing line length
        "1\r\n00:00:01,0\u00e9 --> 00:00:02,000\r\nX\r\n",  # not ASCII
        "1\r\n00:00:01,000 --> 00:00:02,000\r\nX\r\n\r\n3\r\n00:00:03,000 --> 00:00:04,000\r\nY\r\n",  # a gap
    ]
    for s in bad:
        r, cues = parse(s)
        assert r < 0, s
        assert cues == []


def test_caption_blocks_become_cues_through_the_converter():
    r, f, cues = convert([(500, load(b"HELLO WORLD")), (1000, EOC), (3000, EDM), (4000, load(b"SECOND")),
                          (4500, EOC), (6000, EDM)])
    assert (r, f) == (0, 0)
    # the helper keeps 1 ms off both ends and pads lines to 32 columns
    assert cues == [(1001, 2999, "HELLO WORLD                     "), (4501, 5999, "SECOND                          ")]


def test_no_blocks_no_cues():
    assert convert([]) == (0, 0, [])


def test_a_converter_that_cannot_start_or_fails_is_an_error():
    r, _, _ = convert([], b"/nonexistent/ccextractor")
    assert r < 0
    # a program that exits with an error status
    r, f, cues = convert([(500, load(b"X"))], b"/bin/false")
    assert r == 0 and f < 0 and cues == []
