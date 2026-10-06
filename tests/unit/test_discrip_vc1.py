"""VC-1 in the rip core (discrip_vc1.c, SMPTE 421M Advanced Profile): the
sequence header gives the frame rate and the header flags; the frame header
gives the picture type (PTYPE; for field pairs the 3-bit FPTYPE, Table 105)
and the fields (RFF / RPTFRM); display order is counted (B pictures after a
reference picture first, I pictures restart); sequence header + entry point
make a key frame. The core's own cutter makes a unit of one frame with the
headers before it and the user data after it; an end of sequence stays only
after a second field. Streams built bit by bit here."""

from helpers import AV_NOPTS_VALUE, ffi, lib
from helpers.disc import env_or_skip
from helpers.discrip import Handles, codec_id, events_into, fn

FIELD = 18018000  # 1001/60000 s
BASE = 5 * 1080000000
F_KEY, F_DISCARD = lib.DR_F_KEY, lib.DR_F_DISCARD
INVALID = -0x41444E49  # AVERROR_INVALIDDATA


class BitW:
    def __init__(self):
        self.out = bytearray()
        self.cur = 0
        self.n = 0

    def put(self, v, bits):
        for i in range(bits - 1, -1, -1):
            self.cur = ((self.cur << 1) | ((v >> i) & 1)) & 0xff
            self.n += 1
            if self.n == 8:
                self.out.append(self.cur)
                self.cur = 0
                self.n = 0

    def finish(self):
        while self.n != 0:
            self.put(1, 1)
        return bytes(self.out)


def seq(interlace, pulldown):
    b = BitW()
    b.put(3, 2)  # profile: advanced
    b.put(3, 3)  # level
    b.put(0b01000000000, 11)  # colordiff 1, postproc fields 0
    b.put(959, 12)
    b.put(539, 12)
    b.put(int(pulldown), 1)
    b.put(int(interlace), 1)
    b.put(0, 1)  # tfcntrflag
    b.put(0, 2)  # finterpflag, reserved
    b.put(0, 1)  # psf
    b.put(1, 1)  # display extension
    b.put(1919, 14)
    b.put(1079, 14)
    b.put(0, 1)  # aspect ratio flag
    b.put(1, 1)  # frame rate flag
    b.put(0, 1)  # frameratind
    b.put(3, 8)  # 30000
    b.put(2, 4)  # /1001
    b.put(0, 1)  # color format flag
    b.put(0, 1)  # hrd param flag
    return bytes([0, 0, 1, 0x0F]) + b.finish() + bytes([0, 0, 1, 0x0E, 0x88, 0x88, 0x80])  # + entry point


def frame(bits, field2):
    """A frame: the frame-header bits, then filler; field2: a second field unit."""
    b = BitW()
    for v, n in bits:
        b.put(v, n)
    out = bytes([0, 0, 1, 0x0D]) + b.finish() + b"\x55" * 200
    if field2:
        out += bytes([0, 0, 1, 0x0C]) + b"\x55" * 150
    return out


class Sink:
    def __init__(self):
        self.out = []
        self.events = []


def run(units):
    """Units (bytes, PES time) through the cutter and the video timing."""
    cid = codec_id("vc1")
    sink = Sink()

    def on_frame(f):
        sink.out.append((f.time, f.dur, f.flags))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    c = ffi.new("DRCutter **")
    st = ffi.new("DRVideoStats *")
    assert lib.ff_discrip_video_open(v, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame), lib.tb_py_event,
                                     keep(events_into(sink.events))) == 0
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_video_unit"), v[0]) == 0
    for data, t in units:
        assert lib.ff_discrip_cutter_write(c[0], data, len(data), t) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    assert lib.ff_discrip_video_flush(v[0]) == 0
    lib.ff_discrip_video_stats(v[0], st)
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_video_close(v)
    return sink, st


# Progressive PTYPE codes.
P = (0, 1)
B = (0b10, 2)
I = (0b110, 3)  # noqa: E741
BI = (0b1110, 4)


def test_progressive_b_pictures_are_counted_into_display_order():
    # decode order I P B B P B BI, GOPs of 7; display I B B P B BI P
    pattern = [(I, 0), (P, 3), (B, 1), (B, 2), (P, 6), (B, 4), (BI, 5)]
    units = []
    for g in range(40):
        for k, (pt, disp) in enumerate(pattern):
            u = (seq(False, False) if k == 0 else b"") + frame([pt], False)
            units.append((u, BASE + (g * 7 + disp) * 2 * FIELD))
    s, st = run(units)
    assert (st.num, st.den) == (30000, 1001)
    assert len(s.out) == len(units)
    for o, u in zip(s.out, units):
        assert o[0] == u[1], "grid time = PES time"
        assert o[1] == 2 * FIELD
    assert s.events == [], s.events
    assert s.out[0][2] & F_KEY == F_KEY
    assert s.out[2][2] & F_DISCARD == F_DISCARD
    assert s.out[6][2] & F_DISCARD == F_DISCARD, "BI pictures are B pictures"


def test_field_pairs_take_their_type_from_fptype():
    # field-interlaced frames (FCM 11): the GOP starts with an I/P pair and a
    # BI/B pair sits where a B pair belongs -- the 300 Combo case that the
    # PTYPE reading displaces by two frames
    fcm_field = (0b11, 2)
    pattern = [(1, 0), (3, 3), (6, 1), (4, 2)]  # I/P, P/P, BI/B, B/B
    units = []
    for g in range(60):
        for k, (fpt, disp) in enumerate(pattern):
            u = (seq(True, False) if k == 0 else b"") + frame([fcm_field, (fpt, 3)], True)
            units.append((u, BASE + (g * 4 + disp) * 2 * FIELD))
    s, _ = run(units)
    assert len(s.out) == len(units)
    for o, u in zip(s.out, units):
        assert o[0] == u[1]
        assert o[1] == 2 * FIELD, "a field pair lasts two fields"
    assert not any(e["kind"] == lib.DR_EV_VIDEO_TIMECODE for e in s.events), s.events
    assert all(o[2] & F_KEY for o in s.out[::4]), "I/P pairs are key frames"
    assert all(o[2] & F_DISCARD for o in s.out[2::4]), "BI/B pairs are B pictures"


def test_repeated_fields_lengthen_frames():
    # interlaced pulldown: TFF / RFF after PTYPE (FCM 0 = progressive frame)
    # pattern of 2 and 3 fields as in 3:2 pulldown; I and P only
    units, pos = [], 0
    for k in range(200):
        rff = int(k % 2 == 1)
        pt = I if k % 12 == 0 else P
        u = (seq(True, True) if k % 12 == 0 else b"") + frame([(0, 1), pt, (1, 1), (rff, 1)], False)
        units.append((u, BASE + pos * FIELD))
        pos += 2 + rff
    s, _ = run(units)
    for k, (o, u) in enumerate(zip(s.out, units)):
        assert o[0] == u[1]
        assert o[1] == (3 * FIELD if k % 2 == 1 else 2 * FIELD)
    assert s.events == []


def test_skipped_frames_repeat_fields_too():
    # 3:2 pulldown in RFF flags where every second P frame is skipped (PTYPE
    # 1111): the skipped frame still carries TFF / RFF (no TFCNTR), as Blu-ray
    # and HD DVD streams have it; 2 + 3 fields per pair of frames
    skipped = (0b1111, 4)
    units, pos = [], 0
    for k in range(120):
        rff = int(k % 2 == 1)
        pt = I if k % 12 == 0 else (skipped if k % 4 == 3 else P)
        u = (seq(True, True) if k % 12 == 0 else b"") + frame([(0, 1), pt, (1, 1), (rff, 1)], False)
        units.append((u, BASE + pos * FIELD))
        pos += 2 + rff
    s, _ = run(units)
    assert len(s.out) == len(units)
    for k, (o, u) in enumerate(zip(s.out, units)):
        assert o[0] == u[1], f"frame {k}"
        assert o[1] == (3 * FIELD if k % 2 == 1 else 2 * FIELD), f"frame {k}"
    assert s.events == [], s.events


def frames_of(stream, chunk):
    """A stream through the cutter (fed chunk bytes at a time, no PES times)
    and the video stage: the frames' bytes in decode order, or the first
    error the cutter returned."""
    cid = codec_id("vc1")
    out = []

    def on_bytes(f):
        out.append(bytes(ffi.buffer(f.data, f.size)))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    c = ffi.new("DRCutter **")
    assert lib.ff_discrip_video_open(v, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_bytes), ffi.NULL, ffi.NULL) == 0
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_video_unit"), v[0]) == 0
    err = 0
    for i in range(0, len(stream), chunk):
        part = stream[i:i + chunk]
        err = lib.ff_discrip_cutter_write(c[0], part, len(part), AV_NOPTS_VALUE)
        if err < 0:
            break
    if err == 0:
        err = lib.ff_discrip_cutter_flush(c[0])
    if err == 0:
        assert lib.ff_discrip_video_flush(v[0]) == 0
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_video_close(v)
    return err if err < 0 else out


def frames(stream):
    """The same frames whatever the payload sizes."""
    every = frames_of(stream, 2048)
    for chunk in [1, 3, 7]:
        assert frames_of(stream, chunk) == every, f"payloads of {chunk} bytes"
    return every


def bdu(code):
    return bytes([0, 0, 1, code, 0x4A, 0x55, 0x80])


END_OF_SEQUENCE = bytes([0, 0, 1, 0x0A])


def test_user_data_after_a_frame_header_stays_with_its_frame():
    # SMPTE 421M Annex E: frame-level (0x1D) and field-level (0x1C) user data
    # follow the frame header they belong to; FFmpeg's parser starts the next
    # unit there
    a = seq(False, False) + frame([I], False) + bdu(0x1D)
    b = frame([P], False) + bdu(0x1C) + bdu(0x1D)
    c = seq(False, False) + frame([I], False)
    assert frames(a + b + c) == [a, b, c]


def test_an_end_of_sequence_without_a_second_field_is_left_out():
    a = seq(False, False) + frame([I], False)
    b = frame([P], False) + bdu(0x1D)
    c = seq(False, False) + frame([I], False)
    stream = a + END_OF_SEQUENCE + b + END_OF_SEQUENCE + END_OF_SEQUENCE + c + END_OF_SEQUENCE
    assert frames(stream) == [a, b, c]


def test_a_second_field_keeps_its_user_data_and_the_end_of_sequence_after_it():
    fcm_field = (0b11, 2)
    first = frame([fcm_field, (1, 3)], False)  # I/P field pair, first field
    first += bdu(0x1C) + bytes([0, 0, 1, 0x0C]) + b"\x55" * 150 + bdu(0x1C) + END_OF_SEQUENCE + END_OF_SEQUENCE
    a = seq(True, False) + first
    b = seq(True, False) + frame([fcm_field, (1, 3)], True)
    assert frames(a + b) == [a, b]


def test_headers_without_a_frame_are_left_out():
    # a sequence header + entry point followed by another sequence header,
    # or by an end of sequence: those headers belong to no frame
    a = seq(False, False) + frame([I], False)
    stream = seq(False, False) + a + seq(False, False) + END_OF_SEQUENCE + a
    assert frames(stream) == [a, a]


def test_start_codes_where_none_can_be_stop_the_track():
    good = seq(False, False) + frame([I], False)
    # a slice, slice user data, a reserved code
    for code in [0x0B, 0x1B, 0x10]:
        assert frames_of(good + bdu(code) + good, 2048) == INVALID, f"start code 0x{code:02X}"
    # a field after the second field: a field without its frame
    assert frames_of(good + bdu(0x0C) + bdu(0x0C) + good, 2048) == INVALID
    # an entry point after the entry-point user data
    seq_only = seq(False, False)[:-7]
    assert frames_of(seq_only + bdu(0x0E) + bdu(0x1E) + bdu(0x0E) + frame([I], False), 2048) == INVALID
    # frame user data before any frame
    assert frames_of(bdu(0x1D) + good, 2048) == INVALID


def test_print_units_of_a_file():
    """With DISCRIP_VC1_FILE (a raw VC-1 elementary stream): every unit our
    rules read, as `pos key discard fields` lines on stdout (pytest -s), to
    compare with FFmpeg's decoder (ffprobe -show_frames)."""
    path = env_or_skip("DISCRIP_VC1_FILE", need="a VC-1 elementary stream")
    data = open(path, "rb").read()
    cid = codec_id("vc1")
    units = []

    def on_frame(f):
        units.append((f.pos, f.flags, f.dur))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    v = ffi.new("DRVideo **")
    c = ffi.new("DRCutter **")
    assert lib.ff_discrip_video_open(v, ffi.NULL, cid, 0, lib.tb_py_frame, keep(on_frame), ffi.NULL, ffi.NULL) == 0
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_video_unit"), v[0]) == 0
    for i in range(0, len(data), 2048):
        assert lib.ff_discrip_cutter_write(c[0], data[i:i + 2048], len(data[i:i + 2048]), AV_NOPTS_VALUE) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    assert lib.ff_discrip_video_flush(v[0]) == 0
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_video_close(v)
    for pos, flags, dur in units:
        print(f"UNIT {pos} {int(bool(flags & F_KEY))} {int(bool(flags & F_DISCARD))} {dur // FIELD}")
