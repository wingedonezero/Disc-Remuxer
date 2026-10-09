"""libavformat/dvd_streams.c and the track list of dvddec.c: a DVD title's
stream pass (step D3, part b) on built disc folders, seen through
`disc-remuxer info`: an audio stream without data in the first 48 MiB is
left out when the program chain's audio_control does not list it (looked up
by its place in the audio list), else after the whole search as empty; a
stream listed twice keeps its first track; an unknown coding mode is left
out; closed captions in the video give a caption track (English, last)."""

from helpers.dvdnav import Pgc, build, cell, vobus
from synth.dvd import S
from test_dvd_read import ac3_pack, pack
from test_dvd_titles import ac3, info, subp


def put(folder, sector, data):
    """Sector of the title set's title VOBs replaced by data."""
    path = folder / "VIDEO_TS" / "VTS_01_1.VOB"
    vob = bytearray(path.read_bytes())
    vob[sector * S:(sector + 1) * S] = data
    path.write_bytes(bytes(vob))


def audio_pack(stream):
    """An unscrambled AC-3 pack of sub-stream 0x80 + stream."""
    p = bytearray(ac3_pack())
    p[20] &= 0xcf
    p[28] = 0x80 + stream
    return bytes(p)


def disc(tmp_path, audio_control, audio_attrs, data=(), subp_control=bytes(128), subp_attrs=()):
    """One title of one cell (sectors 0-9, NAV packs at 0 and 5); data:
    (sector, bytes) put into its title VOBs."""
    p = Pgc(cells=[cell(0, 9, 1, last_vobu=5)], audio_control=audio_control + bytes(16 - len(audio_control)),
            subp_control=subp_control)
    folder = build(tmp_path, [p], [[(1, 1)]], vobus(0, 9, 5), 10, video_attr=bytes([0x40, 0]),
                   audio_attrs=list(audio_attrs), subp_attrs=list(subp_attrs))
    for sector, d in data:
        put(folder, sector, d)
    return folder


def kinds(t):
    return [(k["kind"], k["codec"], k["language"]) for k in t["tracks"]]


def texts(lines):
    return [x.get("text", "") for x in lines if x.get("type") == "message"]


def said(lines, part):
    return any(part in t for t in texts(lines))


def test_an_unlisted_audio_stream_without_data_is_left_out(tmp_path):
    # audio_control lists stream 0; the title set declares a second one, with no data
    folder = disc(tmp_path, bytes([0x80, 0]), [ac3(b"en"), ac3(b"fr")], [(1, audio_pack(0))])
    ts, lines = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", ""), ("audio", "DD", "eng")]
    assert said(lines, "title 1: audio stream 1 has no data in the first 48 MiB and the program chain does not "
                       "list it: left out")


def test_a_listed_audio_stream_without_data_is_left_out_as_empty(tmp_path):
    folder = disc(tmp_path, bytes([0x80, 0, 0x81, 0]), [ac3(b"en"), ac3(b"fr")], [(1, audio_pack(0))])
    ts, lines = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", ""), ("audio", "DD", "eng")]
    assert said(lines, "title 1: audio stream 1 (track 1) looks empty (no data in the first 200 MiB, the 100 MiB "
                       "from the middle and the last 100 MiB of segment 1): left out")


def test_audio_streams_with_data_are_kept_wherever_it_is(tmp_path):
    folder = disc(tmp_path, bytes([0x80, 0, 0x81, 0]), [ac3(b"en"), ac3(b"fr")],
                  [(1, audio_pack(0)), (9, audio_pack(1))])
    ts, lines = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", ""), ("audio", "DD", "eng"), ("audio", "DD", "fre")]
    assert not said(lines, "audio stream")


def test_whether_audio_control_lists_a_stream_is_taken_from_its_place(tmp_path):
    # audio_control lists stream 1 only: the audio list is stream 1 (place 0),
    # stream 0 (place 1); the test looks at the place: stream 1 counts as not
    # listed, stream 0 as listed
    folder = disc(tmp_path, bytes([0x81, 0]), [ac3(b"en"), ac3(b"fr")])
    ts, lines = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", "")]
    assert said(lines, "title 1: audio stream 1 has no data in the first 48 MiB and the program chain does not list it")
    assert said(lines, "title 1: audio stream 0 (track 1) looks empty")


def test_a_stream_listed_twice_keeps_its_first_track(tmp_path):
    folder = disc(tmp_path, bytes([0x80, 0, 0x80, 0]), [ac3(b"en"), ac3(b"en")], [(1, audio_pack(0))],
                  subp_control=bytes([0x80, 0, 0, 0, 0x80, 0, 0, 0]) + bytes(120), subp_attrs=[subp(), subp()])
    ts, lines = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", ""), ("audio", "DD", "eng"), ("subtitle", "VobSub", "eng")]
    assert said(lines, "title 1: audio track 1 is stream 0 again (as track 0): left out")
    assert said(lines, "title 1: sub-picture track 1 is stream 0 again: left out")


def test_an_audio_stream_of_an_unknown_coding_mode_is_left_out(tmp_path):
    folder = disc(tmp_path, bytes([0x80, 0]), [bytes([0x24, 0]) + b"en" + bytes(4)], [(1, audio_pack(0))])
    ts, lines = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", "")]
    assert said(lines, "title 1: audio stream 0 has coding mode 1, which is not known: left out")


def caption_pack(flag=0):
    return pack(0xe0, b"\0\0\1\xb2CC\x01\xf8" + bytes([flag]) + bytes(32), False)


def test_closed_captions_in_the_video_give_a_caption_track(tmp_path):
    folder = disc(tmp_path, b"", [], [(1, caption_pack())])
    ts, _ = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", ""), ("subtitle", "eia_608", "eng")]


def test_caption_user_data_with_bit_6_set_gives_no_caption_track(tmp_path):
    folder = disc(tmp_path, b"", [], [(1, caption_pack(0x40))])
    ts, _ = info(folder)
    assert kinds(ts[0]) == [("video", "Mpeg2", "")]
