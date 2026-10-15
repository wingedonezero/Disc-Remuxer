"""Blu-ray structure (src/disc/bluray/bluray_*.c): disc folders built here byte
by byte (tests/synth/bluray.py), read with ff_bluray_disc_open and checked
through the structure dump: the file names and BACKUP copies, playlists and
clip information with the reference program's rules, index and id, the disc
name (disc library, BDAV info.bdav, AVCHD card files), recorded text and the
title clean-up. With BLURAY_FOLDERS (folders, comma-separated) and the
cross-check tool's output it can be compared with another reader by hand
(implementation notes)."""

import pytest

from helpers import ffi, install_log, lib, logged
from synth import bluray as bd
from synth.bluray import Item, Stream, SubPath, be16, be32


def dump(path, lang=None, min_length=-1):
    """The structure dump (with the titles when min_length >= 0), or the error."""
    err = ffi.new("int *")
    p = lib.tb_bluray_dump(str(path).encode(), lang.encode() if lang else ffi.NULL, min_length, err)
    if p == ffi.NULL:
        return err[0]
    try:
        return ffi.string(p).decode()
    finally:
        lib.free(p)


def lines(text, prefix):
    return [l for l in text.splitlines() if l.startswith(prefix)]


def head(text):
    return text.splitlines()[0]


def build(tmp_path, files):
    return bd.write(tmp_path / "disc", files)


# ---- a BDMV disc ----

def test_a_disc_with_its_clips_playlists_index_and_id(tmp_path):
    items = [Item("00001", 0, 90000, audio=[Stream(0x1100, 0x81, b"\x61eng")], pg=[Stream(0x1200, 0x90, b"eng")]),
             Item("00002", 1000, 46000, connection=6)]
    files = bd.bdmv({1: bd.clpi(), 2: bd.clpi(packets=2)},
                    {800: bd.mpls(items, subpaths=[SubPath()], marks=[(1, 0, 0), (1, 1, 1000)])},
                    {"CERTIFICATE/id.bdmv": bd.disc_id(),
                     "BDMV/META/DL/bdmt_eng.xml": bd.bdmt("A Film - Blu-ray"),
                     "BDMV/JAR/00000.jar": b"PK", "BDMV/BDJO/00000.bdjo": b"BDJO0200"})
    t = dump(build(tmp_path, files))
    assert head(t) == "disc BDMV name 'A Film' lang 'eng' english 'A Film' clips 2 playlists 1 jars 1 bdjo 1"
    assert "index version 0200 first_play 1/0 top_menu 1/1 titles 1" in t
    assert "id org 7fff1234 disc 000102030405060708090a0b0c0d0e0f" in t
    assert sorted(l.split()[1] for l in lines(t, "clip ")) == ["00001", "00002"]
    assert "playlist 00800 version 0200 playback 1 items 2 subpaths 1 marks 2 ext 0 ext_subpath 65535" in t
    assert "  item 0 clip 00001 codec M2TS in 0 out 90000 connection 1 stc 0 angles 0 audio 1 pg 1 ig 0 ss 0" in t
    assert "  item 1 clip 00002 codec M2TS in 1000 out 46000 connection 6 stc 0 angles 0 audio 0 pg 0 ig 0 ss 0" in t
    assert "    audio entry 1 len 8 [11 00 00 00 00 00 00 00] coding 0x81 len 4 [61 65 6e 67]" in t
    assert "    subitem 0 clip 00002 codec M2TS in 0 out 45000 sync_item 0 sync_pts 0" in t
    assert lines(t, "  mark") == ["  mark 0 type 1 item 0 time 0 pid 0xffff duration 0",
                                  "  mark 1 type 1 item 1 time 1000 pid 0xffff duration 0"]


def test_bd_plus_is_said_in_the_standard_log(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    t = dump(build(tmp_path / "plain", bd.bdmv()))
    assert head(t).startswith("disc BDMV name")
    assert not logged("BD+")
    t = dump(build(tmp_path / "bdplus", bd.bdmv(extra={"BDSVM/00000.svm": b"BDSVM_PK"})))
    assert head(t).startswith("disc BDMV BD+ name")
    assert logged("Blu-ray: the disc uses BD+ (/BDSVM/00000.svm), which is not supported: unless this copy "
                  "was made with BD+ removed, parts of the video may be damaged")


def test_without_clip_information_the_disc_cannot_be_read(tmp_path):
    assert dump(build(tmp_path, bd.bdmv(clips={}))) < 0


def test_8_3_names_are_read_and_sorted_when_the_long_names_are_missing(tmp_path):
    files = {"BDMV/INDEX.BDM": bd.index(), "BDMV/MOVIEOBJ.BDM": bd.header("MOBJ")}
    for n in (3, 1, 2):
        files[f"BDMV/CLIPINF/{n:05d}.CPI"] = bd.clpi()
        files[f"BDMV/STREAM/{n:05d}.MTS"] = bytes(192)
        files[f"BDMV/PLAYLIST/{n:05d}.MPL"] = bd.mpls([Item(f"{n:05d}")])
    t = dump(build(tmp_path, files))
    assert [l.split()[1] for l in lines(t, "clip ")] == ["00001", "00002", "00003"]
    assert [l.split()[1] for l in lines(t, "playlist ")] == ["00001", "00002", "00003"]
    assert "file /BDMV/STREAM/00001.MTS 192" in t
    assert "index version 0200" in t


def test_a_backup_copy_stands_in_for_a_missing_file(tmp_path):
    files = bd.bdmv(playlists={})
    files["BDMV/BACKUP/PLAYLIST/00005.mpls"] = bd.mpls([Item()])
    del files["BDMV/index.bdmv"]
    files["BDMV/BACKUP/index.bdmv"] = bd.index(titles=((1, 0), (1, 1)))
    files["BDMV/PLAYLIST/00005.mpls"] = b""          # listed, but empty: not a playlist
    t = dump(build(tmp_path, files))
    # the main copy exists (and is read): its BACKUP copy is not used
    assert lines(t, "playlist ") == []
    assert "titles 2" in t, "a missing index is read from its backup copy"


def test_numbered_names_need_five_digits_and_the_exact_suffix(tmp_path):
    files = bd.bdmv()
    files["BDMV/CLIPINF/0001a.clpi"] = bd.clpi()
    files["BDMV/CLIPINF/00002.clpi.bak"] = bd.clpi()
    files["BDMV/CLIPINF/000003.clpi"] = bd.clpi()
    t = dump(build(tmp_path, files))
    assert [l.split()[1] for l in lines(t, "clip ")] == ["00001"]


# ---- playlists ----

@pytest.mark.parametrize("why, pl", [
    ("two primary video streams", bd.mpls([Item(videos=[Stream(), Stream(0x1012)])])),
    ("no primary video stream", bd.mpls([Item(videos=[])])),
    ("not a playlist", b"XPLS0200" + bytes(64)),
    ("AppInfoPlayList too short", b"MPLS0200" + be32(0x40) + bytes(28) + be32(1) + bytes(64)),
])
def test_a_playlist_the_rules_refuse_is_left_out(tmp_path, why, pl):
    t = dump(build(tmp_path, bd.bdmv(playlists={1: bd.mpls([Item()]), 2: pl})))
    assert [l.split()[1] for l in lines(t, "playlist ")] == ["00001"], why


def test_a_playitem_ending_past_its_playlist_block_refuses_the_playlist(tmp_path):
    good = bd.mpls([Item()])
    p = int.from_bytes(good[8:12], "big")
    bad = bytearray(good)
    bad[p:p + 4] = be32(int.from_bytes(good[p:p + 4], "big") - 4)   # the block ends 4 bytes early
    t = dump(build(tmp_path, bd.bdmv(playlists={1: bytes(bad)})))
    assert lines(t, "playlist ") == []


def test_angles_and_the_3d_extension(tmp_path):
    item = Item("00001", pg=[Stream(0x1200, 0x90, b"eng"), Stream(0x1201, 0x90, b"fra")],
                angles=[("00002", 1), ("00003", 2)])
    dep = Stream(0x1012, 0x20, b"\x61").bytes()
    rec0 = bytes([0, 0])                                  # no SS entries
    rec1 = bytes([0, 0x01]) + bytes([9, 1]) + be16(0x1220) + bytes(6) + bytes([0, 7])   # one SS entry
    blk = bytes([0x80, 0]) + dep + bytes([0, 3]) + rec0 + rec1
    stn_ss = be16(len(blk)) + blk
    t = dump(build(tmp_path, bd.bdmv(playlists={1: bd.mpls([item], ext=[(2, 1, stn_ss)])})))
    assert "angles 3 audio 0 pg 2 ig 0 ss 1" in t
    assert "    angle 2 clip 00002 codec M2TS stc 1" in t
    assert "    angle 3 clip 00003 codec M2TS stc 2" in t
    assert "ext 1" in t


def test_a_3d_extension_that_does_not_tile_its_entry_refuses_the_playlist(tmp_path):
    dep = Stream(0x1012, 0x20, b"\x61").bytes()
    blk = bytes([0x80, 0]) + dep + bytes([0, 3])
    stn_ss = be16(len(blk)) + blk + bytes(2)             # 2 bytes left over
    t = dump(build(tmp_path, bd.bdmv(playlists={1: bd.mpls([Item()], ext=[(2, 1, stn_ss)])})))
    assert lines(t, "playlist ") == []


def test_the_subpath_extension_gives_its_first_subpath(tmp_path):
    sp = SubPath(type=8).bytes()
    ext = bytes(4) + be16(2) + sp + sp
    t = dump(build(tmp_path, bd.bdmv(playlists={1: bd.mpls([Item()], ext=[(2, 2, ext)])})))
    assert "ext_subpath 8" in t


# ---- clip information ----

def test_clip_rules(tmp_path):
    clips = {
        1: bd.clpi(),
        2: bd.clpi(ts_info_len=4 + 0x1a),                              # length kept above 4: fine
        3: bd.clpi(stcs=((40, 0x1001, 0, 900),)),                      # first STC start packet set to 0
        4: bd.clpi(atcs=[]),                                           # no ATC sequence: left out
        5: bd.header("XXXX") + bytes(0x100),                           # not clip information
    }
    t = dump(build(tmp_path, bd.bdmv(clips=clips)))
    assert sorted(l.split()[1] for l in lines(t, "clip ")) == ["00001", "00002", "00003"]
    i = t.index("clip 00003")
    assert "  atc 0 stc 0 pcr_pid 0x1001 spn 0 start 0 end 900" in t[i:]


def test_unaligned_stc_starts_are_moved_back_to_an_aligned_unit(tmp_path):
    c = bd.clpi(stcs=((0, 0x1001, 0, 900), (70, 0x1001, 1000, 2000)))
    t = dump(build(tmp_path, bd.bdmv(clips={1: c})))
    assert "  atc 0 stc 1 pcr_pid 0x1001 spn 64 start 1000 end 2000" in t


def test_further_atc_sequences_are_dropped_on_bdmv_clips(tmp_path):
    c = bd.clpi(atcs=[(0, 0, [(0, 0x1001, 0, 900)]), (500, 1, [(512, 0x1001, 1000, 2000)])])
    t = dump(build(tmp_path, bd.bdmv(clips={1: c})))
    assert "atc 1" not in t


def test_a_clip_without_its_stream_file_is_left_out(tmp_path):
    files = bd.bdmv(clips={1: bd.clpi(), 2: bd.clpi()})
    del files["BDMV/STREAM/00002.m2ts"]
    t = dump(build(tmp_path, files))
    assert [l.split()[1] for l in lines(t, "clip ")] == ["00001"]


def test_a_tiny_ts_type_info_refuses_the_clip(tmp_path):
    good = bytearray(bd.clpi())
    good[0x28 + 0x94:0x28 + 0x96] = be16(4)
    t = dump(build(tmp_path, bd.bdmv(clips={1: bd.clpi(), 2: bytes(good)})))
    assert [l.split()[1] for l in lines(t, "clip ")] == ["00001"]


def test_the_second_program_info_extension_replaces_the_first(tmp_path):
    prog2 = bytes([0, 1]) + be32(0) + be16(0x100) + bytes([1, 0]) + be16(0x1022) + bytes([2, 0x24, 0x61])
    prog2 = be32(len(prog2)) + prog2
    c = bd.clpi(ext=[(2, 5, prog2)])
    t = dump(build(tmp_path, bd.bdmv(clips={1: c})))
    assert "    pid 0x1022 coding 0x24 len 1 [61]" in t
    assert "pid 0x1011" not in t


# ---- names ----

def test_the_preferred_language_file_names_the_disc_and_english_is_kept(tmp_path):
    files = bd.bdmv(extra={"BDMV/META/DL/bdmt_eng.xml": bd.bdmt("English Name"),
                           "BDMV/META/DL/bdmt_fra.xml": bd.bdmt("Nom Français", "fre")})
    root = build(tmp_path, files)
    assert head(dump(root, "fra")).startswith("disc BDMV name 'Nom Français' lang 'fre' english 'English Name'")
    assert head(dump(root, "fr")).startswith("disc BDMV name 'Nom Français'"), "2- and 3-letter codes match"
    assert head(dump(root)).startswith("disc BDMV name 'English Name' lang 'eng' english 'English Name'")


def test_an_unparsable_disc_library_file_is_repaired_as_the_reference_does(tmp_path):
    thumb = b'<di:description><di:thumbnail href=cover.jpg></di:thumbnail></di:description>'
    xml = bd.bdmt("Repaired").replace(b"</di:discinfo>", thumb + b"</di:discinfo>")
    t = dump(build(tmp_path, bd.bdmv(extra={"BDMV/META/DL/bdmt_eng.xml": xml})))
    assert head(t).startswith("disc BDMV name 'Repaired'"), "href value quoted"
    # a value the repair stops at ('/') leaves the description to be blanked
    thumb = b'<di:description><di:thumbnail href=a/b.jpg></di:thumbnail></di:description>'
    xml = bd.bdmt("Blanked").replace(b"</di:discinfo>", thumb + b"</di:discinfo>")
    t = dump(build(tmp_path / "b", bd.bdmv(extra={"BDMV/META/DL/bdmt_eng.xml": xml})))
    assert head(t).startswith("disc BDMV name 'Blanked'"), "description blanked"


@pytest.mark.parametrize("raw, clean", [
    ("Movie, The", "Movie"),
    ("Movie - Blu-ray 3D", "Movie"),
    ("Movie™", "Movie"),
    ("Movie 4K Ultra HD", "Movie"),
    ("Movie Ultra HD", "Movie"),
    ("Movie – Extended", "Movie – Extended"),
    ("Blu-ray", None),
    ("Line one\nline two\r", "Line one line two"),
    ("", None),
])
def test_the_disc_library_title_is_cleaned(raw, clean):
    p = lib.tb_bluray_clean_title(raw.encode())
    got = None if p == ffi.NULL else ffi.string(p).decode()
    if p != ffi.NULL:
        lib.free(p)
    assert got == clean


@pytest.mark.parametrize("charset, raw, text", [
    (0x10, b"Rec 1", "Rec 1"),
    (0x11, b"Caf\xe9 \x80", "Café €"),
    (0x20, "Grüße".encode(), "Grüße"),
    (0x20, "Hi".encode("utf-16-be"), "Hi"),
    (0x20, b"\xff\xfe\x00", None),        # neither UTF-8 nor (odd length) UTF-16
    (0x01, b"\x1b\x24\x42", None),        # ARIB: not decoded
    (0x05, b"x", None),
])
def test_recorded_text(charset, raw, text):
    p = lib.tb_bluray_text(charset, raw, len(raw))
    got = None if p == ffi.NULL else ffi.string(p).decode()
    if p != ffi.NULL:
        lib.free(p)
    assert got == text


# ---- BDAV, AVCHD ----

def bdav_files(name=b"My Recording", charset=0x10, items=(("00001", 3, 0, 900),), ptype=1):
    return {"BDAV/info.bdav": bd.info_bdav(name, charset),
            "BDAV/CLIPINF/00001.clpi": bd.clpi(kind="M2TS"),
            "BDAV/STREAM/00001.m2ts": bytes(192),
            "BDAV/PLAYLIST/01001.rpls": bd.rpls(list(items), ptype)}


def test_a_bdav_disc_its_name_and_recorder_playlists(tmp_path):
    t = dump(build(tmp_path, bdav_files()))
    assert head(t) == "disc BDAV name 'My Recording' lang '' english '' clips 1 playlists 1 jars 0 bdjo 0"
    assert "clip 00001 type M2TS" in t and "stream_type 1 app_type 1" in t
    assert "file /BDAV/STREAM/00001.m2ts" in t
    assert "  item 0 clip 00001 codec M2TS in 0 out 900 connection 1 stc 3 angles 0 audio 0 pg 0" in t


def test_recorder_playlists_of_another_type_have_no_stc_id(tmp_path):
    t = dump(build(tmp_path, bdav_files(ptype=2)))
    assert "connection 1 stc 255" in t


def test_a_recorder_playlist_with_more_than_200_items_is_left_out(tmp_path):
    many = [("00001", 0, 0, 900)] * 201
    t = dump(build(tmp_path, bdav_files(items=many)))
    assert lines(t, "playlist ") == []
    t = dump(build(tmp_path / "b", bdav_files(items=[("00001", 0, 0, 900)] * 200)))
    assert "items 200" in t


def test_an_avchd_folder_takes_its_name_from_the_card(tmp_path):
    card = tmp_path / "card"
    bd.write(card / "PRIVATE" / "AVCHD", bd.bdmv())
    bd.write(card, {"MISC/AUTPRINT.MRK": b'[JOB]\r\nGEN CRT="CAMERA MODEL X"\r\n'})
    t = dump(card / "PRIVATE" / "AVCHD")
    assert head(t).startswith("disc BDMV name 'CAMERA MODEL X'")
    sony = tmp_path / "sony"
    bd.write(sony / "AVCHD", bd.bdmv())
    bd.write(sony, {".SonyVID": b""})
    assert head(dump(sony / "AVCHD")).startswith("disc BDMV name 'SONY_AVCHD'")


def test_a_disc_library_name_wins_over_the_card(tmp_path):
    card = tmp_path / "card"
    bd.write(card / "AVCHD", bd.bdmv(extra={"BDMV/META/DL/bdmt_eng.xml": bd.bdmt("Library Name")}))
    bd.write(card, {".SonyVID": b""})
    assert head(dump(card / "AVCHD")).startswith("disc BDMV name 'Library Name'")
