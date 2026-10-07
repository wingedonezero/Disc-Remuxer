"""libavformat/discio_folder.c: a disc folder on the host as a file system
(ff_discio_mount_folder): paths looked up below the folder as written, a
file must be a regular file, directories listed in the host's order with
"." and "..", each file read as a host file of its own; the disc format
probe on folders."""

import os

from helpers import ENOENT, averror, ffi, lib
from helpers.disc import Vol


def mount(path, attempts=1):
    fs = ffi.new("DiscIOFS **")
    ret = lib.ff_discio_mount_folder(ffi.NULL, str(path).encode(), attempts, fs)
    assert ret == 0, ret
    return Vol(fs[0], None)


def make(root, files):
    for name, data in files.items():
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)


def test_files_are_found_as_written_and_read_from_the_host(tmp_path):
    make(tmp_path, {"BDMV/index.bdmv": b"INDX0200" + bytes(range(40)), "BDMV/PLAYLIST/00001.mpls": b"MPLS"})
    v = mount(tmp_path)
    assert (v.name(), v.kind()) == ("folder", lib.DISCIO_FS_FOLDER)
    f = v.open("/BDMV/index.bdmv")
    assert f.size() == 48 and f.extents() == []
    assert f.read(8, 4) == bytes([0, 1, 2, 3])
    assert f.read(46, 2) == bytes([38, 39])
    assert f.read(46, 4) < 0, "a read past the end fails"
    # one leading '/' or '\\' is dropped; the case is the host's
    assert v.open("BDMV/index.bdmv").size() == 48
    assert v.open("\\BDMV/index.bdmv").size() == 48
    assert v.open("/BDMV/INDEX.BDMV") == averror(ENOENT)
    assert v.open("/BDMV/missing.bdmv") == averror(ENOENT)
    assert v.open("/BDMV/PLAYLIST") == averror(ENOENT), "a directory is no file"


def test_directories_are_listed_in_host_order_with_dot_entries(tmp_path):
    make(tmp_path, {"BDMV/CLIPINF/00001.clpi": b"x", "BDMV/CLIPINF/00002.clpi": b"y"})
    (tmp_path / "BDMV" / "CLIPINF" / "SUB").mkdir()
    v = mount(tmp_path)
    entries = v.list("/BDMV/CLIPINF")
    host = [(e.name, e.is_dir()) for e in os.scandir(tmp_path / "BDMV" / "CLIPINF")]
    assert sorted(entries) == sorted(host + [(".", True), ("..", True)])
    names = [n for n, _ in entries]
    assert [n for n in names if n not in (".", "..")] == [n for n, _ in host], "the host's order, not sorted"
    assert v.list("/BDMV/NONE") == averror(ENOENT)
    assert v.find_dir("/BDMV/CLIPINF") == 0
    assert v.find_dir("/BDMV/CLIPINF/00001.clpi") == averror(ENOENT)
    assert v.find_dir("/") == 0


def test_a_trailing_separator_and_the_disc_format(tmp_path):
    make(tmp_path, {"BDMV/index.bdmv": b"INDX", "BDMV/MovieObject.bdmv": b"MOBJ"})
    v = mount(str(tmp_path) + "/")
    assert v.format() == lib.DISCIO_DISC_BLURAY
    assert v.open("/BDMV/MovieObject.bdmv").size() == 4


def test_only_folders_are_mounted(tmp_path):
    make(tmp_path, {"file.iso": b"x"})
    fs = ffi.new("DiscIOFS **")
    assert lib.ff_discio_folder_is_dir(str(tmp_path).encode()) == 1
    assert lib.ff_discio_folder_is_dir(str(tmp_path / "file.iso").encode()) == 0
    assert lib.ff_discio_mount_folder(ffi.NULL, str(tmp_path / "file.iso").encode(), 1, fs) < 0
    assert fs[0] == ffi.NULL
