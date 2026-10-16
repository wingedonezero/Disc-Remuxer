"""LATM / LOAS stream values from the header (latm_rule.c dr_latm_header):
the StreamMuxConfig's AudioSpecificConfig gives the output sample rate and
the samples per frame as FFmpeg's LATM decoder sets them up (ISO/IEC
14496-3): AAC LC 1024 or 960 samples, ER LD 512 / 480, SBR signalled at
twice the core rate (explicitly, or backward-compatibly after the config
when its length is stated) twice the samples at that rate. Frames are built
bit by bit; FFmpeg's encoder's test stream is checked against what FFmpeg's
decoder says of it."""

import json
import subprocess

from helpers import DATA, PROGRAM, ffi, lib

RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350]


class Bits:
    def __init__(self):
        self.v, self.n = 0, 0

    def put(self, value, n):
        self.v = (self.v << n) | (value & ((1 << n) - 1))
        self.n += n
        return self

    def value(self, v):                                 # LatmGetValue
        nb = max(1, (v.bit_length() + 7) // 8)
        return self.put(nb - 1, 2).put(v, 8 * nb)

    def bytes(self):
        return (self.v << (-self.n % 8)).to_bytes((self.n + 7) // 8, "big")


def asc(aot, rate_idx, chans=2, flf=0, sbr=None, tail=None):
    """An AudioSpecificConfig: explicit SBR as object type 5 / 29 with its
    extension rate when sbr = (5 or 29, ext rate index)."""
    b = Bits()
    if sbr:
        b.put(sbr[0], 5).put(rate_idx, 4).put(chans, 4).put(sbr[1], 4).put(aot, 5)
    else:
        b.put(aot, 5).put(rate_idx, 4).put(chans, 4)
    b.put(flf, 1).put(0, 1).put(0, 1)                   # frameLengthFlag, dependsOnCoreCoder, extensionFlag
    if tail:
        tail(b)
    return b


def loas(conf, version=0, same=0, programs=0):
    b = Bits().put(same, 1)
    if not same:
        b.put(version, 1)
        if version:
            b.put(0, 1).value(0)                        # audioMuxVersionA, taraBufferFullness
        b.put(1, 1).put(0, 6).put(programs, 4).put(0, 3)
        if version:
            b.value(conf.n)
        b.put(conf.v, conf.n)
        b.put(0, 3).put(0xFF, 8).put(0, 1).put(0, 1)    # frameLengthType, latmBufferFullness, other data, crc
    body = b.bytes() + bytes(20)
    return bytes([0x56, 0xE0 | (len(body) >> 8), len(body) & 0xFF]) + body


def values(frame):
    rate, samples = ffi.new("int *"), ffi.new("int *")
    if lib.dr_latm_header(frame, len(frame), rate, samples) < 0:
        return None
    return rate[0], samples[0]


def test_aac_lc_frames_and_their_lengths():
    assert values(loas(asc(2, 3))) == (48000, 1024)
    assert values(loas(asc(2, 3, flf=1))) == (48000, 960)
    assert values(loas(asc(2, 4))) == (44100, 1024)
    assert values(loas(asc(23, 3))) == (48000, 512)     # ER AAC LD
    assert values(loas(asc(23, 3, flf=1))) == (48000, 480)


def test_sbr_at_twice_the_core_rate_doubles_the_samples():
    assert values(loas(asc(2, 6, sbr=(5, 3)))) == (48000, 2048)      # HE-AAC: 24 kHz core, 48 kHz out
    assert values(loas(asc(2, 6, chans=1, sbr=(29, 3)))) == (48000, 2048)   # HE-AAC v2 (PS)
    assert values(loas(asc(2, 3, sbr=(5, 3)))) == (48000, 1024)      # SBR at the core rate: no doubling


def test_a_backward_compatible_sbr_signal_counts_when_the_config_length_is_stated():
    def sync_ext(b, ext_idx=3):
        b.put(0x2B7, 11).put(5, 5).put(1, 1).put(ext_idx, 4)
    assert values(loas(asc(2, 6, tail=sync_ext), version=1)) == (48000, 2048)
    assert values(loas(asc(2, 6, tail=lambda b: sync_ext(b, 6)), version=1)) == (24000, 1024)
    # without a stated length (audioMuxVersion 0) the library does not look for it
    assert values(loas(asc(2, 6, tail=sync_ext))) == (24000, 1024)


def test_frames_without_a_usable_config_give_no_values():
    assert values(loas(asc(2, 3), same=1)) is None      # useSameStreamMux: no config in the frame
    assert values(loas(asc(2, 3), programs=1)) is None   # two programs
    assert values(loas(asc(8, 3))) is None               # CELP: not timed here
    assert values(b"\x56\xe0") is None


def test_the_encoders_test_stream_has_the_values_the_decoder_gives():
    data = (DATA / "sine_11520.latm").read_bytes()
    first = data[:((data[1] & 0x1F) << 8 | data[2]) + 3]
    ffprobe = PROGRAM[0].rsplit("/", 1)[0] + "/ffprobe"
    p = subprocess.run([ffprobe, "-v", "error", "-f", "loas", "-show_frames", "-read_intervals", "%+#2",
                        "-show_entries", "frame=nb_samples:stream=sample_rate", "-of", "json",
                        str(DATA / "sine_11520.latm")], capture_output=True, text=True)
    j = json.loads(p.stdout)
    decoded = (int(j["streams"][0]["sample_rate"]), j["frames"][0]["nb_samples"])
    assert values(first) == decoded == (48000, 1024)
