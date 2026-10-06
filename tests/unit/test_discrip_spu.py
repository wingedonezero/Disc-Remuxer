"""libavformat/discrip_spu.c: sub-picture units (DVD-Video, and the HD DVD
form with 32-bit sizes) cut from the byte stream, each lasting until its
stop command (STP_DSP delay x 1024 / 90000 s; one step when it has none),
timed at the video grid point nearest to its PES time. The video grid comes
from the MPEG-2 test stream (tests/data/README.md) with every picture on its
grid; the units are built byte by byte."""

import dataclasses
import struct

from helpers import AV_NOPTS_VALUE, DATA, ffi, lib
from helpers.discrip import Handles, codec_id, events_into, fn, pictures

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
FRAME = 36036000  # 1001/30000 s
FIELD = FRAME // 2
BASE = 10 * 1080000000  # the first displayed picture at 10 s
STEP = 12288000  # one SP_DCSQ_STM step: 1024 / 90000 s
MS = 1080000
F_KEY, F_NO_STOP = lib.DR_F_KEY, lib.DR_F_NO_STOP


# ---- units ----

def spu(stop, dspxa, cmds, mark):
    """A DVD-Video sub-picture unit: SPDSZ, SP_DCSQTA, 4 bytes of pixel data,
    SP_DCSQ 1 (delay 0: STA_DSP, cmds, SET_DSPXA when dspxa), SP_DCSQ 2
    (delay stop: STP_DSP) when there is a stop. mark makes units differ."""
    u = bytearray([0, 0, 0, 0, mark, 0x22, 0x33, 0x44])
    d1 = len(u)
    seq = bytearray([0x01]) + bytes(cmds)
    if dspxa:
        seq += bytes([0x06, 0, 4, 0, 6])
    seq.append(0xFF)
    d2 = d1 + 4 + len(seq)
    u += bytes([0, 0]) + struct.pack(">H", d2 if stop is not None else d1) + seq
    if stop is not None:
        u += struct.pack(">H", stop) + struct.pack(">H", d2) + bytes([0x02, 0xFF])
    u[0:2] = struct.pack(">H", len(u))
    u[2:4] = struct.pack(">H", d1)
    return bytes(u)


def spu_hd(stop):
    """An HD DVD sub-picture unit: a zero word, 32-bit size and SP_DCSQTA, 4
    bytes of pixel data, SP_DCSQ 1 with the 8-bit commands (SET_COLOR 0x83,
    SET_CONTR 0x84, SET_DAREA 0x85, SET_DSPXA 0x86), SP_DCSQ 2 with STP_DSP."""
    u = bytearray(10) + bytes([0x11, 0x22, 0x33, 0x44])
    d1 = len(u)
    seq = bytearray([0x01, 0x83]) + b"\x10" * 768 + b"\x84" + b"\x0f" * 256
    seq += bytes([0x85, 0, 0, 0, 0, 0, 0]) + bytes([0x86, 0, 0, 0, 10, 0, 0, 0, 12, 0xFF])
    d2 = d1 + 6 + len(seq)
    u += bytes([0, 0]) + struct.pack(">I", d2) + seq
    u += struct.pack(">H", stop) + struct.pack(">I", d2) + bytes([0x02, 0xFF])
    u[2:6] = struct.pack(">I", len(u))
    u[6:10] = struct.pack(">I", d1)
    return bytes(u)


# ---- the run ----

@dataclasses.dataclass
class Run:
    out: list  # bytes, time, dur, flags
    events: list
    st: object
    cut: object
    snap_before_video: int


def run(payloads, subs_first):
    """The video (every picture on its grid from BASE) and the sub-picture
    payloads (bytes, time) of one segment; the payloads go in before the
    video when subs_first."""
    vid = codec_id("mpeg2video")
    sid = codec_id("dvd_subtitle")
    out, events = [], []

    def on_frame(f):
        out.append((bytes(ffi.buffer(f.data, f.size)), f.time, f.dur, f.flags))
        lib.ff_discrip_frame_unref(f)
        return 0

    def drop(f):
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    vc = ffi.new("DRCutter **")
    s = ffi.new("DRSpu **")
    sc = ffi.new("DRCutter **")
    st = ffi.new("DRSpuStats *")
    cut = ffi.new("DRCutterStats *")
    t = ffi.new("int64_t *")
    assert lib.ff_discrip_video_open(v, ffi.NULL, vid, 0, lib.tb_py_frame, keep(drop), ffi.NULL, ffi.NULL) == 0
    assert lib.ff_discrip_cutter_open(vc, ffi.NULL, vid, fn("ff_discrip_video_unit"), v[0]) == 0
    assert lib.ff_discrip_spu_open(s, ffi.NULL, 1, v[0], lib.tb_py_frame, keep(on_frame), lib.tb_py_event,
                                   keep(events_into(events))) == 0
    assert lib.ff_discrip_cutter_open(sc, ffi.NULL, sid, fn("ff_discrip_spu_unit"), s[0]) == 0
    snap = lib.ff_discrip_video_snap(v[0], BASE, t)

    def subs():
        for data, time in payloads:
            assert lib.ff_discrip_cutter_write(sc[0], data, len(data), AV_NOPTS_VALUE if time is None else time) == 0
    if subs_first:
        subs()
        assert out == [], "units wait for the video's grid"
    for a, b, disp, _ in pictures(M2V):
        assert lib.ff_discrip_cutter_write(vc[0], M2V[a:b], b - a, BASE + disp * FRAME) == 0
    if not subs_first:
        subs()
    assert lib.ff_discrip_cutter_flush(vc[0]) == 0
    assert lib.ff_discrip_video_flush(v[0]) == 0
    assert lib.ff_discrip_cutter_flush(sc[0]) == 0
    assert lib.ff_discrip_spu_flush(s[0]) == 0
    lib.ff_discrip_spu_stats(s[0], st)
    lib.ff_discrip_cutter_stats(sc[0], cut)
    lib.ff_discrip_cutter_close(sc)
    lib.ff_discrip_spu_close(s)
    lib.ff_discrip_cutter_close(vc)
    lib.ff_discrip_video_close(v)
    return Run(out, events, st, cut, snap)


def grid(frames, fields):
    """Times on the grid: display position frames + fields."""
    return BASE + frames * FRAME + fields * FIELD


# ---- tests ----

def test_units_are_cut_from_the_byte_stream_and_keep_their_bytes():
    # unit 2 starts in the first payload (whose time unit 1 took) and ends in
    # the second; unit 3 starts in the second payload and takes its time
    u1, u2, u3 = spu(100, True, [], 1), spu(50, True, [], 2), spu(70, True, [], 3)
    stream = u1 + u2 + u3
    cut = len(u1) + len(u2) // 2
    r = run([(stream[:cut], grid(10, 0)), (stream[cut:], grid(20, 0))], False)
    assert [(o[0], o[1]) for o in r.out] == [(u1, grid(10, 0)), (u3, grid(20, 0))]
    # unit 2 has no time of its own: left out (as the reference does)
    assert r.st.untimed == 1
    assert len(r.events) == 1
    assert r.events[0]["kind"] == lib.DR_EV_SUB_UNTIMED
    assert r.events[0]["pos"] == len(u1), "its byte offset"
    assert (r.st.units, r.st.out) == (3, 2)


def test_a_unit_over_several_payloads_takes_the_time_of_the_one_it_starts_in():
    u = spu(100, True, [], 1)
    r = run([(u[:5], grid(3, 0)), (u[5:], None)], False)
    assert len(r.out) == 1
    assert (r.out[0][0], r.out[0][1]) == (u, grid(3, 0))


def test_a_unit_lasts_until_its_stop_command():
    r = run([(spu(100, True, [], 1), grid(1, 0)), (spu(None, True, [], 2), grid(2, 0)),
             (spu(None, False, [], 3), grid(3, 0))], False)
    assert [(o[2], o[3]) for o in r.out] == [
        (100 * STEP, F_KEY),
        # shows a picture without a stop time: one delay step, marked
        (STEP, F_KEY | F_NO_STOP),
        # no picture and no stop: one delay step
        (STEP, F_KEY),
    ]
    assert r.st.no_stop == 1


def test_hd_dvd_units_with_32_bit_sizes():
    u1, u2 = spu_hd(250), spu_hd(30)
    stream = u1 + u2
    k = len(u1) - 3  # unit 2 starts in the second payload
    r = run([(stream[:k], grid(4, 0)), (stream[k:], grid(8, 0))], False)
    assert [(o[0], o[1], o[2]) for o in r.out] == [(u1, grid(4, 0), 250 * STEP), (u2, grid(8, 0), 30 * STEP)]


def test_a_unit_whose_commands_do_not_parse_is_left_out():
    # 0x55 is no display control command
    r = run([(spu(10, True, [], 1), grid(1, 0)), (spu(10, True, [0x55], 2), grid(2, 0)),
             (spu(10, True, [], 3), grid(3, 0))], False)
    assert [o[0][4] for o in r.out] == [1, 3]
    assert r.cut.skipped == 1


def test_bytes_where_no_unit_starts_are_given_up():
    # a header stating a size below 9: the bytes kept so far are dropped and
    # the next unit is found at the start of the next payload
    junk = bytes([0x00, 0x04, 0x00, 0x02, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80])
    u = spu(10, True, [], 7)
    r = run([(junk, grid(1, 0)), (u, grid(2, 0))], False)
    assert len(r.out) == 1
    assert (r.out[0][0], r.out[0][1]) == (u, grid(2, 0))
    assert (r.cut.skipped, r.cut.skipped_bytes) == (1, 12)


def test_times_go_to_the_nearest_video_field():
    r = run([
        (spu(10, True, [], 1), grid(5, 0) + 3 * MS),   # 3 ms late -> the field before
        (spu(10, True, [], 2), grid(7, 0) + 12 * MS),  # 12 ms late -> the next field
        (spu(10, True, [], 3), grid(9, 1)),            # on a field: unchanged
    ], False)
    assert [o[1] for o in r.out] == [grid(5, 0), grid(7, 1), grid(9, 1)]
    assert r.st.max_shift == FIELD - 12 * MS


def test_units_before_the_video_start():
    # 0.05 ms before the first field: on it; 1 ms before: left out
    r = run([(spu(10, True, [], 1), BASE - 54000), (spu(10, True, [], 2), BASE - MS),
             (spu(10, True, [], 3), grid(1, 0))], False)
    assert [(o[0][4], o[1]) for o in r.out] == [(1, BASE), (3, grid(1, 0))]
    assert r.st.early == 1
    assert len(r.events) == 1
    assert (r.events[0]["kind"], r.events[0]["pos"]) == (lib.DR_EV_SUB_EARLY, BASE - MS)


def test_units_wait_until_the_video_knows_its_grid():
    payloads = [(spu(10, True, [], 1), grid(2, 0) + MS), (spu(10, True, [], 2), grid(6, 1))]
    r = run(payloads, True)
    assert r.snap_before_video < 0, "no grid before the video"
    t = [o[1] for o in r.out]
    assert t == [grid(2, 0), grid(6, 1)]
    assert t == [o[1] for o in run(payloads, False).out]


def test_colour_changes_and_forced_starts_are_counted():
    # CHG_COLCON with its 16-bit size: the command takes size + 3 bytes
    colcon = [0x07, 0x00, 0x02, 0xAA, 0xBB]
    r = run([(spu(10, True, colcon, 1), grid(1, 0)), (spu(10, True, [0x00], 2), grid(2, 0))], False)
    assert len(r.out) == 2
    assert (r.st.colcon, r.st.forced) == (1, 1)
