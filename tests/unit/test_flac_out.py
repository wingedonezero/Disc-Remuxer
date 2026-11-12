"""Lossless audio decoded and written as FLAC, cut between two samples
(src/convert/flac_out.c): a TrueHD stream made with this repository's
FFmpeg is decoded from its first unit; each part's FLAC file must hold
exactly the samples the ffmpeg program decodes from the whole stream between
the part's cuts, the parts joined must be the whole, and the strict decoder
must refuse damaged data."""

import hashlib
import subprocess

import pytest

from helpers import DIST, ffi, lib

TICKS = 1_080_000_000               # the title timeline's ticks per second
RATE = 48000
T = TICKS // RATE                   # one sample
FFMPEG = str(DIST / "ffmpeg")
TRUEHD = 86060                      # AV_CODEC_ID_TRUEHD
SAMPLES = 48000 + 1234              # (not a whole number of FLAC blocks or of units)


def run(*args, data=None):
    return subprocess.run([FFMPEG, "-v", "error", "-nostdin", *args], input=data, capture_output=True, check=True).stdout


@pytest.fixture(scope="module")
def stream(tmp_path_factory):
    """A 5.1 24-bit TrueHD stream (each channel its own tones and noise), its
    access units, and the samples the ffmpeg program decodes from it (s32le
    bytes, strict)."""
    d = tmp_path_factory.mktemp("flac_out")
    thd = d / "in.thd"
    exprs = "|".join(f"0.3*sin(2*PI*{220 * (c + 1)}*t)+0.2*sin(2*PI*{37 * (c + 3)}*t*t)+0.05*random({c})"
                     for c in range(6))
    run("-f", "lavfi", "-i", f"aevalsrc={exprs}:s={RATE}:c=5.1(side),atrim=end_sample={SAMPLES}",
        "-c:a", "truehd", "-strict", "-2", "-sample_fmt", "s32p", "-f", "truehd", str(thd))
    data = thd.read_bytes()
    units, at = [], 0
    while at < len(data):
        n = (int.from_bytes(data[at:at + 2], "big") & 0x0fff) * 2
        units.append(data[at:at + n])
        at += n
    pcm = run("-err_detect", "+crccheck+explode", "-xerror", "-f", "truehd", "-i", str(thd), "-f", "s32le", "-")
    assert len(pcm) == SAMPLES * 6 * 4 and len(units) == -(-SAMPLES // 40)
    return d, thd, units, pcm


def convert(path, units, bounds=(), part=0, times=None, pieces=()):
    """The units through the converter: its result (a dict), or its error code and words."""
    f = ffi.new("DrFlacOut **")
    b = ffi.new("int64_t[]", list(bounds)) if bounds else ffi.NULL
    assert lib.dr_flac_out_open(f, TRUEHD, str(path).encode(), b, len(bounds), part, TICKS) == 0
    try:
        for i, u in enumerate(units):
            ret = lib.dr_flac_out_unit(f[0], u, len(u), times[i] if times else i * 40 * T, i in pieces)
            if ret < 0:
                return ret, ffi.string(lib.dr_flac_out_error(f[0])).decode()
        r = ffi.new("DrFlacResult *")
        ret = lib.dr_flac_out_finish(f[0], r)
        if ret < 0:
            return ret, ffi.string(lib.dr_flac_out_error(f[0])).decode()
        out = {k: getattr(r, k) for k in ("rate", "channels", "bits", "flac_bits", "layout", "native_layout", "units",
                                          "decoded", "before", "kept", "after", "first", "odd_units", "block",
                                          "pieces", "restarts")}
        out["md5"] = ffi.string(r.md5).decode()
        out["ranges"] = [(r.ranges[i].samples, ffi.string(r.ranges[i].md5).decode()) for i in range(r.nb_ranges)]
        return out
    finally:
        lib.dr_flac_out_close(f)


def flac_samples(path):
    """A FLAC file's samples as the ffmpeg program decodes them (s32le bytes)."""
    return run("-i", str(path), "-f", "s32le", "-")


def canon_md5(s32):
    """The MD5 FLAC keeps of 24-bit samples: the upper three bytes of each, little-endian."""
    return hashlib.md5(b"".join(s32[i + 1:i + 4] for i in range(0, len(s32), 4))).hexdigest()


def read_back(path):
    n, hn = ffi.new("int64_t *"), ffi.new("int64_t *")
    md5, hmd5, why = ffi.new("char[33]"), ffi.new("char[33]"), ffi.new("char[200]")
    ret = lib.dr_flac_read_back(str(path).encode(), n, md5, hn, hmd5, why, 200)
    return ret, n[0], ffi.string(md5).decode(), hn[0], ffi.string(hmd5).decode(), ffi.string(why).decode()


def test_the_whole_stream_as_flac_is_the_ffmpeg_programs_decode(stream):
    d, thd, units, pcm = stream
    r = convert(d / "whole.flac", units)
    assert (r["rate"], r["channels"], r["bits"], r["flac_bits"]) == (RATE, 6, 24, 24)
    assert r["native_layout"] == 1 and r["block"] == 4608
    assert (r["units"], r["decoded"], r["before"], r["kept"], r["after"]) == (len(units), SAMPLES, 0, SAMPLES, 0)
    assert r["first"] == 0 and r["odd_units"] == 0
    assert flac_samples(d / "whole.flac") == pcm, "sample for sample"
    assert r["md5"] == canon_md5(pcm) and r["ranges"] == [(SAMPLES, r["md5"])]
    assert read_back(d / "whole.flac") == (0, SAMPLES, r["md5"], SAMPLES, r["md5"], ""), "the file and its header"


def test_the_file_is_the_one_the_ffmpeg_program_writes_with_the_same_settings(stream):
    d, thd, units, pcm = stream
    convert(d / "ours.flac", units)
    run("-err_detect", "+crccheck+explode", "-xerror", "-f", "truehd", "-i", str(thd), "-c:a", "flac",
        "-compression_level", "12", "-max_prediction_order", "12", "-map_metadata", "-1", "-fflags", "+bitexact",
        str(d / "program.flac"))
    assert (d / "ours.flac").read_bytes() == (d / "program.flac").read_bytes()


def test_each_part_gets_its_samples_and_the_parts_joined_are_the_whole(stream):
    d, thd, units, pcm = stream
    # two cuts: one inside a unit and between two samples, one exactly on a sample inside a unit
    a, b = 12345 * T + 7, 30011 * T
    na, nb = 12346, 30011               # the first sample at or after each cut
    frame = 6 * 4
    want = [pcm[:na * frame], pcm[na * frame:nb * frame], pcm[nb * frame:]]
    got = []
    for k, zero in enumerate((0, a, b)):
        # each part's reading: the units that start before its end, on its own clock
        last = len(units) if k == 2 else -(-((a, b)[k]) // (40 * T))
        r = convert(d / f"part{k}.flac", units[:last], bounds=(a - zero, b - zero), part=k,
                    times=[i * 40 * T - zero for i in range(last)])
        got.append(flac_samples(d / f"part{k}.flac"))
        assert got[k] == want[k], f"part {k + 1}"
        assert r["kept"] == len(want[k]) // frame and r["md5"] == canon_md5(want[k])
        after = 40 * last - (na, nb)[k] if k < 2 else 0
        assert r["before"] == (0, na, nb)[k] and r["after"] == after
        assert after < 40, "only the rest of the unit that holds the next cut"
        # the first sample: at or after the part's 0, less than one sample after it
        assert r["first"] == (0, na * T - a, 0)[k] and 0 <= r["first"] < T
        # what this reading decoded between the cuts, up to its own part
        assert [x for x in r["ranges"][:k + 1]] == [(len(w) // frame, canon_md5(w)) for w in want[:k + 1]]
        assert read_back(d / f"part{k}.flac")[:5] == (0, r["kept"], r["md5"], r["kept"], r["md5"])
    assert b"".join(got) == pcm, "nothing lost, nothing twice"


def test_a_part_without_a_sample_leaves_no_file(stream):
    d, thd, units, pcm = stream
    r = convert(d / "none.flac", units[:10], bounds=(10 ** 12,), part=1)
    assert r["kept"] == 0 and r["before"] == 400 and r["first"] == -(1 << 63)
    assert not (d / "none.flac").exists()


def test_a_unit_that_does_not_start_where_the_samples_end_is_counted(stream):
    d, thd, units, pcm = stream
    times = [i * 40 * T + (5 * T if i >= 100 else 0) for i in range(len(units))]
    r = convert(d / "odd.flac", units, times=times)
    assert r["odd_units"] == 1 and r["kept"] == SAMPLES, "the samples follow each other all the same"
    assert flac_samples(d / "odd.flac") == pcm


def test_damaged_data_is_refused(stream):
    d, thd, units, pcm = stream
    bad = list(units)
    u = bytearray(bad[200])
    u[len(u) // 2] ^= 0x10              # one bit inside a substream: its parity / checksum no longer fits
    bad[200] = bytes(u)
    ret, why = convert(d / "bad.flac", bad)
    assert ret < 0 and "TrueHD decoder refuses" in why


def test_a_new_piece_that_starts_with_a_major_sync_restarts_the_decoder(stream):
    d, thd, units, pcm = stream
    # two stream files played back to back: the second one's first restart
    # header checks samples the first file does not hold
    sync = [i for i, u in enumerate(units) if u[4:8] == b"\xf8\x72\x6f\xba"]
    cut, again = sync[3], sync[1]
    joined = units[:cut] + units[again:]
    ret, why = convert(d / "joined_on.flac", joined)
    assert ret < 0 and "TrueHD decoder refuses" in why, "decoded on as one stream: the check across the join fails"
    r = convert(d / "joined.flac", joined, pieces=(cut,))
    assert (r["pieces"], r["restarts"]) == (1, 1)
    assert flac_samples(d / "joined.flac") == pcm[:cut * 40 * 24] + pcm[again * 40 * 24:], "each piece as it decodes alone"
    # a piece that goes on inside the stream (no major sync): decoded on
    r = convert(d / "on.flac", units, pieces=(sync[2] + 1,))
    assert (r["pieces"], r["restarts"]) == (1, 0) and flac_samples(d / "on.flac") == pcm


# every layout the TrueHD encoder makes: the ones FLAC has an order of its own for, and the ones it has not
LAYOUTS = [("mono", 1, 1), ("stereo", 2, 1), ("2.1", 3, 0), ("3.0", 3, 1), ("3.1", 4, 0), ("4.0", 4, 0),
           ("4.1", 5, 0), ("5.0(side)", 5, 1), ("5.1(side)", 6, 1)]


@pytest.mark.parametrize("layout,channels,native", LAYOUTS)
def test_each_channel_stays_in_its_slot_and_the_layout_is_kept(tmp_path, layout, channels, native):
    # each channel its own tone, so a channel in another slot changes the samples
    thd = tmp_path / "in.thd"
    exprs = "|".join(f"0.4*sin(2*PI*{150 * (c + 1)}*t)" for c in range(channels))
    run("-f", "lavfi", "-i", f"aevalsrc={exprs}:s={RATE}:c={layout},atrim=end_sample=9600",
        "-c:a", "truehd", "-strict", "-2", "-sample_fmt", "s32p", "-f", "truehd", str(thd))
    data, units, at = thd.read_bytes(), [], 0
    while at < len(data):
        n = (int.from_bytes(data[at:at + 2], "big") & 0x0fff) * 2
        units.append(data[at:at + n])
        at += n
    r = convert(tmp_path / "out.flac", units)
    assert (r["channels"], r["native_layout"]) == (channels, native)
    pcm = run("-f", "truehd", "-i", str(thd), "-f", "s32le", "-")
    assert flac_samples(tmp_path / "out.flac") == pcm, "every channel in its slot, sample for sample"
    # the file is the ffmpeg program's, with the layout as a tag where FLAC has no order of its own for it
    run("-f", "truehd", "-i", str(thd), "-c:a", "flac", "-compression_level", "12", "-max_prediction_order", "12",
        "-map_metadata", "-1", "-fflags", "+bitexact", str(tmp_path / "program.flac"))
    ours = (tmp_path / "out.flac").read_bytes()
    assert ours == (tmp_path / "program.flac").read_bytes()
    assert (b"WAVEFORMATEXTENSIBLE_CHANNEL_MASK" in ours) == (not native)
    # read back, the file states the source's layout (side or back surrounds are one layout to FLAC)
    probe = subprocess.run([str(DIST / "ffprobe"), "-v", "error", "-show_entries", "stream=channel_layout", "-of",
                            "csv=p=0", str(tmp_path / "out.flac")], capture_output=True, text=True, check=True)
    assert probe.stdout.strip() == layout
