"""Blu-ray menu names (src/disc/bluray/bluray_menu.c): a BD-J menu framework's
project data (BDMV/JAR/<dir>/bluray_project.bin, built here in both layouts
by tests/synth/bluray.py) names playlists; the named ones are listed first,
keep a title when they are one clip piece and carry their names. Every
doubt leaves the data unused with a warning: a file no layout reads to its
end, more than one such file, no entry for this disc, a playlist named
twice, a named playlist that does not last what the data says (a playlist
that plays a clip twice, a menu loop, is not checked). Only on discs with
BD-J titles. The real-disc check is by hand (libs/README.md)."""

from helpers import ffi, install_log, lib, logged
from synth import bluray as bd
from synth.bluray import Item

S, E = 27000000, 27000000 + 90000          # the built clips' STC sequence


def dump(path):
    err = ffi.new("int *")
    p = lib.tb_bluray_dump(str(path).encode(), ffi.NULL, 0, err)
    assert p != ffi.NULL, err[0]
    try:
        return ffi.string(p).decode()
    finally:
        lib.free(p)


def titles(text):
    return [l.split()[2] for l in text.splitlines() if l.startswith("title ")]


def menu(text):
    return [l for l in text.splitlines() if l.startswith("menu ")]


def part_item(clip, cc=1):
    return Item(clip, S + 45000, E, connection=cc)          # 1 s


PLAYLISTS = {1: bd.mpls([part_item("00001"), part_item("00002", 5)]),   # 2 s, a title of its own
             2: bd.mpls([part_item("00003")])}                          # 1 s, one clip piece
DISC_ID = bytes(range(16))
HEX_ID = DISC_ID.hex().upper()


def disc(tmp_path, project=None, bdj=True, playlists=None, more=None):
    extra = {"BDMV/index.bdmv": bd.index(((2, 1),) if bdj else ((1, 0),)),
             "CERTIFICATE/id.bdmv": bd.disc_id(did=DISC_ID)}
    if project is not None:
        extra["BDMV/JAR/00000/bluray_project.bin"] = project
    extra.update(more or {})
    files = bd.bdmv({n: bd.clpi() for n in (1, 2, 3)}, playlists or PLAYLISTS, extra)
    return bd.write(tmp_path / "disc", files)


def project(pls=(("Logo", 2, 1.0),), discs=None, **kw):
    return bd.menu_project("My Project", discs or [("Disc 1", HEX_ID, list(pls))], **kw)


def test_without_menu_data_the_one_clip_playlist_gives_a_clip_title(tmp_path):
    t = dump(disc(tmp_path))
    assert sorted(titles(t)) == ["00001.mpls", "00003.m2ts"]
    assert menu(t) == []


def test_named_playlists_come_first_keep_their_title_and_carry_their_names(tmp_path):
    install_log(lib.AV_LOG_INFO)
    t = dump(disc(tmp_path, project()))
    assert titles(t) == ["00002.mpls", "00001.mpls"], "named first; its one clip piece keeps a playlist title"
    assert menu(t) == ["menu 'My Project' named 1", "menu 00002 'Logo'"]
    assert logged("Blu-ray: the menu project data (BDMV/JAR/00000/bluray_project.bin, older layout, project "
                  "'My Project', disc 'Disc 1') names 1 playlist(s)")


def test_the_newer_layout_with_its_version(tmp_path):
    install_log(lib.AV_LOG_INFO)
    t = dump(disc(tmp_path, project((("Logo", 2, 1.0), ("Main", 1, 2.0)), v2=True, version=(7, 4, 8))))
    assert menu(t) == ["menu 'My Project' named 2", "menu 00001 'Main'", "menu 00002 'Logo'"] or \
        menu(t) == ["menu 'My Project' named 2", "menu 00002 'Logo'", "menu 00001 'Main'"]
    assert logged("newer layout")


def test_only_on_discs_with_bdj_titles(tmp_path):
    assert menu(dump(disc(tmp_path, project(), bdj=False))) == []


def test_a_file_no_layout_reads_to_its_end_is_not_used(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    data = project()
    for bad in (data[:-1], data + b"\0", data[:20]):
        t = dump(disc(tmp_path / str(len(bad)), bad))
        assert menu(t) == []
        assert sorted(titles(t)) == ["00001.mpls", "00003.m2ts"]
    assert logged("bluray_project.bin is not menu project data in a known layout: menu names not used")


def test_two_project_files_are_not_used(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    t = dump(disc(tmp_path, project(), more={"BDMV/JAR/00001/bluray_project.bin": project()}))
    assert menu(t) == []
    assert logged("which one the menu reads is not known: not used")


def test_the_disc_entry_is_chosen_by_the_disc_id(tmp_path):
    other = "0" * 32
    t = dump(disc(tmp_path, project(discs=[("Disc A", other, [("Wrong", 1, 2.0)]),
                                           ("Disc B", HEX_ID, [("Logo", 2, 1.0)])])))
    assert menu(t) == ["menu 'My Project' named 1", "menu 00002 'Logo'"]
    install_log(lib.AV_LOG_WARNING)
    t = dump(disc(tmp_path / "b", project(discs=[("Disc A", other, []), ("Disc B", other, [])])))
    assert menu(t) == []
    assert logged("the menu project data has 2 discs and none is this one (by its disc ID): menu names not used")


def test_a_playlist_named_twice_is_not_used(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    t = dump(disc(tmp_path, project((("Logo", 2, 1.0), ("Again", 2, 1.0)))))
    assert menu(t) == []
    assert logged("the menu project data names playlist 00002 twice ('Logo', 'Again'): menu names not used")


def test_a_named_playlist_of_another_length_is_not_used(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    t = dump(disc(tmp_path, project((("Logo", 2, 4.5),))))
    assert menu(t) == []
    assert logged("the menu project data gives playlist 00002 ('Logo') 4.5 s, the playlist lasts 1.0 s: the data "
                  "may not name the playlists it seems to: menu names not used")
    # a playlist that plays a clip twice (a menu loop: the data gives one pass) is not checked
    loop = {**PLAYLISTS, 3: bd.mpls([part_item("00003"), part_item("00003")])}
    t = dump(disc(tmp_path / "b", project((("Loop", 3, 1.0),)), playlists=loop))
    assert menu(t) == ["menu 'My Project' named 1", "menu 00003 'Loop'"]


def test_named_playlists_the_disc_does_not_have_are_said(tmp_path):
    install_log(lib.AV_LOG_INFO)
    t = dump(disc(tmp_path, project((("Logo", 2, 1.0), ("Splash", 114, 58.5)))))
    assert menu(t) == ["menu 'My Project' named 1", "menu 00002 'Logo'"]
    assert logged("Blu-ray: the menu project data names 1 playlist(s) the disc does not have: 00114 'Splash'")
