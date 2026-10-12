"""MPEG audio and AAC in the title timeline (disctl_codecs.c). MPEG audio frames
last their own samples, are never sync units, and any MPEG audio track is
reviewed (user rule). AAC ADTS frames last 1024 samples per raw block and
are sync units when they carry a channel configuration or a program config
element; frames with the MPEG-4 ID count the same and are reviewed. LATM
takes its values from FFmpeg's decoder on the frame that carries the
StreamMuxConfig and is reviewed. Streams made by FFmpeg's encoders."""

from helpers import DATA, lib
from helpers.disctl import dur, run

MP2 = (DATA / "sine_11520.mp2").read_bytes()
ADTS = (DATA / "sine_11520.aac").read_bytes()
LATM = (DATA / "sine_11520.latm").read_bytes()
F_KEY, F_SYNC = lib.DR_F_KEY, lib.DR_F_SYNC


def adts_frames(s):
    """ADTS frames (13-bit frame length of each header): (start, length)."""
    out, i = [], 0
    while i < len(s):
        n = ((s[i + 3] & 3) << 11) | (s[i + 4] << 3) | (s[i + 5] >> 5)
        out.append((i, n))
        i += n
    return out


def header(r):
    return (r.audio.header.rate, r.audio.header.samples)


def test_mp2_frames_last_1152_samples_are_not_sync_units_and_are_reviewed():
    r = run("mp2", [(MP2, 3)])
    assert len(r.outs) == 10
    assert all(len(o.bytes) == 576 and o.dur == dur(1152, 48000) and o.flags == F_KEY for o in r.outs)
    assert header(r) == (48000, 1152)
    assert r.audio.review > 0


def test_adts_mpeg4_id_frames_are_kept_timed_sync_units_and_reviewed():
    r = run("aac", [(ADTS, 3)])
    assert len(r.outs) == len(adts_frames(ADTS))
    assert all(o.dur == dur(1024, 48000) and o.flags == F_KEY | F_SYNC for o in r.outs)
    assert r.audio.review > 0


def test_adts_mpeg2_id_frames_are_sync_units():
    s = bytearray(ADTS)
    for at, _ in adts_frames(ADTS):
        s[at + 1] |= 0x08  # ID = MPEG-2 (no CRC in these frames)
    r = run("aac", [(bytes(s), 3)])
    assert all(o.dur == dur(1024, 48000) and o.flags == F_KEY | F_SYNC for o in r.outs)
    assert header(r) == (48000, 1024)
    assert r.audio.review == 0


def test_latm_takes_its_values_from_the_frame_with_the_stream_mux_config():
    r = run("aac_latm", [(LATM, 3)])
    assert len(r.outs) == 13
    assert header(r) == (48000, 1024)
    assert all(o.dur == dur(1024, 48000) for o in r.outs)
    assert r.outs[0].flags == F_KEY | F_SYNC
    assert all(o.flags == F_KEY for o in r.outs[1:]), "useSameStreamMux frames are not sync units"
    assert r.audio.review > 0


# ---- the disc reader's MPEG audio frame rules (mpegaudio_parser.c disc mode) ----

# bits of bit allocation per subband, Layer II table B.2a (48 kHz, 56-80+ kbit/s per channel)
NBAL_A = [4] * 11 + [3] * 12 + [2] * 4


def bits(data, start, n):
    out = []
    for i in range(start, start + n):
        out.append(data[i // 8] >> (7 - i % 8) & 1)
    return out


def crc16(bitlist):
    crc = 0xFFFF
    for b in bitlist:
        top = crc >> 15 & 1
        crc = (crc << 1) & 0xFFFF
        if top != b:
            crc ^= 0x8005
    return crc


def with_crc(frame):
    """The frame with a CRC (ISO/IEC 11172-3 2.4.3.1: header bits 16-31, then
    the bit allocation and the scale factor selection information); the
    frame keeps its length (its last two bytes, padding, give way)."""
    body = bytes([frame[0], frame[1] & 0xFE]) + frame[2:4]
    side = frame[4:]
    pos, alloc = 0, []
    for nb in NBAL_A:                          # stereo, bound 32: one allocation per channel
        a = []
        for _ in range(2):
            a.append(int("".join(map(str, bits(side, pos, nb))) or "0", 2))
            pos += nb
        alloc.append(a)
    covered = pos + 2 * sum(1 for a in alloc for v in a if v)
    crc = crc16(bits(body, 16, 16) + bits(side, 0, covered))
    return body + crc.to_bytes(2, "big") + side[:-2]


def mp2_frames():
    return [MP2[i:i + 576] for i in range(0, len(MP2), 576)]


def test_junk_between_frames_is_skipped_to_the_next_frame_start():
    f = mp2_frames()
    r = run("mp2", [(b"".join(f[:4]) + b"\x12\x34" * 50 + b"".join(f[4:]), 3)])
    assert [o.bytes for o in r.outs] == f
    assert r.frames.skipped_bytes == 100


def test_a_header_whose_crc_is_wrong_is_no_frame_start():
    f = [with_crc(x) for x in mp2_frames()]
    fake = bytearray(f[1][:8])
    fake[4] ^= 0xFF                             # its CRC no longer fits
    r = run("mp2", [(b"".join(f[:4]) + b"\x55" * 7 + bytes(fake) + b"".join(f[4:]), 3)])
    assert [o.bytes for o in r.outs] == f, "the fake header skipped with the junk"
    assert r.frames.skipped_bytes == 15


def test_free_format_frames_take_the_distance_to_the_matching_headers():
    f = []
    for x in mp2_frames():
        f.append(x[:2] + bytes([x[2] & 0x0F]) + x[3:])     # bitrate index 0: free format
    r = run("mp2", [(b"".join(f), 3)])
    assert [o.bytes for o in r.outs] == f


def test_on_a_dvd_dual_channel_mpeg_audio_is_no_frame():
    f = [x[:3] + bytes([(x[3] & 0x3F) | 0x80]) + x[4:] for x in mp2_frames()]  # mode: dual channel
    assert len(run("mp2", [(b"".join(f), 3)]).outs) == 10
    assert len(run("mp2", [(b"".join(f), 3)], flags=lib.DR_AUDIO_DVD).outs) == 0


# ---- the disc reader's ADTS / LOAS frame rules (aac_parser.c / latm_parser.c disc mode) ----

def loas_frames(s):
    out, i = [], 0
    while i + 3 <= len(s):
        n = (((s[i + 1] & 0x1F) << 8) | s[i + 2]) + 3
        out.append(s[i:i + n])
        i += n
    return out


def test_an_adts_header_whose_next_header_is_not_there_is_no_frame_start():
    f = [ADTS[a:a + n] for a, n in adts_frames(ADTS)]
    fake = bytearray(f[2][:7])                 # a copy of a header in the junk: its frame_length leads nowhere
    junk = b"\x11" * 5 + bytes(fake) + b"\x22" * 9
    r = run("aac", [(b"".join(f[:2]) + junk + b"".join(f[2:]), 3)])
    assert [o.bytes for o in r.outs] == f
    assert r.frames.skipped_bytes == len(junk)


def test_junk_between_loas_frames_is_skipped_to_the_next_confirmed_sync():
    f = loas_frames(LATM)
    # junk with a sync whose length points at no next sync (a sync right where a
    # frame is expected would be taken: the reference's AudioMuxElement check is not done)
    junk = b"\x33" * 4 + b"\x56\xe0\x05" + b"\x33" * 6
    r = run("aac_latm", [(b"".join(f[:3]) + junk + b"".join(f[3:]), 3)])
    assert [o.bytes for o in r.outs] == f
    assert r.frames.skipped_bytes == len(junk)
