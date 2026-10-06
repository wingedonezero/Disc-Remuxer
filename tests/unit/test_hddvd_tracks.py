"""libavformat/hddvd_tracks.c: the tracks of HD DVD titles. With
HDDVD_CORPUS (a folder of HD DVD images), HDDVD_TRACKS_REFERENCE (one
<image name>.tracks.txt per image, every character but A-Z a-z 0-9 _ of the
name replaced by _; one line per title: index|first EVO file|tracks, each
track type:Matroska codec id:language) and HDDVD_KEYDB (key files,
comma-separated, for the encrypted images) every image that opens must give
the reference's titles and tracks."""

import os
import pathlib
import re

import pytest

from helpers.disc import env_or_skip
from helpers.hddvd import open_file, open_image
from synth.hddvd import (ac3_frame, attr_record, clip, eac3_frame, evob, image, pack, playlist, ps1_packet,
                         tmap_blocks, video_packet, vti_with)


def mkv_codec(name):
    """Matroska codec id of an FFmpeg codec name (the codecs HD DVD tracks have)."""
    return {"mpeg2video": "V_MPEG2", "vc1": "V_VC1", "h264": "V_MPEG4/ISO/AVC", "ac3": "A_AC3", "eac3": "A_EAC3",
            "truehd": "A_TRUEHD", "dts": "A_DTS", "mp2": "A_MPEG/L2", "pcm_dvd": "A_PCM/INT/BIG",
            "dvd_subtitle": "S_VOBSUB"}.get(name, name)


def track_lines(dump):
    """Our titles and tracks as reference lines."""
    out = []
    for line in dump.splitlines():
        if line.startswith("title "):
            # i|name|lang|kind|selected|duration|secs|size|file|...
            f = line[6:].split("|")
            out.append(f"{f[0]}|{f[8]}|")
        elif line.startswith("  track "):
            # k|type|codec|index|number|lang[|WxH]
            f = line[8:].split("|")
            kind = {"video": "Video", "audio": "Audio", "subtitle": "Subtitles"}.get(f[1], f[1])
            if not out[-1].endswith("|"):
                out[-1] += ","
            out[-1] += f"{kind}:{mkv_codec(f[2])}:{f[5]}"
    return out


@pytest.mark.disc
def test_corpus_every_track_list_equals_the_reference():
    folder, refdir = env_or_skip("HDDVD_CORPUS", "HDDVD_TRACKS_REFERENCE")
    kf = [f for f in os.environ.get("HDDVD_KEYDB", "").split(",") if f]
    images = sorted(p for p in pathlib.Path(folder).iterdir() if p.suffix.lower() == ".iso")
    bad = checked = 0
    for img in images:
        stem = re.sub(r"[^A-Za-z0-9_]", "_", img.stem)
        d = open_file(img, kf)
        if d.error:
            print(f"{stem}: does not open ({d.error}): not checked")
            continue
        got = track_lines(d.tracks())
        want = (pathlib.Path(refdir) / f"{stem}.tracks.txt").read_text().splitlines()
        checked += 1
        if got == want:
            print(f"{stem}: {len(got)} titles, tracks equal")
            continue
        bad += 1
        print(f"{stem}: DIFFERS ({len(got)} titles, reference {len(want)})")
        for i in range(max(len(got), len(want))):
            g = got[i] if i < len(got) else "-"
            w = want[i] if i < len(want) else "-"
            if g != w:
                print(f"  ours {g}\n  ref  {w}")
    assert checked > 0, "no image opened"
    assert bad == 0, "track lists differ"


# ---- rules on built images ----

T = "00:00:00:00"
PALETTE = [0x00108080 + i for i in range(32)]


def disc(evobs, attr_of, attrs, xpl, content):
    """A disc: EVOBs with their attribute records, a playlist, each EVOB's
    stream content (packs; empty: one padding pack)."""
    files = [("ADV_OBJ", "VPLST000.XPL", xpl), ("HVDVD_TS", "HVA00001.VTI", vti_with(evobs, attr_of, attrs))]
    for e, c in zip(evobs, content):
        c = c or pack([])
        base = e.name[:-len(".EVO")]
        files.append(("HVDVD_TS", f"{base}.MAP", tmap_blocks(len(c) // 2048)))
        files.append(("HVDVD_TS", e.name, c))
    return image(files)


def tracks_of(im):
    """The titles and tracks of a disc: the title lines' first EVO file, the
    track lines."""
    o = open_image(im)
    assert not o.error
    d = o.tracks()
    assert not isinstance(d, int), f"tracks failed: {d}"
    out = []
    for line in d.splitlines():
        if line.startswith("title "):
            out.append(f"title {line[6:].split('|')[8]}")
        elif line.startswith("  track "):
            out.append(f"  {line[8:]}")
    return out


def clip_with(m, children):
    """A PrimaryAudioVideoClip with child elements."""
    c = clip(m, T, T, False)
    return f"{c[:-2]}>{children}</PrimaryAudioVideoClip>"


def one_title(name, extra):
    return playlist("", f"<Title>{clip_with(name, extra)}</Title>")


def test_tracks_follow_the_attribute_record():
    attr = attr_record([0x60, 0, 0x50], [0x00, 0x04, 0x08, 0x10, 0x14, 0x20],
                       [[0x00, 0x21, 0, 0, 0], [0x20, 0, 0, 0x23, 0], [0x80, 0, 0, 0, 0x24], [0x40, 0x25, 0, 0, 0],
                        [0x00, 0, 0, 0, 0]], PALETTE)
    im = disc([evob("A.EVO", 1, 10)], [1], [attr], one_title("A.MAP", ""), [b""])
    # a first byte of 0x20 or more: no track; sub-pictures: the stream number of the first
    # display mode with bit 5, codings 0 / 1 / 4 only, the palette's upper half
    # when the first display mode's bit 5 is set
    assert tracks_of(im) == [
        "title A.EVO",
        "  0|video|vc1|0|0|",
        "  1|audio|ac3|1|0|",
        "  2|audio|truehd|2|1|",
        "  3|audio|mp2|3|2|",
        "  4|audio|pcm_dvd|4|3|",
        "  5|audio|pcm_dvd|5|4|",
        "  6|subtitle|dvd_subtitle|7|1||720x480|00108090",
        "  7|subtitle|dvd_subtitle|8|3||720x480|00108080",
        "  8|subtitle|dvd_subtitle|9|4||720x480|00108080",
    ]


def test_an_audio_stream_of_coding_3_leaves_the_title_out():
    ok = attr_record([0x60, 0, 0x50], [0x00], [], PALETTE)
    bad = attr_record([0x60, 0, 0x50], [0x00, 0x0c], [], PALETTE)
    body = f"<Title>{clip('A.MAP', T, T, False)}</Title><Title>{clip('B.MAP', T, T, False)}</Title>"
    im = disc([evob("A.EVO", 1, 10), evob("B.EVO", 2, 10)], [1, 2], [ok, bad], playlist("", body), [b"", b""])
    assert tracks_of(im) == ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|ac3|1|0|"]


def test_video_coding_and_sub_picture_frame_size():
    cases = [
        ([0x00, 0, 0x00], "mpeg2video", "352x240"),
        ([0x24, 0, 0x10], "mpeg2video", "352x576"),
        ([0x40, 0, 0x30], "h264", "544x480"),
        ([0x74, 0, 0x80], "vc1", "1280x720"),
        ([0x60, 0, 0xc0], "vc1", "1920x1080"),
        ([0x60, 0, 0x60], "vc1", "0x0"),
    ]
    names = ["A.EVO", "B.EVO", "C.EVO", "D.EVO", "E.EVO", "F.EVO", "G.EVO"]
    attrs = [attr_record(v, [], [[0, 0x20, 0, 0, 0]], PALETTE) for v, _, _ in cases]
    attrs.append(attr_record([0x80, 0, 0], [], [], PALETTE))  # no video: the title is left out
    evobs = [evob(n, i + 1, 10) for i, n in enumerate(names)]
    body = "".join(f"<Title>{clip(n.replace('EVO', 'MAP'), T, T, False)}</Title>" for n in names)
    im = disc(evobs, [1, 2, 3, 4, 5, 6, 7], attrs, playlist("", body), [b""] * 7)
    want = []
    for i, (_, codec, size) in reversed(list(enumerate(cases))):
        want += [f"title {names[i]}", f"  0|video|{codec}|0|0|", f"  1|subtitle|dvd_subtitle|1|0||{size}|00108090"]
    assert tracks_of(im) == want


def test_a_stream_with_the_codec_and_number_of_an_earlier_one_is_left_out():
    attr = attr_record([0x60, 0, 0x50], [0x00], [[0, 0x21, 0, 0, 0], [0x20, 0x21, 0, 0, 0], [0, 0x22, 0, 0, 0]],
                       PALETTE)
    im = disc([evob("A.EVO", 1, 10)], [1], [attr], one_title("A.MAP", ""), [b""])
    assert tracks_of(im) == [
        "title A.EVO",
        "  0|video|vc1|0|0|",
        "  1|audio|ac3|1|0|",
        "  2|subtitle|dvd_subtitle|2|1||720x480|00108090",
        "  3|subtitle|dvd_subtitle|4|2||720x480|00108090",
    ]


def test_languages_come_from_the_playlists_track_navigation_list():
    attr = attr_record([0x60, 0, 0x50], [0, 0, 0, 0], [[0, 0x21, 0, 0, 0]], PALETTE)
    streams = ('<Audio streamNumber="1" track="1"/><Audio streamNumber="2" track="2"/><Audio streamNumber="3" '
               'track="3"/><Audio streamNumber="4" track="4"/><Subtitle streamNumber="2" track="1"/>')
    nav = ('<TrackNavigationList><AudioTrack track="1" langcode="fr:0"/><AudioTrack track="2" langcode="xx:1"/>'
           '<AudioTrack track="3" langcode="engl:1"/><AudioTrack track="4" langcode="de"/><SubtitleTrack '
           'track="1" langcode="DE:0"/></TrackNavigationList>')
    # the first Title has two TrackNavigationLists: not used; the second one gives
    # the languages; a langcode without ":" or with more than 3 characters before
    # it gives none
    c = clip_with("A.MAP", streams)
    body = f"<Title>{c}{nav}{nav}</Title><Title>{c}{nav}</Title>"
    im = disc([evob("A.EVO", 1, 10)], [1], [attr], playlist("", body), [b""])
    assert tracks_of(im) == [
        "title A.EVO",
        "  0|video|vc1|0|0|",
        "  1|audio|ac3|1|0|fre",
        "  2|audio|ac3|2|1|xx",
        "  3|audio|ac3|3|2|",
        "  4|audio|ac3|4|3|",
        "  5|subtitle|dvd_subtitle|5|1|ger|720x480|00108090",
    ]


def test_the_tracks_come_from_the_clip_with_the_most_bytes():
    a = attr_record([0x00, 0, 0x50], [0x00], [], PALETTE)
    b = attr_record([0x60, 0, 0x50], [0x04, 0x00], [], PALETTE)
    body = f"<Title>{clip('A.MAP', T, T, False)}{clip('B.MAP', T, T, True)}</Title>"
    im = disc([evob("A.EVO", 1, 10), evob("B.EVO", 2, 10)], [1, 2], [a, b], playlist("", body),
              [b"", pack([]) + pack([])])
    assert tracks_of(im) == ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|truehd|1|0|", "  2|audio|ac3|2|1|"]


# ---- the Dolby Digital Plus probe ----

def ddplus(packs):
    """One title over one EVOB with a Dolby Digital Plus stream 0 and packs."""
    attr = attr_record([0x60, 0, 0x50], [0x1c], [], PALETTE)
    im = disc([evob("A.EVO", 1, 10)], [1], [attr], one_title("A.MAP", ""), [b"".join(packs)])
    return tracks_of(im)


EAC3 = ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|eac3|1|0|"]


def e():
    return eac3_frame(256, 0, None)


def test_four_e_ac3_frames_make_an_e_ac3_track_four_ac3_frames_an_ac3_track():
    assert ddplus([pack([ps1_packet(0xc0, 90000, e() * 4)])]) == EAC3
    # frames run on into the next pack (its packet without a PTS)
    a = ac3_frame
    assert ddplus([pack([ps1_packet(0xc0, 90000, a() + a() + a())]), pack([ps1_packet(0xc0, None, a())])]) == \
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|ac3|1|0|"]
    # data before the first frame start is skipped
    assert ddplus([pack([ps1_packet(0xc0, 90000, bytes(100) + e() * 4)])]) == EAC3


def test_a_mix_of_ac3_and_e_ac3_frames_or_fewer_than_four_frames_leave_the_stream_out():
    video_only = ["title A.EVO", "  0|video|vc1|0|0|"]
    assert ddplus([pack([ps1_packet(0xc0, 90000, e() + e() + ac3_frame() + e())])]) == video_only
    assert ddplus([pack([ps1_packet(0xc0, 90000, e() * 3)])]) == video_only


def test_the_stream_needs_a_pts_packet_first_in_one_of_its_first_2000_packs():
    audio = pack([ps1_packet(0xc0, 90000, e() * 4)])
    assert ddplus([pack([video_packet(1000)])] * 1999 + [audio]) == EAC3
    # the same packet behind a video packet does not count; nor does a PTS packet in pack 2001
    packs = [pack([video_packet(1000), ps1_packet(0xc0, 90000, e())])]
    packs += [pack([video_packet(1000)])] * 1999
    packs.append(audio)
    assert ddplus(packs) == []


def test_the_first_frame_must_be_a_valid_header():
    # a dependent stream frame whose channel map gives 1 channel for a 2/0 mode
    bad = eac3_frame(256, 1, 0x8000)
    assert ddplus([pack([ps1_packet(0xc0, 90000, bad + e() * 3)])]) == []
    good = eac3_frame(256, 1, 0xa000)  # L, R
    assert ddplus([pack([ps1_packet(0xc0, 90000, good + e() * 3)])]) == EAC3


def test_a_pack_with_more_than_two_packets_fails_the_probe():
    assert ddplus([
        pack([ps1_packet(0xc0, 90000, e() * 2)]),
        pack([video_packet(500), ps1_packet(0xc0, None, e()), ps1_packet(0xc0, None, e())]),
    ]) == []


# ---- the DTS probe ----

def dts(packs):
    """One title over one EVOB with a DTS stream 0 and packs."""
    attr = attr_record([0x60, 0, 0x50], [0x18], [], PALETTE)
    im = disc([evob("A.EVO", 1, 10)], [1], [attr], one_title("A.MAP", ""), [b"".join(packs)])
    return tracks_of(im)


def dts_core(size):
    """A DTS core frame of size bytes (48 kHz, 3/2, 512 samples)."""
    f = bytearray(size)
    f[:4] = bytes([0x7f, 0xfe, 0x80, 0x01])
    fsize = size - 1
    nblks = 15  # 16 blocks of 32 samples
    # bits from byte 4: FTYPE 1, SHORT 5 (31), CPF 0, NBLKS 7, FSIZE 14, AMODE 6, SFREQ 4, RATE 5, ...
    v = (1 << 63) | (31 << 58) | (nblks << 50) | (fsize << 36) | (9 << 30) | (13 << 26) | (15 << 21)
    f[4:12] = v.to_bytes(8, "big")
    return bytes(f)


def dts_extss(size, asset):
    """A DTS-HD extension substream frame of size bytes: no static fields,
    one asset of asset bytes (header 16 bytes)."""
    f = bytearray(size)
    f[:4] = bytes([0x64, 0x58, 0x20, 0x25])
    # user 8, index 2, header size type 0, header size-1 8, frame size-1 16, static 0, asset size-1 16,
    # descriptor size 9, index 3
    bits = [(0, 8), (0, 2), (0, 1), (15, 8), (size - 1, 16), (0, 1), (asset - 1, 16), (0, 9), (0, 3)]
    acc, n = 0, 0
    for v, w in bits:
        acc = (acc << w) | v
        n += w
    acc <<= 128 - n
    f[4:20] = acc.to_bytes(16, "big")
    return bytes(f)


def test_a_dts_stream_is_dts_or_dts_hd_with_its_core_as_a_second_track():
    core = dts_core(900)
    assert dts([pack([ps1_packet(0x88, 90000, core + core)])]) == \
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|dts|1|0|"]
    # an extension substream after the first core frame: DTS-HD, then its core
    unit = dts_core(600) + dts_extss(200, 184)
    assert dts([pack([ps1_packet(0x88, 90000, unit + unit)])]) == \
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|dts|1|0||with core", "  2|audio|dts|1|0||core"]


def test_a_dts_stream_without_a_frame_or_with_a_bad_first_header_leaves_the_title_out():
    # no frame at all
    assert dts([pack([ps1_packet(0x88, 90000, bytes(100))])]) == []
    # the asset is larger than the substream frame
    unit = dts_core(600) + dts_extss(200, 300)
    assert dts([pack([ps1_packet(0x88, 90000, unit + unit)])]) == []


def test_dts_data_before_a_frame_start_is_dropped_up_to_the_first_access_unit():
    # the packet starts with the end of a frame; its first access unit pointer
    # says where the next frame starts: those bytes are dropped, the frame is found
    core = dts_core(900)
    p = bytearray(ps1_packet(0x88, 90000, b"\x55" * 50 + core + core))
    hdr = 9 + 5  # PES header with a PTS
    p[hdr + 2:hdr + 4] = (51).to_bytes(2, "big")  # first access unit at byte 50 of the payload
    assert dts([pack([bytes(p)])]) == ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|dts|1|0|"]
