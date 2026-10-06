"""libavformat/discio_fs.c: the file system a disc image is read through
(ff_discio_mount_image), the DVD-Video check, the label copy rule and the
disc format probe, on images built here byte by byte: ISO 9660 / Joliet
trees (ECMA-119) with or without a UDF volume (ECMA-167 / OSTA UDF) over
the same sectors, as on DVD-Video bridge discs. With
DISCIO_FS_REFERENCE=<file> (tab-separated: id, image path, disc format,
file system, label), every listed image must give the recorded choice with
either UDF reader."""

import os

import pytest

from helpers import EINVAL, ENOENT, averror, error_text, ffi, lib
from helpers.disc import Image, env_or_skip
from synth.image import (BUP, VMG, VTS1, Img, Udf, bridge, broken_files, dvd_files, iso,
                         udf102)

INVALIDDATA = -0x41444E49  # FFERRTAG('I','N','D','A')


def chosen(im):
    v = Image.memory(im).mount()
    return v.name(), v.label()


def opts(udf_reader, force):
    return (udf_reader, int(force))


# ---- the choice with a UDF volume ----

def test_a_udf_volume_of_another_revision_is_kept_without_the_check():
    u = Udf("UDF_DISC_LABEL", 0x0150, 2004, broken_files())
    im = bridge(u, dvd_files())
    v = Image.memory(im).mount()
    assert (v.name(), v.kind(), v.label()) == ("UDF (NetBSD)", lib.DISCIO_FS_UDF, "UDF_DISC_LABEL")
    assert not v.check(), "the UDF tree fails the check it was not given"


def test_a_recent_valid_udf_102_volume_is_kept():
    im = bridge(udf102(2009, dvd_files()), dvd_files())
    assert chosen(im) == ("UDF (NetBSD)", "UDF_DISC_LABEL")


def test_an_old_udf_102_volume_prefers_iso_9660_with_the_udf_label():
    im = bridge(udf102(2004, dvd_files()), dvd_files())
    v = Image.memory(im).mount()
    assert (v.name(), v.kind(), v.label()) == ("ISO 9660", lib.DISCIO_FS_ISO9660, "UDF_DISC_LABEL")
    # 2005 is still before 2006; 2006 is not.
    assert chosen(bridge(udf102(2005, dvd_files()), dvd_files()))[0] == "ISO 9660"
    assert chosen(bridge(udf102(2006, dvd_files()), dvd_files()))[0] == "UDF (NetBSD)"


def test_the_option_off_keeps_an_old_valid_udf_102_volume():
    im = bridge(udf102(2004, dvd_files()), dvd_files())
    assert Image.memory(im).mount(opts(lib.DISCIO_UDF_NETBSD, False)).name() == "UDF (NetBSD)"


def test_a_udf_102_volume_failing_the_check_gives_way_to_iso_9660():
    im = bridge(udf102(2009, broken_files()), dvd_files())
    assert chosen(im) == ("ISO 9660", "UDF_DISC_LABEL")
    # The option off changes nothing here: the UDF volume was checked.
    assert Image.memory(im).mount(opts(lib.DISCIO_UDF_NETBSD, False)).name() == "ISO 9660"


def test_joliet_comes_after_iso_9660():
    im = bridge(udf102(2004, dvd_files()), broken_files(), dvd_files())
    assert chosen(im) == ("Joliet", "UDF_DISC_LABEL")
    assert Image.memory(im).mount().kind() == lib.DISCIO_FS_JOLIET


def test_the_udf_volume_is_kept_unchecked_when_nothing_else_passes():
    im = bridge(udf102(2004, broken_files()), broken_files(), broken_files())
    assert chosen(im) == ("UDF (NetBSD)", "UDF_DISC_LABEL")


def test_a_short_udf_label_leaves_the_iso_9660_label():
    # Over "ISO_LABEL" (9 bytes, half = 4): "UDF_DISC_LABEL" (14) and "UDF_"
    # (4) are copied, "UDF" (3) is not.
    for udf_label, want in [("UDF_DISC_LABEL", "UDF_DISC_LABEL"), ("UDF_", "UDF_"), ("UDF", "ISO_LABEL")]:
        u = udf102(2004, dvd_files())
        u.label = udf_label
        im = bridge(u, dvd_files())
        assert chosen(im) == ("ISO 9660", want), f"UDF label {udf_label!r}"


def test_the_linux_based_reader_is_chosen_by_the_option():
    linux = opts(lib.DISCIO_UDF_LINUX, True)
    im = bridge(udf102(2009, dvd_files()), dvd_files())
    v = Image.memory(im).mount(linux)
    assert (v.name(), v.kind(), v.label()) == ("UDF (Linux)", lib.DISCIO_FS_UDF, "UDF_DISC_LABEL")
    im = bridge(udf102(2004, dvd_files()), dvd_files())
    v = Image.memory(im).mount(linux)
    assert (v.name(), v.label()) == ("ISO 9660", "UDF_DISC_LABEL")
    im = bridge(udf102(2009, broken_files()), dvd_files())
    assert Image.memory(im).mount(linux).name() == "ISO 9660"


def test_an_unknown_udf_reader_is_refused():
    im = bridge(udf102(2009, dvd_files()), dvd_files())
    assert Image.memory(im).mount(opts(7, True)) == averror(EINVAL)


# ---- the choice without a UDF volume ----

def test_without_udf_a_valid_iso_9660_volume_is_used():
    assert chosen(bridge(None, dvd_files(), dvd_files())) == ("ISO 9660", "ISO_LABEL")


def test_without_udf_joliet_is_used_when_only_it_passes():
    assert chosen(bridge(None, broken_files(), dvd_files())) == ("Joliet", "JOLIET_LABEL")


def test_without_udf_iso_9660_is_kept_unchecked_when_nothing_passes():
    assert chosen(bridge(None, broken_files(), broken_files())) == ("ISO 9660", "ISO_LABEL")
    assert chosen(bridge(None, broken_files()))[0] == "ISO 9660"


def test_no_file_system_at_all():
    assert Image.memory(Img()).mount() == INVALIDDATA


# ---- the DVD-Video check ----

def iso_only(files):
    im = bridge(None, files)
    im.vmg(VMG, b"DVDVIDEO-VMG", 1)
    return im


def check_iso(im):
    return Image.memory(im).mount_one("iso").check()


def test_dvd_video_check_rules():
    assert check_iso(iso_only(dvd_files()))
    # No VIDEO_TS directory: not DVD-Video, passes.
    im = Img()
    iso(im, "ISO_LABEL", [("OTHER", [])])
    assert check_iso(im)
    # VIDEO_TS.IFO missing; empty (zero sectors).
    assert not check_iso(iso_only([("VTS_01_0.IFO", VTS1, 4096)]))
    assert not check_iso(iso_only([("VIDEO_TS.IFO", VMG, 0), ("VTS_01_0.IFO", VTS1, 4096)]))
    # Wrong identifier.
    im = iso_only(dvd_files())
    im.vmg(VMG, b"DVDVIDEO-VTS", 1)
    assert not check_iso(im)
    # Title set counts 0 and 100 are out of range; 2 needs VTS_02_0.IFO.
    for n in [0, 100, 2]:
        im = iso_only(dvd_files())
        im.vmg(VMG, b"DVDVIDEO-VMG", n)
        assert not check_iso(im), f"{n} title sets"
    im = iso_only([("VIDEO_TS.IFO", VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096), ("VTS_02_0.IFO", VTS1, 2048)])
    im.vmg(VMG, b"DVDVIDEO-VMG", 2)
    assert check_iso(im)
    # VTS IFO of zero sectors.
    assert not check_iso(iso_only([("VIDEO_TS.IFO", VMG, 4096), ("VTS_01_0.IFO", VTS1, 0)]))
    # VIDEO_TS.IFO unreadable: VIDEO_TS.BUP is read instead, and checked the
    # same way.
    im = iso_only(dvd_files())
    im.bad.append(VMG)
    assert check_iso(im)
    im.vmg(BUP, b"XXXXXXXXXXXX", 1)
    assert not check_iso(im), "the backup copy is checked the same way"
    # Unreadable and no separate backup: fails.
    im = iso_only([("VIDEO_TS.IFO", VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)])
    im.bad.append(VMG)
    assert not check_iso(im)
    # A backup at the same sector as the IFO is no second chance.
    im = iso_only([("VIDEO_TS.IFO", VMG, 4096), ("VIDEO_TS.BUP", VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)])
    im.bad.append(VMG)
    assert not check_iso(im)


def test_dvd_video_check_reads_through_every_reader():
    im = bridge(udf102(2009, dvd_files()), dvd_files(), dvd_files())
    for kind in ["iso", "joliet", "netbsd", "linux"]:
        assert Image.memory(im).mount_one(kind).check(), kind
    im = bridge(udf102(2009, broken_files()), broken_files(), broken_files())
    for kind in ["iso", "joliet", "netbsd", "linux"]:
        assert not Image.memory(im).mount_one(kind).check(), kind


def test_every_reader_finds_directories_only():
    im = bridge(udf102(2009, dvd_files()), dvd_files(), dvd_files())
    for kind in ["iso", "joliet", "netbsd", "linux"]:
        v = Image.memory(im).mount_one(kind)
        assert v.find_dir("/") == 0, kind
        assert v.find_dir("/VIDEO_TS") == 0, kind
        assert v.find_dir("/NOPE") == averror(ENOENT), kind
        assert v.find_dir("/VIDEO_TS/VIDEO_TS.IFO") == averror(ENOENT), f"{kind}: a file is not a directory"
        assert v.find_dir("/NOPE/VIDEO_TS") == averror(ENOENT), kind


# ---- the label copy rule ----

def copied(dst, src):
    d = ffi.new("char[]", lib.DISCIO_LABEL_SIZE)
    ffi.memmove(d, dst, len(dst))
    lib.ff_discio_label_copy(d, src)
    return ffi.string(d)


def test_label_copied_when_at_least_half_as_long():
    assert copied(b"LABYRINTH_1", b"U") == b"LABYRINTH_1"
    assert copied(b"LABYRINTH_1", b"UDFLB") == b"UDFLB", "5 >= 11 / 2"
    assert copied(b"LABYRINTH_1", b"UDFL") == b"LABYRINTH_1"
    assert copied(b"", b"") == b""
    assert copied(b"ISO", b"") == b"ISO", "0 < 3 / 2"
    assert copied(b"IS", b"U") == b"U", "1 >= 2 / 2"


def test_long_labels_are_kept_whole():
    # Decision for this program: a label is never emptied for its length.
    # UDF labels decode to at most 258 bytes; all of the room is usable.
    for n in [160, 161, 200, 258, 259]:
        assert copied(b"", b"A" * n) == b"A" * n, f"{n} bytes"
    # 54 three-byte characters (162 bytes), the shortest non-Latin label a
    # 161-byte room would empty.
    s = ("あ" * 54).encode()
    assert copied(b"", s) == s
    # Past the room: cut at a whole character.
    assert copied(b"", ("あ" * 90).encode()) == ("あ" * 86).encode(), "258 of 270 bytes"


def test_labels_are_cut_at_the_first_invalid_sequence():
    # A lone surrogate encoded on its own (ED A0 80) is not valid UTF-8.
    assert copied(b"", b"AB\xed\xa0\x80C") == b"AB"
    assert copied(b"", b"A\xc3") == b"A", "a lead byte at the end"
    assert copied(b"", b"A\xe9B") == b"A", "a Latin-1 byte"
    assert copied(b"", "Ä€😀".encode()) == "Ä€😀".encode()
    assert copied(b"", b"A\xc0\x80") == b"A", "overlong"
    assert copied(b"", b"A\xf8\x88\x80\x80\x80") == b"A", "5-byte form"
    assert copied(b"", b"A\xf4\x90\x80\x80") == b"A", "above U+10FFFF"
    assert copied(b"", b"A\xf4\x8f\xbf\xbf") == b"A\xf4\x8f\xbf\xbf", "U+10FFFF"
    # After ED / F4 only the upper limit of the second byte is checked.
    assert copied(b"", b"A\xed\x61\xb0") == b"A\xed\x61\xb0"


# ---- the disc format ----

def format_of(dirs):
    im = Img()
    iso(im, "ISO_LABEL", dirs)
    return Image.memory(im).mount_one("iso").format()


def test_disc_format_from_the_files_present():
    def f(name):
        return (name, 50, 100)
    assert format_of([("BDMV", [f("index.bdmv"), f("MovieObject.bdmv")])]) == lib.DISCIO_DISC_BLURAY
    assert format_of([("BDMV", [f("INDEX.BDM"), f("MOVIEOBJ.BDM")])]) == lib.DISCIO_DISC_BLURAY
    assert format_of([("BDMV", [f("index.bdmv")])]) == lib.DISCIO_DISC_NONE, "index alone"
    assert format_of([("BDAV", [f("info.bdav")])]) == lib.DISCIO_DISC_BLURAY
    assert format_of([("ADV_OBJ", [f("DISCID.DAT")]), ("HVDVD_TS", [f("HVA00001.VTI")])]) == lib.DISCIO_DISC_HDDVD
    assert format_of([("ADV_OBJ", [f("DISCID.DAT")]), ("HDDVD_TS", [f("HVA00001.VTI")])]) == lib.DISCIO_DISC_HDDVD
    assert format_of([("HVDVD_TS", [f("HVA00001.VTI")]), ("VIDEO_TS", [f("VIDEO_TS.IFO")])]) == \
        lib.DISCIO_DISC_DVD, "an HD DVD title set without DISCID.DAT"
    assert format_of([("VIDEO_TS", [f("VIDEO_TS.IFO")])]) == lib.DISCIO_DISC_DVD
    assert format_of([("BDMV", [f("index.bdmv"), f("MovieObject.bdmv")]), ("VIDEO_TS", [f("VIDEO_TS.IFO")])]) == \
        lib.DISCIO_DISC_BLURAY, "Blu-ray first"
    assert format_of([("VIDEO_TS", [f("VTS_01_0.IFO")])]) == lib.DISCIO_DISC_NONE


# ---- real discs against the reference record ----

@pytest.mark.disc
def test_corpus_choice_matches_the_reference():
    reference = env_or_skip("DISCIO_FS_REFERENCE")
    ok, skipped, bad = 0, 0, []
    for line in open(reference).read().splitlines():
        if not line or line.startswith("#"):
            continue
        ident, path, fmt, fs, label = line.split("\t")
        if not os.path.exists(path):
            print(f"SKIP {ident}: {path} not found")
            skipped += 1
            continue
        for reader, udf in [("NetBSD", lib.DISCIO_UDF_NETBSD), ("Linux", lib.DISCIO_UDF_LINUX)]:
            v = Image.open(path).mount(opts(udf, True))
            assert not isinstance(v, int), f"{ident}: {error_text(v)}"
            got_fs = {lib.DISCIO_FS_UDF: "UDF", lib.DISCIO_FS_ISO9660: "ISO 9660"}.get(v.kind(), "Joliet")
            got_format = {lib.DISCIO_DISC_DVD: "dvd", lib.DISCIO_DISC_BLURAY: "bluray",
                          lib.DISCIO_DISC_HDDVD: "hddvd"}.get(v.format(), "none")
            got = (got_format, got_fs, v.label())
            good = got == (fmt, fs, label)
            print(f"{'OK ' if good else 'BAD'} {ident} ({reader}-based UDF reader): {got}")
            if good:
                ok += 1
            else:
                bad.append(f"{ident} ({reader}): got {got}, want {(fmt, fs, label)}")
    print(f"{ok} OK, {len(bad)} BAD, {skipped} skipped")
    assert not bad, bad
    assert ok > 0, "no image of the reference was checked"
