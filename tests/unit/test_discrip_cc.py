"""DVD-Video line-21 captions in the rip core: the MPEG-2 rules take GOP user
data out of the video as side units, timed at the next picture after their
unit (discrip_mpv.c, discrip_video.c); discrip_cc.c checks a caption block
and turns its entries into the caption decoder's input (field markers 4 /
5). The video is the MPEG-2 test stream (tests/data/README.md) with user
data inserted after its GOP headers."""

from helpers import DATA, ffi, lib
from helpers.discrip import Handles, codec_id, fn, pictures

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
FRAME = 36036000  # 1001/30000 s
BASE = 10 * 1080000000


def cc_block(count, entries):
    """A caption block with its entries (marker byte, two data bytes)."""
    return bytes([0x00, 0x00, 0x01, 0xB2, ord("C"), ord("C"), 0x01, 0xF8, count]) + b"".join(bytes(e) for e in entries)


def with_user_data(insert):
    """The stream with insert(gop) after each GOP header."""
    out = bytearray()
    i = gop = 0
    while i < len(M2V):
        if i + 4 <= len(M2V) and M2V[i:i + 4] == b"\x00\x00\x01\xb8":
            out += M2V[i:i + 8] + insert(gop)
            gop += 1
            i += 8
        else:
            out.append(M2V[i])
            i += 1
    return bytes(out)


def run(m2v, side):
    """The stream through the cutter and the video timing (every picture on
    its grid): (video frames, side units, stats), frames as (bytes, time,
    dur, flags)."""
    cid = codec_id("mpeg2video")
    video, sides = [], []

    def into(lst):
        def on_frame(f):
            lst.append((bytes(ffi.buffer(f.data, f.size)), f.time, f.dur, f.flags))
            lib.ff_discrip_frame_unref(f)
            return 0
        return on_frame
    keep = Handles()
    v = ffi.new("DRVideo **")
    c = ffi.new("DRCutter **")
    st = ffi.new("DRVideoStats *")
    assert lib.ff_discrip_video_open(v, ffi.NULL, cid, 0, lib.tb_py_frame, keep(into(video)), ffi.NULL, ffi.NULL) == 0
    if side:
        lib.ff_discrip_video_set_side(v[0], lib.tb_py_frame, keep(into(sides)))
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_video_unit"), v[0]) == 0
    for a, b, disp, _ in pictures(m2v):
        assert lib.ff_discrip_cutter_write(c[0], m2v[a:b], b - a, BASE + disp * FRAME) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    assert lib.ff_discrip_video_flush(v[0]) == 0
    lib.ff_discrip_video_stats(v[0], st)
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_video_close(v)
    return video, sides, st


def test_gop_user_data_leaves_the_video_with_the_next_pictures_time():
    # a caption block after every GOP header, other user data after every
    # 10th one as well
    def block(g):
        return cc_block(0x82, [[0xFF, g % 256, 0x80], [0xFE, 0x80, 0x80]])

    def other(g):
        return bytes([0x00, 0x00, 0x01, 0xB2, ord("X"), ord("Y"), 0x42]) if g % 10 == 0 else b""
    stream = with_user_data(lambda g: block(g) + other(g))
    video, sides, st = run(stream, True)

    # the video is the stream without the user data, picture by picture
    assert b"".join(o[0] for o in video) == M2V
    assert st.out == len(pictures(M2V))

    # each piece, in stream order, at the time of the next picture after
    # its I picture when that one is in the I picture's batch (shown before
    # it: the first B picture of an open GOP, the GOP's first picture on
    # screen), else at the I picture's own time (a closed GOP: the I picture
    # is a batch of its own and the first on screen); one field long
    pics = pictures(stream)
    gop_at = [i for i in range(len(stream) - 3) if stream[i:i + 4] == b"\x00\x00\x01\xb8"]
    want = []
    for g, at in enumerate(gop_at):
        k = next(n for n, p in enumerate(pics) if p[0] <= at < p[1])
        shown = pics[k + 1][2] if pics[k + 1][2] < pics[k][2] else pics[k][2]
        t = BASE + shown * FRAME
        want.append((block(g), t))
        if g % 10 == 0:
            want.append((other(g), t))
    assert [(o[0], o[1]) for o in sides] == want
    assert all(o[2] == FRAME // 2 and o[3] == lib.DR_F_KEY for o in sides)
    assert st.side == len(want)


def test_without_a_side_callback_the_user_data_is_still_taken_out():
    video, _, st = run(with_user_data(lambda g: cc_block(0x80, [])), False)
    assert b"".join(o[0] for o in video) == M2V
    assert st.side == 101


def check(b):
    return lib.ff_discrip_cc_check(bytes(b), len(b))


def test_a_caption_block_is_its_header_and_its_entries():
    b = cc_block(0x03, [[0xFF, 1, 2], [0xFE, 3, 4], [0xFF, 5, 6]])
    assert check(b) == 18
    assert check(b + bytes(3)) == 18, "bytes after the entries are not part of it"
    assert check(b[:17]) == 0, "shorter than its count"
    bit6 = bytearray(b)
    bit6[8] |= 0x40
    assert check(bit6) == 0
    other = bytearray(b)
    other[4] = ord("X")
    assert check(other) == 0, "other user data"
    assert check(b[:8]) == 0


def triplets(b):
    out = ffi.new("uint8_t[189]")
    n = lib.ff_discrip_cc_triplets(b, len(b), out)
    return [list(out[3 * i:3 * i + 3]) for i in range(n)]


def test_entries_get_field_markers_from_their_pattern():
    # odd field first (bit 7): FF FE -> field 1, field 2
    assert triplets(cc_block(0x82, [[0xFF, 0x94, 0x2C], [0xFE, 0x80, 0x80]])) == [[4, 0x94, 0x2C], [5, 0x80, 0x80]]
    # odd field first, FF FF: field 1, field 2 (both marked odd)
    assert triplets(cc_block(0x82, [[0xFF, 1, 2], [0xFF, 3, 4]])) == [[4, 1, 2], [5, 3, 4]]
    # odd field first, FE FF: field 2, field 1
    assert triplets(cc_block(0x82, [[0xFE, 1, 2], [0xFF, 3, 4]])) == [[5, 1, 2], [4, 3, 4]]
    # even field first: FF FE -> field 1, field 2; FE FE / FE FF / FF FF -> field 2, field 1
    assert triplets(cc_block(0x02, [[0xFF, 1, 2], [0xFE, 3, 4]])) == [[4, 1, 2], [5, 3, 4]]
    for pat in [[0xFE, 0xFE], [0xFE, 0xFF], [0xFF, 0xFF]]:
        assert triplets(cc_block(0x02, [[pat[0], 1, 2], [pat[1], 3, 4]])) == [[5, 1, 2], [4, 3, 4]]


def test_an_unknown_marker_ends_the_block():
    b = cc_block(0x85, [[0xFF, 1, 2], [0xFE, 3, 4], [0x7F, 5, 6], [0xFE, 7, 8], [0xFF, 9, 10]])
    assert triplets(b) == [[4, 1, 2], [5, 3, 4]], "the odd last entry is not reached either"


def test_a_last_single_entry():
    # odd field first: FF -> field 1, FE -> field 2; even field first: field 2
    assert triplets(cc_block(0x81, [[0xFF, 1, 2]])) == [[4, 1, 2]]
    assert triplets(cc_block(0x81, [[0xFE, 1, 2]])) == [[5, 1, 2]]
    assert triplets(cc_block(0x01, [[0xFF, 1, 2]])) == [[5, 1, 2]]
    b = cc_block(0x83, [[0xFF, 1, 2], [0xFE, 3, 4], [0xFF, 5, 6]])
    assert triplets(b) == [[4, 1, 2], [5, 3, 4], [4, 5, 6]]
