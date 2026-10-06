"""Our libdvdnav's VM rules (libs/README.md): GPRM counters on the playback
clock (a set restarts the counter from 0 and is recorded when its value is
not 0), the Rnd operation from an application source, the deterministic
random program order, and the tolerant link rules."""

import dataclasses
import struct

from helpers import ffi, lib


@dataclasses.dataclass
class Run:
    """What one run of the VM left behind."""
    ret: int
    gprm: list
    pg_n: int
    failures: int
    ignored: int
    ign_reg: int
    ign_value: int


RND = {}


@ffi.def_extern()
def tb_py_rnd(priv):
    return RND["fn"]()


def run(commands, ticks_between, rnd=None):
    flat = b"".join(bytes(c) for c in commands)
    gprm = ffi.new("uint16_t[16]")
    out = [ffi.new("int *"), ffi.new("unsigned *"), ffi.new("unsigned *"), ffi.new("int *"), ffi.new("int *")]
    RND["fn"] = rnd
    ret = lib.dr_selftest_vm_run(flat, len(commands), 3, 2, ticks_between, lib.tb_py_rnd if rnd else ffi.NULL,
                                 ffi.NULL, gprm, *out)
    return Run(ret, list(gprm), *(o[0] for o in out))


def set_counter(reg, value):
    """SetGPRMMD: GPRM reg to counter mode, set to value (immediate)."""
    v = struct.pack(">H", value)
    return [0x53, 0, v[0], v[1], 0, 0x80 | reg, 0, 0]


def mov(dst, src):
    """Set (mov): GPRM dst = GPRM src."""
    return [0x61, 0, 0, dst, 0, src, 0, 0]


def rnd(dst, n):
    """Set (Rnd): GPRM dst = a random number from 1 to n (immediate)."""
    v = struct.pack(">H", n)
    return [0x78, 0, 0, dst, v[0], v[1], 0, 0]


TEN_SECONDS = 1758
"""10 seconds of playback clock (5625 * 10 / 32 units, rounded up)."""


def test_counters_count_playback_seconds_from_their_set():
    # set to 0, 10 s later copied out: 10
    r = run([set_counter(0, 0), mov(1, 0)], TEN_SECONDS)
    assert r.gprm[1] == 10
    assert r.ignored == 0, "a set to 0 is not recorded"


def test_a_counter_set_to_a_non_zero_value_restarts_from_0_and_is_recorded():
    r = run([set_counter(3, 25), mov(1, 3)], TEN_SECONDS)
    assert r.gprm[1] == 10, "the value 25 does not offset the counter"
    assert (r.ignored, r.ign_reg, r.ign_value) == (1, 3, 25)


def test_counters_follow_the_playback_clock_not_the_wall_clock():
    # no playback time between the commands: 0, however long the test takes
    assert run([set_counter(0, 0), mov(1, 0)], 0).gprm[1] == 0


def test_rnd_takes_its_numbers_from_the_application_source():
    # 1 + n * r / (RAND_MAX + 1): r = 2^30 (half of RAND_MAX on glibc) -> 1 + n / 2
    assert run([rnd(0, 10)], 0, lambda: 1 << 30).gprm[0] == 6
    assert run([rnd(0, 10)], 0, lambda: 0).gprm[0] == 1


def test_the_random_program_order_is_deterministic_and_a_permutation_per_cycle():
    state = ffi.new("uint32_t *")
    lib.vm_rand_seed(state, 12345)
    lib.vm_rand_next(state)
    s = state[0]
    for bound in [1, 2, 3, 4, 7, 20, 99]:
        order = [lib.vm_rand_shuffle(s, bound, k) for k in range(bound)]
        again = [lib.vm_rand_shuffle(s, bound, k) for k in range(bound)]
        assert order == again, "same state, same order"
        assert sorted(order) == list(range(bound)), f"bound {bound}: {order}"
        if bound >= 4:
            assert order[0] != 0, "with 4 or more programs a cycle does not start with program 1"
    assert lib.vm_rand_shuffle(s, 0, 0) == -1, "no programs"


def link_pgn(pg):
    """Link instruction LinkPGN (link command type 6): program pg."""
    return [0x20, 0x06, 0, 0, 0, 0, 0, pg]


def test_a_link_to_program_0_stops_without_a_broken_assumption():
    r = run([link_pgn(0)], 0)
    assert r.ret == 0, "playback stops"
    assert r.failures == 0, "not a broken assumption"
    assert r.pg_n == 1
