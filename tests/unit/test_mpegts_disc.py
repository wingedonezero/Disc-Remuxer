"""The MPEG-TS clip demuxer (src/mux/ts_disc.c, through the clip demuxer):
192-byte source packets read as the disc reader reads Blu-ray stream files:
PES packets of the PIDs asked for put together with its rules, each payload
piece given out as it arrives on the stream of its PID and PES stream key,
the PTS with the first piece that has bytes; what cannot be read dropped
until the PID's next PES start. Built packets here; the corpus check is by
hand (implementation notes)."""

import os

import pytest

from helpers import install_log, logged
from helpers.disc import env_or_skip
from helpers.parser import NOPTS, ts_read
from synth import bluray as bd

V, A = 0x1011, 0x1100


def sid(pid, key):
    return pid | key << 16


def payloads(pieces, stream):
    return b"".join(d for i, d, _, _ in pieces if i == stream)


def test_a_bounded_pes_packet_comes_out_as_it_arrives_with_its_time_on_the_first_piece():
    data = bytes(range(256)) * 2
    pes = bd.pes(0xfd, data, ext=0x71, pts=90000)
    ret, out = ts_read(bd.source_packets(bd.ts_packets(A, pes)), [A])
    assert ret == len(out) == 3
    assert [i for i, _, _, _ in out] == [sid(A, 0x71fd)] * 3
    assert payloads(out, sid(A, 0x71fd)) == data
    assert [p for _, _, p, _ in out] == [90000, NOPTS, NOPTS]
    assert [pos for _, _, _, pos in out] == [0, 192, 384]


def test_an_unbounded_video_pes_runs_to_the_next_pes_start():
    def video(pts, n):
        body = bytes([pts & 0xff]) * n
        p = bytearray(bd.pes(0xe0, body, pts=pts))
        p[4:6] = b"\0\0"                       # PES_packet_length 0
        return bytes(p), body
    p1, b1 = video(1000, 500)
    p2, b2 = video(4000, 10)
    ret, out = ts_read(bd.source_packets(bd.ts_packets(V, p1) + bd.ts_packets(V, p2)), [V])
    assert payloads(out, sid(V, 0xe0)) == b1 + b2
    assert [p for _, _, p, _ in out if p != NOPTS] == [1000, 4000]


def test_each_pes_stream_key_of_a_pid_is_its_own_stream():
    truehd, ac3 = b"T" * 100, b"A" * 50
    pk = bd.ts_packets(A, bd.pes(0xfd, truehd, ext=0x72, pts=10)) + bd.ts_packets(A, bd.pes(0xfd, ac3, ext=0x76, pts=20))
    pk += bd.ts_packets(0x1200, bd.pes(0xbd, b"P" * 30, pts=30))
    ret, out = ts_read(bd.source_packets(pk), [A, 0x1200])
    assert payloads(out, sid(A, 0x72fd)) == truehd
    assert payloads(out, sid(A, 0x76fd)) == ac3
    assert payloads(out, sid(0x1200, 0xbd)) == b"P" * 30
    ret, out = ts_read(bd.source_packets(pk), [0x1200])
    assert {i for i, _, _, _ in out} == {sid(0x1200, 0xbd)}, "only the PIDs asked for"


def test_zero_source_packets_are_skipped_and_unreadable_ts_packets_left_out():
    install_log()
    pk = bd.ts_packets(A, bd.pes(0xbd, b"x" * 300, pts=5))
    bad = bytearray(pk[1])
    bad[1] |= 0x80                             # transport error bit
    src = bd.source_packets([pk[0]]) + bytes(192) + bd.source_packets([bytes(bad)])
    ret, out = ts_read(src, [A])
    assert [len(d) for _, d, _, _ in out] == [184 - 14]
    assert logged("TS: bytes 192-383: 1 source packets of zeros: left out")
    assert logged("TS: bytes 384-575: 1 source packets that cannot be read (transport_error_indicator set): left out")


def test_a_pes_header_without_payload_bytes_gives_its_time_to_the_next_piece():
    # the first TS packet holds only the PES header (an adaptation field fills the rest)
    pes = bd.pes(0xbd, b"z" * 100, pts=777)
    first = bd.ts_packet(A, pes[:14], pusi=True)
    rest = bd.ts_packet(A, pes[14:], cc=1)
    ret, out = ts_read(bd.source_packets([first, rest]), [A])
    assert out == [(sid(A, 0xbd), b"z" * 100, 777, 192)]


def test_a_pes_start_without_a_start_code_continues_the_running_pes():
    install_log()
    p = bytearray(bd.pes(0xe0, b"v" * 400, pts=1))
    p[4:6] = b"\0\0"
    pk = bd.ts_packets(V, bytes(p))
    odd = bytearray(pk[1])
    odd[1] |= 0x40                             # a payload unit start, but the payload is no PES header
    ret, out = ts_read(bd.source_packets([pk[0], bytes(odd)] + pk[2:]), [V])
    assert payloads(out, sid(V, 0xe0)) == b"v" * 400
    assert logged("a PES start without a start code: taken as a continuation")


def test_what_cannot_be_read_is_dropped_until_the_next_pes_start():
    install_log()
    junk = bd.ts_packet(A, b"\x00\x00\x02" + bytes(181), pusi=True)        # no start code
    good = bd.ts_packets(A, bd.pes(0xbd, b"g" * 20, pts=3), cc=1)
    ret, out = ts_read(bd.source_packets([junk] + good), [A])
    assert out == [(sid(A, 0xbd), b"g" * 20, 3, 192)]
    assert logged("PID 0x1100: a PES packet without its start code: its data is dropped until its next PES start")
    assert logged("TS: PID 0x1100: a PES packet without its start code at byte 0: 184 bytes left out until the next "
                  "PES start")


def test_small_pes_packets_in_one_ts_packet_all_come_out():
    two = bd.pes(0xbd, b"a" * 10, pts=1) + bd.pes(0xbd, b"b" * 12, pts=2)
    ret, out = ts_read(bd.source_packets([bd.ts_packet(A, two, pusi=True)]), [A])
    assert [(d, p) for _, d, p, _ in out] == [(b"a" * 10, 1), (b"b" * 12, 2)]


def test_a_pes_packet_cut_short_by_the_next_pes_start():
    install_log()
    long = bd.ts_packets(A, bd.pes(0xbd, b"c" * 400, pts=1))
    nxt = bd.ts_packets(A, bd.pes(0xbd, b"d" * 10, pts=2), cc=5)
    ret, out = ts_read(bd.source_packets(long[:2] + nxt), [A])
    assert [p for _, _, p, _ in out if p != NOPTS] == [1, 2]
    assert payloads(out, sid(A, 0xbd)).endswith(b"d" * 10)
    assert logged("PID 0x1100: a PES packet cut short")


def test_a_pes_header_whose_pts_markers_are_not_set_gives_no_time_and_says_so():
    install_log()
    p = bytearray(bd.pes(0xbd, b"m" * 50, pts=12345))
    p[9] &= 0xfe                               # the PTS's first marker bit
    ret, out = ts_read(bd.source_packets(bd.ts_packets(A, bytes(p))), [A])
    assert [(d, t) for _, d, t, _ in out] == [(b"m" * 50, NOPTS)]
    assert logged("TS: PID 0x1100: a PES header at byte 0 has a PTS whose marker bits are not set: read as having "
                  "no time")


def test_continuity_counter_jumps_are_said_and_their_data_kept():
    install_log()
    data = bytes(range(256)) * 4
    pk = bd.ts_packets(A, bd.pes(0xfd, data, ext=0x71, pts=90000))      # counters 0..5
    ret, out = ts_read(bd.source_packets(pk[:2] + pk[4:], renumber=False), [A])
    got = payloads(out, sid(A, 0x71fd))
    assert got == data[:184 * 2 - 17] + data[184 * 4 - 17:], "the packets that came are kept"
    assert logged("TS: PID 0x1100: the continuity counter goes from 1 to 4 at byte 384")


def test_a_duplicate_packet_is_left_out():
    install_log()
    data = bytes(range(256)) * 4
    pk = bd.ts_packets(A, bd.pes(0xfd, data, ext=0x71, pts=90000))
    ret, out = ts_read(bd.source_packets(pk[:2] + [pk[1]] + pk[2:], renumber=False), [A])
    assert payloads(out, sid(A, 0x71fd)) == data
    assert logged("TS: PID 0x1100: a duplicate transport packet at byte 384: left out")
    assert not logged("continuity counter")


def test_the_same_counter_with_other_bytes_is_said_and_kept():
    install_log()
    a = bd.ts_packet(A, bd.pes(0xbd, b"a" * 100, pts=1), pusi=True, cc=3)
    b = bd.ts_packet(A, bd.pes(0xbd, b"b" * 100, pts=2), pusi=True, cc=3)
    ret, out = ts_read(bd.source_packets([a, b], renumber=False), [A])
    assert [d for _, d, _, _ in out] == [b"a" * 100, b"b" * 100]
    assert logged("TS: PID 0x1100: the continuity counter goes from 3 to 3 at byte 192")


def test_a_discontinuity_indicator_or_a_packet_without_payload_is_no_jump():
    install_log()
    a = bd.ts_packet(A, bd.pes(0xbd, b"a" * 100, pts=1), pusi=True, cc=3)
    b = bytearray(bd.ts_packet(A, bd.pes(0xbd, b"b" * 100, pts=2), pusi=True, cc=9))
    b[5] |= 0x80                               # discontinuity_indicator
    c = bytearray(bd.ts_packet(A, b"", cc=7))  # adaptation field only: no payload, its counter not checked
    c[3] = 0x20 | 7
    d = bd.ts_packet(A, bd.pes(0xbd, b"d" * 100, pts=3), pusi=True, cc=10)
    ret, out = ts_read(bd.source_packets([a, bytes(b), bytes(c), d], renumber=False), [A])
    assert [x for _, x, _, _ in out] == [b"a" * 100, b"b" * 100, b"d" * 100]
    assert not logged("continuity counter")


def test_a_run_of_unreadable_packets_starts_the_counters_again():
    install_log()
    pk = bd.ts_packets(A, bd.pes(0xbd, b"x" * 600, pts=5))
    ret, out = ts_read(bd.source_packets(pk[:1], renumber=False) + bytes(192) +
                       bd.source_packets(pk[2:], renumber=False), [A])
    assert logged("TS: bytes 192-383: 1 source packets of zeros: left out")
    assert not logged("continuity counter"), "the run already says the data is missing"


# ---- the corpus: an independent reading of the same stream files ----

def reference_pes(path, max_bytes, pids):
    """PES packets of pids read straight from the ISO/IEC 13818-1 layout
    (each PES packet from one payload unit start to the next): per stream
    id (PID | key << 16) the payload bytes and the PTS of the PES packets."""
    out, cur = {}, {}

    def finish(pid):
        b = cur.pop(pid, None)
        if not b or len(b) < 9 or b[:3] != b"\0\0\1":
            return
        stream, flags, hl = b[3], b[7], b[8]
        key = stream
        if (stream & 0xfe) == 0xbe:
            hl, flags = -3, 0
        o = 9 + 5 * (flags >> 7 & 1) + 5 * (flags >> 6 & 1)
        if flags & 1 and not flags & 0x3e and b[o] < 0x40 and b[o] & 1:
            r = o + (3 if b[o] & 0x10 else 1)
            if b[r] == 0x81 and b[r + 1] < 0x80:
                key = b[r + 1] << 8 | stream
        total = (b[4] << 8 | b[5]) + 6 if (b[4] or b[5]) else len(b)
        pts = None
        if flags & 0x80 and b[9] & 1 and b[11] & 1 and b[13] & 1:
            pts = ((b[9] >> 1) & 7) << 30 | b[10] << 22 | (b[11] >> 1) << 15 | b[12] << 7 | b[13] >> 1
        e = out.setdefault(pid | key << 16, [bytearray(), []])
        e[0] += b[hl + 9:total]
        if pts is not None:
            e[1].append(pts)

    with open(path, "rb") as f:
        data = f.read(max_bytes)
    for i in range(0, len(data) - 191, 192):
        p = data[i + 4:i + 192]
        if p[0] != 0x47 or p[1] & 0x80:
            continue
        pid = (p[1] & 0x1f) << 8 | p[2]
        if pid not in pids:
            continue
        off = 4 + (p[4] + 1 if p[3] & 0x20 else 0)
        if not p[3] & 0x10:
            continue
        if p[1] & 0x40:
            finish(pid)
            cur[pid] = bytearray()
        if pid in cur:
            cur[pid] += p[off:]
    return out


@pytest.mark.disc
def test_the_corpus_stream_files_read_as_an_independent_reader_reads_them():
    files = env_or_skip("BLURAY_STREAM_FILES", need="Blu-ray stream files (.m2ts, comma-separated)")
    for path in files.split(","):
        pids = sorted({p for p in (0x1011, 0x1015, *range(0x1100, 0x1110), *range(0x1200, 0x1220))})
        size = min(os.path.getsize(path), 64 << 20) // 192 * 192
        size -= size % (192 * 1000)                 # whole PES packets at the end are not known: cut back
        ret, out = ts_read(path=path, max_bytes=size, pids=pids)
        assert ret >= 0, f"{path}: {ret}"
        ours = {}
        for i, d, pts, _ in out:
            e = ours.setdefault(i, [bytearray(), []])
            e[0] += d
            if pts != NOPTS:
                e[1].append(pts)
        ref = reference_pes(path, size, set(pids))
        assert set(ours) == set(ref), path
        for k in ref:
            # the last PES packet of each stream may still be open at the cut: compare up to its start
            n = min(len(ours[k][0]), len(ref[k][0]))
            assert ours[k][0][:n] == ref[k][0][:n], f"{path} stream {k:#x}"
            assert ours[k][1][:len(ref[k][1])] == ref[k][1] or ours[k][1] == ref[k][1][:len(ours[k][1])], \
                f"{path} stream {k:#x}: times"
            assert abs(len(ours[k][0]) - len(ref[k][0])) < 1 << 20, f"{path} stream {k:#x}: lengths"
