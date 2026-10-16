"""The WAV header of a PCM stream file (writer.c writer_wav_header): as
always for a file that cannot pass 4 GiB; for one whose title is long
enough that it may (from 3.5 GiB of sound expected), a 36-byte JUNK chunk
after "WAVE" keeps room, and a file that does pass 4 GiB is RF64 (EBU Tech
3306): "RF64", the ds64 chunk in the JUNK chunk's place with the 64-bit
RIFF size, data size and sample frames, the 32-bit sizes 0xFFFFFFFF.
FFmpeg reads such a file to its full length (a sparse 4.5 GiB file)."""

import json
import struct
import subprocess

from helpers import PROGRAM, ffi, lib

GIB = 1 << 30


def header(channels, bits, data, room, rate=48000, mask=0x3F):
    out = ffi.new("uint8_t[]", lib.WAV_HEADER_MAX)
    n = lib.writer_wav_header(out, channels, rate, bits, bits, mask, data, room)
    return bytes(ffi.buffer(out, n))


def le(b, at, n):
    return int.from_bytes(b[at:at + n], "little")


def test_a_file_that_cannot_pass_4_gib_keeps_the_plain_header():
    h = header(2, 16, 1000, 0)
    assert len(h) == 44 and h[:4] == b"RIFF" and le(h, 4, 4) == 36 + 1000 and h[36:40] == b"data"
    h = header(6, 24, 1000, 0)
    assert len(h) == 68 and h[:4] == b"RIFF" and le(h, 64, 4) == 1000


def test_room_kept_and_not_needed_is_a_junk_chunk_in_a_riff_file():
    h = header(6, 24, 3 * GIB, 1)
    assert len(h) == 68 + 36
    assert h[:4] == b"RIFF" and le(h, 4, 4) == len(h) - 8 + 3 * GIB and h[8:12] == b"WAVE"
    assert h[12:16] == b"JUNK" and le(h, 16, 4) == 28 and h[20:48] == bytes(28)
    assert h[48:52] == b"fmt " and h[-8:-4] == b"data" and le(h, len(h) - 4, 4) == 3 * GIB


def test_a_file_past_4_gib_with_room_is_rf64():
    data = 5 * GIB - 6 * 3 * 7                       # whole sample frames of 6 channels, 24 bits
    h = header(6, 24, data, 1)
    assert h[:4] == b"RF64" and le(h, 4, 4) == 0xFFFFFFFF and h[8:12] == b"WAVE"
    assert h[12:16] == b"ds64" and le(h, 16, 4) == 28
    assert (le(h, 20, 8), le(h, 28, 8), le(h, 36, 8), le(h, 44, 4)) == (len(h) - 8 + data, data, data // 18, 0)
    assert h[-8:-4] == b"data" and le(h, len(h) - 4, 4) == 0xFFFFFFFF


def test_a_file_past_4_gib_without_room_keeps_its_sizes_capped():
    h = header(6, 24, 5 * GIB, 0)
    assert h[:4] == b"RIFF" and le(h, 4, 4) == 0xFFFFFFFF and le(h, len(h) - 4, 4) == 0xFFFFFFFF


def test_the_processing_library_reads_an_rf64_file_to_its_end(tmp_path):
    rate, ch = 48000, 6
    data = 4 * GIB + 512 * (1 << 20)
    data -= data % (ch * 3)
    h = header(ch, 24, data, 1, rate)
    f = tmp_path / "big.wav"
    with open(f, "wb") as o:                         # sparse: the sound is zeros
        o.write(h)
        o.truncate(len(h) + data)
    ffprobe = PROGRAM[0].rsplit("/", 1)[0] + "/ffprobe"
    p = subprocess.run([ffprobe, "-v", "error", "-show_entries", "format=duration,format_name", "-of", "json",
                        str(f)], capture_output=True, text=True)
    fmt = json.loads(p.stdout)["format"]
    assert abs(float(fmt["duration"]) - data / (rate * ch * 3)) < 0.01, (fmt, p.stderr)
    assert p.stderr == "", p.stderr                  # no "file may be invalid"
