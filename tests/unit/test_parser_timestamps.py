"""Timestamps the AC-3 parser gives frames when an AC-3 header is split
across two packets.

MPEG systems rule (ISO/IEC 13818-1 2.4.3.7): a PES packet's PTS belongs to
the first access unit that starts in that packet. A frame that starts in one
packet and whose 7-byte header ends in the next must not get the next
packet's PTS (that one belongs to the frame after it). DVD-Video audio packs
carry such splits wherever the frame size and the pack payload size line up
that way."""

from helpers import AV_NOPTS_VALUE
from helpers.parser import run

FRAME = 768
PAYLOAD = 2016  # DVD AC-3 pack payload size
TICKS = 2880  # 1536 samples at 48 kHz in 90 kHz units


def ac3_frame():
    """One AC-3 frame: 48 kHz, 192 kbit/s (frmsizecod 0x14, 768 bytes), bsid
    8, 2/0 stereo; the rest of the frame is zero (the parser only reads
    headers)."""
    return bytes([0x0B, 0x77, 0x00, 0x00, 0x14, 0x40, 0x40]) + bytes(FRAME - 7)


def test_split_header_frame_does_not_take_the_next_packets_pts():
    # lead junk bytes before frame 0 shift frame starts against packet
    # boundaries so that headers are split (e.g. frame 13 starts 4 bytes before
    # the end of packet 4, counting from 0).
    lead = 92
    nb_frames = 60
    stream = bytes(lead) + ac3_frame() * nb_frames

    def frame_start(n):
        return lead + n * FRAME
    packets = [stream[i:i + PAYLOAD] for i in range(0, len(stream), PAYLOAD)]
    pts, split = [], []
    for k, p in enumerate(packets):
        start, end = k * PAYLOAD, k * PAYLOAD + len(p)
        # PTS of the first frame starting in this packet, if any.
        first = next((n for n in range(nb_frames) if start <= frame_start(n) < end), None)
        pts.append(AV_NOPTS_VALUE if first is None else first * TICKS)
        split += [n for n in range(nb_frames) if frame_start(n) < end < frame_start(n) + 7]
    assert split, "test layout has no split header"

    ret, frames = run("ac3", list(zip(packets, pts)))
    assert ret == 0
    # The parser returns the lead bytes as a first chunk, then the frames.
    got = [t for size, t, _ in frames if size == FRAME]
    assert len(got) == nb_frames
    for n, t in enumerate(got):
        assert t in (AV_NOPTS_VALUE, n * TICKS), f"frame {n} (split header: {n in split}) got pts {t}"
    for n in split:
        assert got[n] == AV_NOPTS_VALUE, f"split-header frame {n} must get no PTS of its own"
