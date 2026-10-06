"""libavformat/discrip_ps.c: the disc formats' copy of FFmpeg's MPEG-PS
demuxer. A private stream 1 sub-stream 0xA0-0xAF is always linear PCM with
its LPCM header kept in the payload (FFmpeg's mpeg.c guesses MLP from the
header's dynamic range byte and then cuts 6 more bytes); the other
sub-streams keep FFmpeg's header cuts. The streams are built byte by byte."""

import struct

from helpers import lib
from helpers.discrip import codec_id
from helpers.parser import ps_read

PACK = bytes([0x00, 0x00, 0x01, 0xBA, 0x44, 0x00, 0x04, 0x00, 0x04, 0x01, 0x01, 0x89, 0xC3, 0xF8])


def pts_bytes(pts):
    return bytes([0x21 | ((pts >> 29) & 0x0E), (pts >> 22) & 0xFF, 0x01 | ((pts >> 14) & 0xFE),
                  (pts >> 7) & 0xFF, 0x01 | ((pts << 1) & 0xFE)])


def pes(stream_id, payload, pts=90000):
    """A PES packet with a PTS (MPEG-2 header)."""
    body = bytes([0x81, 0x80, 0x05]) + pts_bytes(pts) + payload
    return bytes([0x00, 0x00, 0x01, stream_id]) + struct.pack(">H", len(body)) + body


def private1(sub, header, data, pts=90000):
    """A pack with one private stream 1 packet: sub-stream id, the 3 bytes of
    frame count and first access unit pointer, a codec header, the data."""
    return PACK + pes(0xBD, bytes([sub, 0x01, 0x00, 0x01]) + header + data, pts)


SAMPLES = bytes(range(1, 97))


def test_lpcm_is_always_lpcm_with_its_header_kept():
    # DVD-Video 3-byte LPCM header and HD DVD 5-byte header, each with the
    # usual dynamic range byte 0x80 and with another value
    for header in (bytes([0x00, 0x01, 0x80]), bytes([0x00, 0x01, 0x23]),
                   bytes([0x00, 0x01, 0x80, 0x00, 0x01]), bytes([0x00, 0x01, 0x55, 0x00, 0x01])):
        ret, pkts = ps_read(private1(0xA0, header, SAMPLES) * 3)
        assert ret == 3, header.hex()
        assert all((p[0], p[1], p[2]) == (0xA0, codec_id("pcm_dvd"), header + SAMPLES) for p in pkts), header.hex()


def test_ffmpeg_s_reader_cuts_lpcm_when_the_dynamic_range_byte_is_not_0x80():
    # what the copy fixes: upstream takes the stream for MLP and cuts 6 bytes
    _, pkts = ps_read(private1(0xA0, bytes([0x00, 0x01, 0x23]), SAMPLES), ours=False)
    assert (pkts[0][1], pkts[0][2]) == (codec_id("mlp"), (bytes([0x00, 0x01, 0x23]) + SAMPLES)[6:])
    _, pkts = ps_read(private1(0xA0, bytes([0x00, 0x01, 0x80]), SAMPLES), ours=False)
    assert (pkts[0][1], pkts[0][2]) == (codec_id("pcm_dvd"), bytes([0x00, 0x01, 0x80]) + SAMPLES)


def test_the_other_sub_streams_keep_ffmpeg_s_header_cuts():
    cases = [
        (0x20, b"", "dvd_subtitle"),                     # sub-pictures: the sub-stream id only
        (0x80, b"", "ac3"),                              # AC-3: id + 3
        (0x88, b"", "dts"),                              # DTS: id + 3
        (0xC0, b"", "ac3"),                              # Dolby Digital Plus: id + 3 (E-AC-3 decided later)
        (0xB0, bytes([0x07]), "truehd"),                 # TrueHD: id + 4
    ]
    for sub, extra, codec in cases:
        stream = private1(sub, extra, SAMPLES)
        ours, theirs = ps_read(stream), ps_read(stream, ours=False)
        assert ours == theirs, hex(sub)
        want = SAMPLES if sub != 0x20 else bytes([0x01, 0x00, 0x01]) + SAMPLES
        assert (ours[1][0][1], ours[1][0][2]) == (codec_id(codec), want), hex(sub)
    # MPEG audio is its own stream id (0xC0 + n), video 0xE0
    stream = PACK + pes(0xC0, SAMPLES) + PACK + pes(0xE0, SAMPLES)
    assert ps_read(stream) == ps_read(stream, ours=False)
    assert [p[0] for p in ps_read(stream)[1]] == [0x1C0, 0x1E0]
