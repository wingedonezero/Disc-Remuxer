"""The log's repeat filter (src/base/log.c): a library line already shown
in this run, given again while a disc or a title is opened (a title opened
again repeats the disc's messages), becomes a detail line; while a title is
read (log_title_reading()) every line is that title's own, shown each time
and not kept as seen. Decided by when a line comes, not by its words."""

from helpers import ffi, lib


def lines(steps):
    """Runs steps (("line", level, text) or ("reading", on)) with a sink
    installed; returns the (level, text) of each line as logged."""
    got = []

    @ffi.callback("void(void *, int, int, const char *, const char *, int)")
    def line(opaque, level, code, source, text, heading):
        got.append((level, ffi.string(text).decode()))

    sink = ffi.new("LogSink *", {"line": line})
    lib.log_set_sink(sink)
    try:
        for s in steps:
            if s[0] == "reading":
                lib.log_title_reading(s[1])
            else:
                lib.log_library_line(s[1], b"test", s[2].encode())
    finally:
        lib.log_title_reading(0)
        lib.log_set_sink(ffi.NULL)
    return got


def test_a_repeat_while_opening_is_a_detail_line():
    t = "repeat filter test: a disc's note"
    assert lines([("line", lib.LOG_WARNING, t), ("line", lib.LOG_WARNING, t)]) == \
        [(lib.LOG_WARNING, t), (lib.LOG_DETAIL, t)]


def test_while_a_title_is_read_every_line_shows_whatever_its_words():
    t = "Damaged: repeat filter test, a line read twice"
    out = lines([("reading", 1), ("line", lib.LOG_WARNING, t), ("line", lib.LOG_WARNING, t),
                 ("reading", 0), ("line", lib.LOG_WARNING, t)])
    # the read lines are not kept as seen: the same words when opening again are new
    assert out == [(lib.LOG_WARNING, t)] * 3


def test_a_title_s_own_words_opening_are_filtered_like_any_line():
    t = "Title check: repeat filter test, said while opening"
    assert lines([("line", lib.LOG_INFO, t), ("line", lib.LOG_INFO, t)]) == [(lib.LOG_INFO, t), (lib.LOG_DETAIL, t)]
