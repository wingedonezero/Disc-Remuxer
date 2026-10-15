"""The MPEG-PS clip demuxer (src/mux/ps_disc.c): the disc formats' program
stream. The stream is read one 2048-byte sector (one pack) at a time with
the pack rules of the reference's disc reader; payloads leave with FFmpeg's
MPEG-PS stream ids and header cuts, except that a private stream 1
sub-stream 0xA0-0xAF is always linear PCM with its audio frame header kept
(FFmpeg's own mpeg.c guesses MLP from the header's dynamic range byte
and then cuts 6 more bytes). The sectors are built byte by byte."""

import struct

from helpers import AV_NOPTS_VALUE
from helpers.disctl import codec_id
from helpers.parser import ps_read

SECTOR = 2048
PACK = bytes([0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8])
SAMPLES = bytes(range(1, 97))


def pts_bytes(pts, marker_ok=True):
    b = bytearray([0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, 0x01 | ((pts >> 14) & 0xFE),
                   (pts >> 7) & 0xFF, 0x01 | ((pts << 1) & 0xFE)])
    if not marker_ok:
        b[2] &= 0xFE
    return bytes(b)


def pes(stream_id, payload, pts=90000, marker_ok=True, stuffing=0, ext=None):
    """A PES packet with a PTS (MPEG-2 header), optional stuffing bytes in
    its header and a PES extension 2 with a stream_id_extension."""
    flags = 0x80 | (0x01 if ext is not None else 0)
    opt = pts_bytes(pts, marker_ok)
    if ext is not None:
        opt += bytes([0x01, 0x81, ext])
    opt += b"\xff" * stuffing
    body = bytes([0x81, flags, len(opt)]) + opt + payload
    return bytes([0x00, 0x00, 0x01, stream_id]) + struct.pack(">H", len(body)) + body


def private1(sub, header, data, pts=90000, nf=1, fau=1):
    """A private stream 1 packet: sub-stream id, frame count, first access
    unit pointer, a codec header, the data."""
    return pes(0xBD, bytes([sub, nf]) + struct.pack(">H", fau) + header + data, pts)


def sector(*packets, fill="padding"):
    """A 2048-byte sector: the pack header, the packets, then a padding
    packet (or 0xff fill) to its end."""
    s = PACK + b"".join(packets)
    left = SECTOR - len(s)
    assert left >= 0
    if fill == "padding" and left >= 6:
        s += bytes([0x00, 0x00, 0x01, 0xBE]) + struct.pack(">H", left - 6) + b"\xff" * (left - 6)
    else:
        s += b"\xff" * left
    return s


def payloads(stream, **kw):
    ret, pkts = ps_read(stream, **kw)
    return ret, [(p[0], p[2], p[3]) for p in pkts]


# ---- LPCM ----

def test_lpcm_is_always_lpcm_with_its_header_kept():
    # DVD-Video 3-byte LPCM header and HD DVD 5-byte header, each with the
    # usual dynamic range byte 0x80 and with another value
    for header, n in ((bytes([0x00, 0x01, 0x80]), 3), (bytes([0x00, 0x01, 0x23]), 3),
                      (bytes([0x00, 0x01, 0x80, 0x00, 0x01]), 5), (bytes([0x00, 0x01, 0x55, 0x00, 0x01]), 5)):
        # the first access unit pointer points past the audio frame header
        ret, pkts = ps_read(sector(private1(0xA0, header, SAMPLES, fau=n + 1)) * 3, lpcm_header=n)
        assert ret == 3, header.hex()
        assert all((p[0], p[1], p[2]) == (0xA0, codec_id("pcm_dvd"), header + SAMPLES) for p in pkts), header.hex()


def test_ffmpeg_s_reader_cuts_lpcm_when_the_dynamic_range_byte_is_not_0x80():
    # what the disc reader fixes: upstream takes the stream for MLP and cuts 6 bytes
    _, pkts = ps_read(sector(private1(0xA0, bytes([0x00, 0x01, 0x23]), SAMPLES)), disc=False)
    assert (pkts[0][1], pkts[0][2]) == (codec_id("mlp"), (bytes([0x00, 0x01, 0x23]) + SAMPLES)[6:])
    _, pkts = ps_read(sector(private1(0xA0, bytes([0x00, 0x01, 0x80]), SAMPLES)), disc=False)
    assert (pkts[0][1], pkts[0][2]) == (codec_id("pcm_dvd"), bytes([0x00, 0x01, 0x80]) + SAMPLES)


def test_lpcm_packets_that_cannot_hold_their_header_fail():
    # shorter than the 5-byte header: fails; the first access unit inside the
    # header: fails; only the header: no packet (padding)
    assert ps_read(sector(private1(0xA0, bytes([0x00, 0x01, 0x80]), b"")), lpcm_header=5)[0] < 0
    assert ps_read(sector(private1(0xA0, bytes(5), SAMPLES, fau=3)), lpcm_header=5)[0] < 0
    assert ps_read(sector(private1(0xA0, bytes(5), b"")), lpcm_header=5) == (0, [])


# ---- the other streams ----

def test_the_other_streams_leave_as_ffmpeg_s_reader_gives_them():
    cases = [
        (private1(0x20, b"", SAMPLES), 0x20, bytes([0x01, 0x00, 0x01]) + SAMPLES),  # sub-pictures: the id only
        (private1(0x80, b"", SAMPLES), 0x80, SAMPLES),                             # AC-3: id + 3
        (private1(0x88, b"", SAMPLES), 0x88, SAMPLES),                             # DTS: id + 3
        (private1(0xC0, b"", SAMPLES), 0xC0, SAMPLES),                             # Dolby Digital Plus: id + 3
        (private1(0xB0, bytes([0x07]), SAMPLES), 0xB0, SAMPLES),                  # TrueHD: id + 4
        (pes(0xC0, SAMPLES), 0x1C0, SAMPLES),                                     # MPEG audio
        (pes(0xE0, SAMPLES), 0x1E0, SAMPLES),                                     # MPEG-2 video
    ]
    for pkt, sid, want in cases:
        stream = sector(pkt)
        ours, theirs = payloads(stream), payloads(stream, disc=False)
        assert ours == theirs, hex(sid)
        assert ours == (1, [(sid, want, 90000)]), hex(sid)


def test_vc1_takes_its_stream_id_extension():
    assert payloads(sector(pes(0xFD, SAMPLES, ext=0x55))) == (1, [(0xFD55, SAMPLES, 90000)])


# ---- the pack rules ----

def test_a_pts_whose_marker_bits_are_wrong_is_no_time():
    stream = sector(pes(0xE0, SAMPLES, marker_ok=False))
    assert payloads(stream) == (1, [(0x1E0, SAMPLES, AV_NOPTS_VALUE)])
    assert payloads(stream, disc=False)[1][0][2] != AV_NOPTS_VALUE, "FFmpeg's reader keeps it"


def test_only_all_zero_sectors_are_empty():
    # the reference skips a sector whose byte 4 is 0 and that is all 0x00;
    # its all-0xff branch can never match (byte 4 would be 0xff), so an
    # all-0xff sector is read as a pack and fails
    stream = b"\x00" * SECTOR + sector(pes(0xE0, SAMPLES))
    assert payloads(stream) == (1, [(0x1E0, SAMPLES, 90000)])
    assert payloads(b"\xff" * SECTOR)[0] < 0


def test_a_sector_holds_at_most_two_packets_unless_one_is_mpeg_audio():
    two = sector(pes(0xE0, SAMPLES), private1(0x80, b"", SAMPLES))
    assert payloads(two)[0] == 2
    three = sector(pes(0xE0, SAMPLES), private1(0x80, b"", SAMPLES), private1(0x81, b"", SAMPLES))
    assert payloads(three)[0] < 0
    # MPEG audio on stream_id 0xD0-0xDF lifts the limit
    with_mpa = sector(pes(0xE0, SAMPLES), pes(0xD0, SAMPLES), private1(0x80, b"", SAMPLES))
    assert payloads(with_mpa)[0] == 3


def test_bytes_that_are_no_packet_after_video_drop_the_rest_of_the_sector():
    junk = b"\x12\x34\x56\x78" + b"\x9a" * 20
    first = PACK + pes(0xE0, SAMPLES) + junk
    first += b"\xff" * (SECTOR - len(first))
    stream = first + sector(pes(0xE0, SAMPLES, pts=180000))
    assert payloads(stream) == (2, [(0x1E0, SAMPLES, 90000), (0x1E0, SAMPLES, 180000)])
    # after an MPEG audio packet the same bytes fail
    bad = PACK + pes(0xC0, SAMPLES) + junk
    bad += b"\xff" * (SECTOR - len(bad))
    assert payloads(bad)[0] < 0


def test_a_first_packet_running_past_the_sector_leaves_it_out_or_fails():
    big = SAMPLES * 30                          # 2880 bytes: past the sector's end
    short = (PACK + pes(0xE0, big))[:SECTOR]
    stream = short + sector(pes(0xE0, SAMPLES, pts=180000))
    assert payloads(stream) == (1, [(0x1E0, SAMPLES, 180000)]), "a short header: the sector is left out"
    long_header = (PACK + pes(0xE0, big, stuffing=40))[:SECTOR]
    assert payloads(long_header)[0] < 0, "a long header: fails"


def test_a_failed_sector_stays_failed():
    three = sector(pes(0xE0, SAMPLES), private1(0x80, b"", SAMPLES), private1(0x81, b"", SAMPLES))
    ret, pkts = payloads(three + sector(pes(0xE0, SAMPLES)))
    assert ret < 0
    assert pkts == [], "nothing after the failure"


def test_an_audio_payload_is_split_at_its_first_access_unit():
    # the payload starts with the end of a frame that began earlier; the
    # first access unit pointer says where the next frame starts: the bytes
    # before it and from it leave as two packets, the time with the second
    tail, frame = b"\x55" * 50, b"\x0b\x77" + bytes(98)
    ret, got = payloads(sector(private1(0x80, b"", tail + frame, fau=51)))
    assert (ret, got) == (2, [(0x80, tail, AV_NOPTS_VALUE), (0x80, frame, 90000)])
    # a first access unit at the payload's start, or none: one packet
    assert payloads(sector(private1(0x80, b"", frame, fau=1))) == (1, [(0x80, frame, 90000)])
    assert payloads(sector(private1(0x80, b"", tail, nf=0, fau=0))) == (1, [(0x80, tail, 90000)])
    # ordinary program streams are read as before: one packet
    assert payloads(sector(private1(0x80, b"", tail + frame, fau=51)), disc=False)[0] == 1
