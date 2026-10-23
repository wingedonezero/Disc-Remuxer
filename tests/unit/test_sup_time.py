"""A PGS display set's time in its .sup (writer.c writer_sup_time): the 90 kHz
tick in the 32 bits a .sup's "PG" header holds, up to 2^32 ticks (13 h 15 min
21.858 s); past it the tick does not fit, the .sup keeps its low 32 bits (a
time that starts again from 0, as every program's .sup does) and the writer
counts it for the title's warning."""

from helpers import ffi, lib

TICK = lib.DR_TICKS_PER_SECOND // 90000


def sup_time(ticks):
    pts = ffi.new("uint32_t *")
    wrapped = lib.writer_sup_time(ticks, pts)
    return wrapped, pts[0]


def test_a_time_the_sup_holds_is_its_90_khz_tick():
    assert sup_time(0) == (0, 0)
    assert sup_time(3600 * lib.DR_TICKS_PER_SECOND) == (0, 3600 * 90000)
    assert sup_time((2**32 - 1) * TICK) == (0, 2**32 - 1), "the last tick the 32 bits hold"


def test_a_time_past_13_h_15_min_does_not_fit_and_keeps_its_low_32_bits():
    assert sup_time(2**32 * TICK) == (1, 0), "13:15:21.858: the first tick past the 32 bits"
    assert sup_time(14 * 3600 * lib.DR_TICKS_PER_SECOND) == (1, 14 * 3600 * 90000 - 2**32)
