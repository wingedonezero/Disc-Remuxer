"""Subtitles tied to their pictures for mkvmerge (src/disc-remuxer/subtie.c):
mkvmerge's picture lengths for a stream file without times (the rule from its
code; the numbers measured against the MKV files mkvmerge 102 / 103 made from
real discs' stream files, every picture), each subtitle's picture and fields on
that clock, and the tied copies of a .sup and a VobSub index."""

import struct

import pytest

from helpers import ffi, lib

T = lib.DR_TICKS_PER_SECOND
NTSC = (30000, 1001)
FILM = (24000, 1001)
FIELD = T * 1001 // 60000          # one field at 29.97, in ticks (18,018,000)


def test_the_lengths_mkvmerge_gives_the_pictures_measured_on_real_discs():
    c = lib
    assert c.subtie_picture_ns(c.DR_CODEC_H264, *FILM, 0, 2) == 41708332, "H.264 23.976: twice the cut field"
    assert c.subtie_picture_ns(c.DR_CODEC_H264, *NTSC, 0, 2) == 33366666, "H.264 29.97 frame pictures"
    assert c.subtie_picture_ns(c.DR_CODEC_H264, *NTSC, 1, 1) == 16683333, "H.264 field pictures"
    assert c.subtie_picture_ns(c.DR_CODEC_HEVC, *FILM, 0, 2) == 41708333, "HEVC 23.976: the cut frame"
    assert c.subtie_picture_ns(c.DR_CODEC_MPEG2VIDEO, *NTSC, 0, 2) == 33366666
    assert c.subtie_picture_ns(c.DR_CODEC_MPEG2VIDEO, *NTSC, 0, 3) == 50050000, "repeat_first_field: 3 fields cut"


def test_the_same_rule_at_the_other_rates():
    c = lib
    assert c.subtie_picture_ns(c.DR_CODEC_MPEG2VIDEO, 25, 1, 0, 2) == 40000000
    assert c.subtie_picture_ns(c.DR_CODEC_MPEG2VIDEO, *FILM, 0, 4) == 83416666, "a progressive frame repeated"
    assert c.subtie_picture_ns(c.DR_CODEC_H264, 24, 1, 0, 2) == 41666666
    assert c.subtie_picture_ns(c.DR_CODEC_H264, 60000, 1001, 0, 2) == 16683332
    assert c.subtie_picture_ns(c.DR_CODEC_HEVC, 50, 1, 0, 2) == 20000000


def test_pictures_mkvmerge_does_not_count_as_the_disc_are_refused():
    c = lib
    assert c.subtie_picture_ns(c.DR_CODEC_VC1, *NTSC, 0, 2) == -1, "VC-1: not covered"
    assert c.subtie_picture_ns(c.DR_CODEC_H264, *NTSC, 0, 3) == -1, "pic_struct 3 fields: mkvmerge counts 2"
    assert c.subtie_picture_ns(c.DR_CODEC_HEVC, *FILM, 0, 3) == -1
    assert c.subtie_picture_ns(c.DR_CODEC_MPEG2VIDEO, *NTSC, 1, 1) == -1, "a field pair is one MPEG-2 picture"
    assert c.subtie_picture_ns(c.DR_CODEC_MPEG2VIDEO, *NTSC, 0, 5) == -1


def clock(codec, rate, frames):
    """frames: (time, duration, pictures) in ticks; returns the SubTie (freed by the test's end)."""
    s = ffi.gc(lib.subtie_alloc(), lambda p: lib.subtie_free(ffi.new("SubTie **", p)))
    for f in frames:
        assert lib.subtie_video_frame(s, *f) == 0
    why = ffi.new("char[256]")
    r = lib.subtie_clock(s, codec, *rate, why, 256)
    return s, r, ffi.string(why).decode()


def place(s, t):
    ms = ffi.new("int64_t *", -7)
    j = lib.subtie_place(s, t, ms)
    return j, ms[0]


def test_a_subtitle_on_a_half_ms_picture_takes_the_picture_s_ms_on_mkvmerge_s_clock():
    # 29.97 frame pictures: picture 225 is at exactly 7,507.5 ms; mkvmerge counts it 225 x 33,366,666 ns =
    # 7,507.49985 ms -> 7,507, where the disc's exact time would round to 7,508
    s, r, why = clock(lib.DR_CODEC_MPEG2VIDEO, NTSC, [(k * 2 * FIELD, 2 * FIELD, 1) for k in range(300)])
    assert r == 1, why
    assert place(s, 225 * 2 * FIELD) == (0, 7507)
    assert place(s, 0) == (0, 0)
    assert place(s, 299 * 2 * FIELD) == (0, (299 * 33366666 + 500000) // 1000000)


def test_soft_telecine_pictures_and_subtitles_on_their_later_fields():
    # 2-3 pulldown: pictures of 2 and 3 fields; a subtitle on a picture's 2nd field (or a 3-field picture's 3rd)
    # gets the picture's time plus those fields on the same clock
    pattern = [2, 3] * 50
    frames, t = [], 0
    for f in pattern:
        frames.append((t, f * FIELD, 1))
        t += f * FIELD
    s, r, why = clock(lib.DR_CODEC_MPEG2VIDEO, NTSC, frames)
    assert r == 1, why
    starts, ns = [], 0
    for f in pattern:
        starts.append(ns)
        ns += 33366666 if f == 2 else 50050000
    k = 41                                                  # a 3-field picture
    assert pattern[k] == 3
    ms = lambda v: (v + 500000) // 1000000
    assert place(s, frames[k][0]) == (0, ms(starts[k]))
    assert place(s, frames[k][0] + FIELD) == (1, ms(starts[k] + 16683333))
    assert place(s, frames[k][0] + 2 * FIELD) == (2, ms(starts[k] + 33366666))
    assert place(s, frames[k - 1][0] + FIELD) == (1, ms(starts[k - 1] + 16683333)), "a 2-field picture's 2nd field"
    assert place(s, frames[k - 1][0] + 2 * FIELD)[0] == 0, "past a 2-field picture's fields: the next picture"


def test_a_subtitle_off_the_fields_or_outside_the_pictures_is_not_placed():
    s, r, _ = clock(lib.DR_CODEC_MPEG2VIDEO, NTSC, [(T + k * 2 * FIELD, 2 * FIELD, 1) for k in range(10)])
    assert r == 1
    assert place(s, T + FIELD // 2) == (-1, -7), "between two fields"
    assert place(s, T - FIELD) == (-1, -7), "before the first picture"
    assert place(s, T + 20 * FIELD) == (-1, -7), "after the last picture's end"
    assert place(s, T + 2 * FIELD + T // 90000) == (0, 33), "within one 90 kHz tick of a picture"


def test_h264_field_pictures_are_one_field_each():
    s, r, why = clock(lib.DR_CODEC_H264, NTSC, [(k * 2 * FIELD, 2 * FIELD, 2) for k in range(50)])
    assert r == 1, why
    assert place(s, 7 * FIELD) == (0, (7 * 16683333 + 500000) // 1000000)


def test_videos_mkvmerge_does_not_count_as_the_disc_are_not_tied():
    s, r, why = clock(lib.DR_CODEC_VC1, NTSC, [(k * 2 * FIELD, 2 * FIELD, 1) for k in range(5)])
    assert r == 0 and "VC-1" in why
    s, r, why = clock(lib.DR_CODEC_H264, NTSC, [(0, 2 * FIELD, 1), (2 * FIELD, 3 * FIELD, 1), (5 * FIELD, 2 * FIELD, 1)])
    assert r == 0 and "3 fields" in why, why
    s, r, why = clock(lib.DR_CODEC_MPEG2VIDEO, NTSC, [(0, 2 * FIELD, 1), (2 * FIELD + 5, 2 * FIELD, 1)])
    assert r == 0 and "not a whole number of fields" in why, why
    assert place(s, 0) == (-1, -7), "no clock, nothing placed"


def test_a_sup_time_gives_mkvmerge_s_ms():
    assert lib.subtie_sup_ms(675675) == 7508, "7,507.5 ms: halves up"
    assert lib.subtie_sup_ms(675630) == 7507
    assert lib.subtie_sup_ms(2**32 - 1) == ((2**32 - 1) * 100000 // 9 + 500000) // 1000000


def sup_segment(kind, body, pts):
    return b"PG" + struct.pack(">II", pts, 0) + bytes([kind]) + struct.pack(">H", len(body)) + body


def test_the_tied_sup_changes_only_each_set_s_times(tmp_path):
    sets = [[sup_segment(0x16, b"\1\2\3", 900), sup_segment(0x80, b"", 900)],
            [sup_segment(0x16, b"\4" * 40, 1800), sup_segment(0x14, b"\5" * 9, 1800), sup_segment(0x80, b"", 1800)],
            [sup_segment(0x16, b"\6", 2700), sup_segment(0x80, b"", 2700)]]
    data = [b"".join(s) for s in sets]
    src, out = tmp_path / "a.sup", tmp_path / "a.tied.sup"
    src.write_bytes(b"".join(data))
    pos = [0, len(data[0]), len(data[0]) + len(data[1])]
    assert lib.subtie_sup(str(src).encode(), str(out).encode(), pos, [len(d) for d in data], [9, -1, 31], 3) == 0
    got = out.read_bytes()
    assert len(got) == src.stat().st_size
    segs, o = [], 0
    while o < len(got):
        n = struct.unpack(">H", got[o + 11:o + 13])[0]
        segs.append((struct.unpack(">I", got[o + 2:o + 6])[0], got[o + 6:o + 13 + n]))
        o += 13 + n
    assert [t for t, _ in segs] == [810, 810, 1800, 1800, 1800, 2790, 2790], "ms x 90 per set; -1 keeps the time"
    orig, o = [], 0
    raw = src.read_bytes()
    while o < len(raw):
        n = struct.unpack(">H", raw[o + 11:o + 13])[0]
        orig.append(raw[o + 6:o + 13 + n])
        o += 13 + n
    assert [b for _, b in segs] == orig, "everything but the time byte for byte"
    # the sets must cover the file: a missing set is an error and no copy is left
    assert lib.subtie_sup(str(src).encode(), str(out).encode(), pos[:2], [len(d) for d in data[:2]], [9, 20], 2) < 0
    assert not out.exists()


def test_the_tied_index_changes_only_the_timestamp_lines(tmp_path):
    head = "# VobSub index file, v7 (do not modify this line!)\nsize: 720x480\n\n# eng\nid: eng, index: 0\n"
    lines = ["timestamp: 00:00:07:508, filepos: 000000000\n", "timestamp: 01:02:03:004, filepos: 000000800\n"]
    src, out = tmp_path / "a.idx", tmp_path / "a.tied.idx"
    src.write_text(head + "".join(lines))
    assert lib.subtie_idx(str(src).encode(), str(out).encode(), [7507, -1], 2) == 0
    assert out.read_text() == head + "timestamp: 00:00:07:507, filepos: 000000000\n" + lines[1]
    assert lib.subtie_idx(str(src).encode(), str(out).encode(), [1], 1) < 0, "one line per unit, no more"
    assert not out.exists()


def test_the_sub_next_to_a_tied_index_is_the_stream_file_s(tmp_path):
    src, out = tmp_path / "a.sub", tmp_path / "a.tied.sub"
    src.write_bytes(b"\0\0\1\xba" * 512)
    assert lib.subtie_link_or_copy(str(src).encode(), str(out).encode()) == 0
    assert out.read_bytes() == src.read_bytes()
