"""The MKV file's read-back (setting mkv.read_back): each track's content is
summed in memory as its stream file is written (src/analyze/content_sum.c)
and the finished MKV file is read back and summed the same way
(mkv_readback.c); the writer compares the two, says where bytes changed,
and makes the mux once more when the stream files are as written
(src/disc-remuxer/writer.c)."""

import os
import shutil
import subprocess

import pytest

from helpers import DATA, PROGRAM, ffi, lib
from test_mkv_writer import avc_lpcm_disc

H264, AC3, TRUEHD, PGS, MPEG2 = 27, 86019, 86060, 94214, 2      # the library's codec ids
THD = (DATA / "sine_4813.thd").read_bytes()
A52 = (DATA / "sine_7680.ac3").read_bytes()
HAVE_MKVMERGE = shutil.which("mkvmerge") is not None


def summed(kind, pieces=(), nals=()):
    s = ffi.new("DrContentSum **")
    assert lib.dr_content_sum_open(s, kind) == 0
    for p in pieces:
        lib.dr_content_sum_data(s[0], p, len(p))
    for n in nals:
        lib.dr_content_sum_nal(s[0], n, len(n))
    r = ffi.new("DrContentResult *")
    lib.dr_content_sum_finish(s[0], r)
    lib.dr_content_sum_close(s)
    return ffi.string(r.digest).decode(), r.bytes, r.units, r.odd


def test_only_codecs_whose_bytes_a_muxer_was_measured_to_keep_have_a_form():
    assert lib.dr_content_kind(H264, 0) == lib.DR_CONTENT_NAL
    assert lib.dr_content_kind(TRUEHD, 0) == lib.dr_content_kind(PGS, 0) == lib.DR_CONTENT_BYTES
    assert lib.dr_content_kind(AC3, 0) == lib.DR_CONTENT_BYTES
    assert lib.dr_content_kind(AC3, 1) == lib.DR_CONTENT_AC3_NO_DIALNORM
    assert lib.dr_content_kind(MPEG2, 0) == lib.DR_CONTENT_NONE, "not measured: its track is said as not compared"


def test_nal_units_sum_the_same_from_a_byte_stream_and_from_units():
    nals = [b"\x09\x10", b"\x67\x64\x00\x29\xac", b"\x68\xee\x3c\x80", b"\x65\x88\x00\x00\x03\x00\x01\xff" * 50,
            b"\x41\x9a\x00\x00\x03\x00\x00\x03\x01\x23"]
    want = summed(lib.DR_CONTENT_NAL, nals=nals)
    assert want[2] == 3 and want[1] == sum(len(n) for n in nals) and not want[3], "3 units and 2 parameter sets"
    # 3- and 4-byte start codes, zero bytes after a unit: not the units' own
    stream = b"\x00\x00\x00\x01" + nals[0] + b"\x00\x00\x01" + nals[1] + b"\x00\x00\x00\x00\x01" + nals[2] + \
             b"\x00\x00\x01" + nals[3] + b"\x00\x00\x00\x01" + nals[4] + b"\x00\x00"
    assert summed(lib.DR_CONTENT_NAL, [stream]) == want
    # cut into pieces at every place, also inside a start code and inside a run of zero bytes
    for cut in range(1, len(stream), 7):
        assert summed(lib.DR_CONTENT_NAL, [stream[:cut], stream[cut:]]) == want, cut
    assert summed(lib.DR_CONTENT_NAL, [bytes([b]) for b in stream]) == want, "byte by byte"
    # one changed bit, and the same bytes cut into other units, are other sums
    bad = bytearray(stream)
    bad[40] ^= 0x08
    assert summed(lib.DR_CONTENT_NAL, [bytes(bad)])[0] != want[0]
    assert summed(lib.DR_CONTENT_NAL, nals=[nals[0] + nals[1]] + nals[2:])[0] != want[0]
    assert summed(lib.DR_CONTENT_NAL, [b"\xaa" + stream])[3] == 1, "bytes before the first start code: not a byte stream"


def test_a_parameter_set_counts_once_wherever_and_however_often_it_stands():
    aud, sps, pps, sei = b"\x09\x10", b"\x67\x64\x00\x29\xac\x2c", b"\x68\xee\x3c\x80", b"\x06\x01\x02\x03\x80"
    idr, p = b"\x65\x88\x84\x00\x21\xff" * 30, b"\x41\x9a\x24\x6c\x41\x4f" * 20
    # as on a disc: the second key picture comes without its parameter sets
    disc = [aud, sps, pps, sei, idr, aud, p, aud, sei, idr, aud, p]
    want = summed(lib.DR_CONTENT_NAL, nals=disc)
    assert want[2] == 10 and want[1] == sum(len(n) for n in disc) and not want[3]
    # as a muxer stores it: the sets once more in front of that key picture
    muxed = [aud, sps, pps, sei, idr, aud, p, aud, sps, pps, sei, idr, aud, p]
    assert summed(lib.DR_CONTENT_NAL, nals=muxed) == want
    assert summed(lib.DR_CONTENT_NAL, [b"".join(b"\x00\x00\x01" + n for n in muxed)]) == want
    # a changed bit in one of the copies is a set the stream did not have
    bad = bytes([sps[0], sps[1], sps[2] ^ 0x10]) + sps[3:]
    assert summed(lib.DR_CONTENT_NAL, nals=muxed[:8] + [bad] + muxed[9:])[0] != want[0]
    assert summed(lib.DR_CONTENT_NAL, nals=[aud, bad] + muxed[2:])[0] != want[0]
    # a set that is missing, and the other units in another order, are other sums
    assert summed(lib.DR_CONTENT_NAL, nals=[n for n in disc if n != pps])[0] != want[0]
    assert summed(lib.DR_CONTENT_NAL, nals=[aud, sps, pps, idr, sei] + disc[5:])[0] != want[0]
    # two sets in the other order: the same sets
    assert summed(lib.DR_CONTENT_NAL, nals=[aud, pps, sps] + disc[3:]) == want


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = (crc << 1 ^ 0x8005 if crc & 0x8000 else crc << 1) & 0xFFFF
    return crc


def test_ac3_frames_sum_the_same_once_their_dialogue_normalization_is_removed():
    frames = [A52[i:i + 768] for i in range(0, len(A52), 768)]
    assert len(frames) == 5 and all(f[:2] == b"\x0b\x77" for f in frames)
    want = summed(lib.DR_CONTENT_AC3_NO_DIALNORM, frames)
    assert want[2] == 5 and not want[3]
    assert summed(lib.DR_CONTENT_AC3_NO_DIALNORM, [A52]) == want, "any number of frames per call"
    assert summed(lib.DR_CONTENT_AC3_NO_DIALNORM, [A52[:1000], A52[1000:1001], A52[1001:]]) == want
    # what a muxer changes when it removes the dialogue normalization: the value (stereo: 5 bits from bit 54),
    # crc1, crc2 and the bit reserved next to it
    changed = []
    for f in frames:
        f = bytearray(f)
        assert f[6] >> 5 == 2, "2/0: the value follows dsurmod and lfeon"
        v = int.from_bytes(f[6:8], "big") | 0x03E0          # bits 54..58 set: -31 dB
        f[6:8] = v.to_bytes(2, "big")
        f[2:4] = b"\x12\x34"
        f[-3] ^= 1
        f[-2:] = b"\xab\xcd"
        changed.append(bytes(f))
    assert summed(lib.DR_CONTENT_AC3_NO_DIALNORM, changed) == want
    assert summed(lib.DR_CONTENT_BYTES, changed)[0] != summed(lib.DR_CONTENT_BYTES, frames)[0]
    # any other changed bit is another sum
    bad = bytearray(A52)
    bad[2000] ^= 0x08
    assert summed(lib.DR_CONTENT_AC3_NO_DIALNORM, [bytes(bad)])[0] != want[0]
    assert summed(lib.DR_CONTENT_AC3_NO_DIALNORM, [A52[:-5]])[3] == 1, "a stream that ends inside a frame"
    assert summed(lib.DR_CONTENT_AC3_NO_DIALNORM, [b"\x00" * 800])[3] == 1, "no AC-3 frame"


def read_back(path, kinds):
    tr = ffi.new("DrMkvTrack[]", len(kinds))
    for i, k in enumerate(kinds):
        tr[i].kind = k
    why = ffi.new("char[256]")
    ret = lib.dr_mkv_read_back(str(path).encode(), tr, len(kinds), why, 256)
    return ret, [(ffi.string(t.got.digest).decode(), t.got.bytes, t.got.odd) for t in tr], ffi.string(why).decode()


@pytest.mark.skipif(not HAVE_MKVMERGE, reason="mkvmerge is not installed")
def test_a_matroska_file_read_back_holds_each_tracks_bytes_and_a_changed_bit_shows_in_its_track(tmp_path):
    (tmp_path / "a.thd").write_bytes(THD)
    (tmp_path / "b.ac3").write_bytes(A52 * 40)
    mkv = tmp_path / "t.mkv"
    subprocess.run(["mkvmerge", "-q", "-o", str(mkv), str(tmp_path / "a.thd"), str(tmp_path / "b.ac3")], check=True)
    want = [summed(lib.DR_CONTENT_BYTES, [THD]), summed(lib.DR_CONTENT_BYTES, [A52 * 40])]
    ret, got, why = read_back(mkv, [lib.DR_CONTENT_BYTES, lib.DR_CONTENT_BYTES])
    assert ret == 0 and [g[:2] for g in got] == [w[:2] for w in want], why
    # a track that is not asked for is not read
    ret, got, _ = read_back(mkv, [lib.DR_CONTENT_NONE, lib.DR_CONTENT_BYTES])
    assert ret == 0 and got[0][0] == "" and got[1][:2] == want[1][:2]
    # one bit inside the AC-3 track's data: that track's sum differs, the other's does not
    data = bytearray(mkv.read_bytes())
    at = data.rindex(A52[300:400])
    data[at + 50] ^= 0x08
    mkv.write_bytes(data)
    ret, got, _ = read_back(mkv, [lib.DR_CONTENT_BYTES, lib.DR_CONTENT_BYTES])
    assert ret == 0 and got[0][:2] == want[0][:2] and got[1][0] != want[1][0] and got[1][1] == want[1][1]
    assert read_back(mkv, [lib.DR_CONTENT_BYTES])[0] < 0, "another number of tracks than was given"


def demux(tmp_path, name, *opts, **env):
    disc, _ = avc_lpcm_disc(tmp_path / name)
    out = tmp_path / name / "out"
    p = subprocess.run([*PROGRAM, "--set", "output.format=mkvmerge", "--set", "titles.min_length=0", *opts, "demux",
                        str(disc), "0", str(out)], capture_output=True, text=True, env={**os.environ, **env})
    return p, out


@pytest.mark.skipif(not HAVE_MKVMERGE, reason="mkvmerge is not installed")
def test_the_mkv_file_is_read_back_and_a_damaged_mux_is_made_once_more(tmp_path):
    # as made: both tracks (H.264, LPCM) hold what was written
    p, out = demux(tmp_path, "a", DR_MKV_NO_VARIABLE_DATA="1")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "MKV read back from the drive: 2 of 2 tracks compared with what was written: PASS" in p.stdout
    assert "the MKV file read back: PASS)" in p.stdout and "WARNING" not in p.stdout
    good = (out / "title_t00.mkv").read_bytes()
    # bits changed in the first mux only: said, the mux made once more, the second file is right
    p, out = demux(tmp_path, "b", DR_TEST_MKV_DAMAGE="1", DR_MKV_NO_VARIABLE_DATA="1")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "its stream file read from the drive is as written" in p.stdout
    assert "MKV read-back: the mux is made once more" in p.stdout
    assert "the second MKV file holds what was written: the first mux left damaged data" in p.stdout
    assert "done with" in p.stdout and "the MKV file read back: PASS)" in p.stdout
    assert not (out / "title_t00").exists(), "the stream files are deleted once the file is right"
    assert (out / "title_t00.mkv").read_bytes() == good, "the file kept is the second mux's, as an undamaged run's"
    # bits changed in both muxes, elsewhere each time: the title fails, the stream files and the file are kept
    p, out = demux(tmp_path, "c", DR_TEST_MKV_DAMAGE="2")
    assert p.returncode != 0
    assert "MKV read-back: FAIL: two muxes in a row left damaged data in the MKV file" in p.stdout
    assert "the MKV file made from them does not hold what was written" in p.stdout
    assert not (out / "title_t00.mkv").exists() and (out / "title_t00_DAMAGED.mkv").exists()
    kept = sorted(f.name for f in (out / "title_t00").iterdir())
    assert any(f.endswith(".h264") for f in kept) and any(f.endswith(".wav") for f in kept), kept
    # the same other bytes in both muxes with the stream files as written: a muxer that stores the track in
    # another form than the check knows, not damage: said, not compared, the title is written
    p, out = demux(tmp_path, "d", DR_TEST_MKV_DAMAGE="2", DR_TEST_MKV_DAMAGE_SAME="1")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "both muxes hold the same bytes, other than those written" in p.stdout
    assert "[untested on real discs]" in p.stdout and (out / "title_t00.mkv").exists()


@pytest.mark.skipif(not HAVE_MKVMERGE, reason="mkvmerge is not installed")
def test_the_read_back_can_be_turned_off(tmp_path):
    p, out = demux(tmp_path, "a", "--set", "mkv.read_back=false", DR_TEST_MKV_DAMAGE="2")
    assert p.returncode == 0, p.stdout + p.stderr
    assert "MKV read" not in p.stdout
    assert "the MKV file itself is not read back (setting mkv.read_back is off)" in p.stdout
