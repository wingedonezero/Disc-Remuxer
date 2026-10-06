"""libavformat/discrip_verify.c: the checks of a track's output. Units are
parsed again (AC-3 CRC, TrueHD length and major-sync checksum, MPEG-2 start
codes); times are checked (audio in order, video without holes in display
order); the audio sync error is measured as a stream file plays the frames
(back to back from the start delay) and by the output times. Units from
FFmpeg's encoders (tests/data/README.md)."""

from helpers import DATA, ffi, lib
from helpers.discrip import Frame, Handles, codec_id, events_into, pictures

AC3 = (DATA / "sine_7680.ac3").read_bytes()
THD = (DATA / "sine_4813.thd").read_bytes()
M2V = (DATA / "testsrc_1200.m2v").read_bytes()
MS = 1080000
AC3_DUR = 32 * MS
F_KEY, F_MARKER = lib.DR_F_KEY, lib.DR_F_MARKER
AUDIO, VIDEO = lib.DR_KIND_AUDIO, lib.DR_KIND_VIDEO


def fi(data, time, dur, src, flags):
    """One output frame: bytes, output time, duration, own (source) time."""
    return {"data": data, "time": time, "dur": dur, "src": src, "flags": flags}


def check(codec, kind, frames):
    events = []
    keep = Handles()
    v = ffi.new("DRVerify **")
    st = ffi.new("DRVerifyStats *")
    assert lib.ff_discrip_verify_open(v, ffi.NULL, codec_id(codec), 1, kind, lib.tb_py_event,
                                      keep(events_into(events))) == 0
    for f in frames:
        fr = Frame(f["data"], f["time"], f["dur"], f["flags"], src=f["src"])
        assert lib.ff_discrip_verify_frame(v[0], fr.ptr) == 0
    assert lib.ff_discrip_verify_finish(v[0]) == 0
    lib.ff_discrip_verify_stats(v[0], st)
    lib.ff_discrip_verify_close(v)
    return st, events


def ac3_frames():
    return [AC3[i:i + 768] for i in range(0, len(AC3), 768)]


def kinds(e):
    return [x["kind"] for x in e]


def test_good_ac3_frames_pass():
    f = [fi(d, k * AC3_DUR + 5 * MS, AC3_DUR, k * AC3_DUR + 5 * MS, F_KEY) for k, d in enumerate(ac3_frames())]
    st, ev = check("ac3", AUDIO, f)
    assert ev == [], ev
    assert (st.frames, st.bad_units, st.crc_errors, st.unchecked) == (5, 0, 0, 0)
    assert st.delay == 5 * MS
    assert (st.es_err_max, st.es_err_end, st.mkv_err_max) == (0, 0, 0)


def test_a_damaged_or_cut_ac3_frame_is_found():
    bad = bytearray(ac3_frames()[1])
    bad[300] ^= 0x10
    cut = ac3_frames()[2][:700]
    f = [fi(ac3_frames()[0], 0, AC3_DUR, 0, F_KEY), fi(bytes(bad), AC3_DUR, AC3_DUR, AC3_DUR, F_KEY),
         fi(cut, 2 * AC3_DUR, AC3_DUR, 2 * AC3_DUR, F_KEY)]
    st, ev = check("ac3", AUDIO, f)
    assert (st.crc_errors, st.bad_units) == (1, 1)
    assert kinds(ev) == [lib.DR_EV_VERIFY_CRC, lib.DR_EV_VERIFY_UNIT]


def test_the_sync_error_of_a_stream_file_and_of_the_output_times():
    # frame 2 dropped (the junction cut a skew): later frames play 32 ms
    # early in a stream file, their output times are their own; then a
    # real 100 ms gap the stream file cannot hold: 132 ms early from there
    fr = ac3_frames()
    src = [0, AC3_DUR, 3 * AC3_DUR, 4 * AC3_DUR + 100 * MS]
    st, _ = check("ac3", AUDIO, [fi(fr[k], s, AC3_DUR, s, F_KEY) for k, s in enumerate(src)])
    assert (st.es_err_max, st.es_err_at) == (-132 * MS, 4 * AC3_DUR + 100 * MS)
    assert st.es_err_end == -132 * MS
    assert st.mkv_err_max == 0
    # output times shifted against the own times (a junction skew) show up
    # in the output-time error
    st, _ = check("ac3", AUDIO, [fi(fr[k], k * AC3_DUR + 3 * MS, AC3_DUR, k * AC3_DUR, F_KEY) for k in range(3)])
    assert (st.mkv_err_max, st.es_err_max, st.delay) == (3 * MS, 3 * MS, 3 * MS)


def test_audio_out_of_order_or_without_duration():
    fr = ac3_frames()
    f = [fi(fr[0], AC3_DUR, AC3_DUR, AC3_DUR, F_KEY), fi(fr[1], AC3_DUR, AC3_DUR, AC3_DUR, F_KEY),
         fi(fr[2], 2 * AC3_DUR, 0, 2 * AC3_DUR, F_KEY)]
    st, ev = check("ac3", AUDIO, f)
    assert st.order_errors == 2
    assert kinds(ev) == [lib.DR_EV_VERIFY_ORDER, lib.DR_EV_VERIFY_ORDER]


def thd_units():
    """The TrueHD access units of the test stream."""
    v, o = [], 0
    while o + 4 <= len(THD):
        n = (((THD[o] & 0xF) << 8) | THD[o + 1]) * 2
        v.append(THD[o:o + n])
        o += n
    return v


def thd_frames(units, first):
    au = 40 * 1080000000 // 48000  # 40 samples at 48 kHz
    return [fi(d, first + k * au, au, first + k * au, F_KEY) for k, d in enumerate(units)]


def test_truehd_checksums():
    units = thd_units()
    assert len(units) == 121
    st, ev = check("truehd", AUDIO, thd_frames(units, 0))
    assert ev == [], ev
    assert (st.bad_units, st.crc_errors) == (0, 0)
    # a damaged major sync
    bad = bytearray(units[0])
    bad[12] ^= 0x01
    f = thd_frames(units[:3], 0)
    f[0]["data"] = bytes(bad)
    st, _ = check("truehd", AUDIO, f)
    assert st.crc_errors == 1


def m2v_frames(skip):
    """The MPEG-2 test stream's pictures as output frames on a 29.97 fps
    grid (display order from the bitstream), except those in skip."""
    frame = 36036000
    return [fi(M2V[a:b], disp * frame, frame, disp * frame, 0)
            for k, (a, b, disp, _) in enumerate(pictures(M2V)) if k not in skip]


def test_video_in_display_order_without_holes():
    st, ev = check("mpeg2video", VIDEO, m2v_frames([]))
    assert ev == [], ev
    assert (st.bad_units, st.holes, st.overlaps) == (0, 0, 0)


def test_a_missing_picture_is_a_hole_and_placeholders_are_counted_in_it():
    frame = 36036000
    f = m2v_frames([100, 101])
    disp = [p[2] for p in pictures(M2V)]
    a, b = min(disp[100], disp[101]), max(disp[100], disp[101])
    f.insert(50, fi(b"", a * frame, 0, a * frame, F_KEY | F_MARKER))
    st, ev = check("mpeg2video", VIDEO, f)
    holes = [e for e in ev if e["kind"] == lib.DR_EV_VERIFY_HOLE]
    if b == a + 1:
        assert len(holes) == 1
        assert (holes[0]["pos"], holes[0]["dur"], holes[0]["count"]) == (a * frame, 2 * frame, 1)
    else:
        assert len(holes) == 2
    assert st.holes == len(holes)
    assert st.markers == 1


def test_video_overlaps_and_bad_units():
    frame = 36036000
    f = m2v_frames([])
    f[10]["dur"] = frame + frame // 2  # runs half a frame into the next one shown
    f[20]["data"] = b"\xaa" * 16
    st, ev = check("mpeg2video", VIDEO, f)
    assert (st.overlaps, st.overlap_dur, st.bad_units) == (1, frame // 2, 1)
    assert lib.DR_EV_VERIFY_OVERLAP in kinds(ev)
    assert lib.DR_EV_VERIFY_UNIT in kinds(ev)


def test_a_codec_without_a_unit_check_is_counted():
    st, _ = check("pcm_dvd", AUDIO, [fi(bytes(64), 0, MS, 0, F_KEY)])
    assert st.unchecked == 1
