"""Broken navigation data must not abort the program: the VM records the
broken assumption and stops (libdvdnav upstream asserts)."""

from helpers import ffi, lib

LINK_PREV_PG = bytes([0x20, 0x01, 0, 0, 0, 0, 0, 0x07])
"""DVD-Video command LinkPrevPG (link instruction, sub-instruction 7, no
condition, no button)."""


def exec_(command, programs, pg_n):
    failures = ffi.new("unsigned *")
    first = ffi.new("const char **")
    ret = lib.dr_selftest_vm_exec(command, programs, pg_n, failures, first)
    return ret, failures[0], ffi.string(first[0]).decode()


def test_link_prev_pg_at_the_first_program_without_a_previous_chain_is_recorded_not_aborted():
    # At program 1 LinkPrevPG goes to the previous program chain; this chain
    # has none, a broken assumption that is recorded (upstream asserts).
    ret, failures, first = exec_(LINK_PREV_PG, 3, 1)
    assert ret == 0, "the command must not continue playback"
    assert failures == 1
    assert "prev_pgc_nr != 0" in first, f"first failure: {first}"
