"""libavformat/dvdvideo_source.c: the DVD-Video demuxer's disc source.
libdvdread opens the disc through our file callbacks (DVDOpenFiles), so its
files come from the file system our choice rule picked (images) or from the
disc folder, read through our disc readers; images that are not DVD-Video,
or whose DVD files are scattered, are refused; an IFO block that cannot be
read comes from its BUP; the IFO layout checks warn; the title-set checks
(VOBU address map, cells inside the title VOBs) on built IFOs. With
DVDVIDEO_CSS_IMAGE / DVDVIDEO_CSS_FOLDER (a CSS-scrambled disc as an image /
as a folder of its files), blocks descrambled by the source equal
libdvdcss's own decrypting read."""

import os
import pathlib
import stat
import struct

import pytest

from helpers import EINVAL, ENOENT, LOG, averror, error_text, ffi, install_log, lib
from helpers.css import dvdcss_read
from helpers.disc import Image, env_or_skip, free_file
from helpers.dvd import image_options
from synth.image import (BUP, NO_VMG, PART_START, VMG, VTS1, Img, bridge, dvd_files, iso,
                         udf102)

S = 2048
EOF_ = -0x20464F45  # FFERRTAG('E','O','F',' ')
INVALIDDATA = -0x41444E49  # FFERRTAG('I','N','D','A')
PATCHWELCOME = -0x45574150  # FFERRTAG('P','A','W','E')
DVD_READ_INFO_FILE = 0
DVD_READ_TITLE_VOBS = 3


# ---- running the C code ----

class Src:
    """An open disc source."""

    def __init__(self, ptr):
        self.ptr = ptr

    @classmethod
    def open(cls, path):
        """A Src, or the negative error code."""
        src = ffi.new("DVDVideoSource **")
        ret = lib.ff_dvdvideo_source_open(ffi.NULL, str(path).encode(), image_options(), 5, src)
        return ret if ret < 0 else cls(src[0])

    def __del__(self):
        lib.ff_dvdvideo_source_close(ffi.new("DVDVideoSource **", self.ptr))

    def reader(self):
        """libdvdread on the source's file callbacks."""
        fs = lib.ff_dvdvideo_source_files(self.ptr)
        assert fs != ffi.NULL
        dvd = lib.DVDOpenFiles(ffi.NULL, ffi.NULL, b"/", fs)
        assert dvd != ffi.NULL, "DVDOpenFiles failed"
        return Dvd(dvd)

    def vob_block(self, vtsn, menu, sector):
        """Block sector of the menu VOBs (menu) or title VOBs of title set
        vtsn, or the error code."""
        buf = ffi.new("uint8_t[]", S)
        ret = lib.ff_dvdvideo_source_vob_read(self.ptr, vtsn, int(menu), sector, buf, 1)
        return ret if ret < 0 else bytes(buf)


def opened(path):
    src = Src.open(path)
    assert not isinstance(src, int), f"opening {path}: {error_text(src)}"
    return src


class Dvd:
    def __init__(self, ptr):
        self.ptr = ptr

    def __del__(self):
        lib.DVDClose(self.ptr)

    def read_bytes(self, title, domain, n):
        """The first n bytes of the file of title in domain."""
        f = lib.DVDOpenFile(self.ptr, title, domain)
        assert f != ffi.NULL, f"DVDOpenFile({title}, {domain}) failed"
        buf = ffi.new("uint8_t[]", n)
        got = lib.DVDReadBytes(f, buf, n)
        lib.DVDCloseFile(f)
        assert got == n
        return bytes(buf)

    def read_title_blocks(self, title, offset, count):
        """count blocks of the title VOBs of title from block offset."""
        f = lib.DVDOpenFile(self.ptr, title, DVD_READ_TITLE_VOBS)
        assert f != ffi.NULL
        buf = ffi.new("unsigned char[]", count * S)
        got = lib.DVDReadBlocks(f, offset, count, buf)
        lib.DVDCloseFile(f)
        assert got == count
        return bytes(buf)


def write_image(folder, im):
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / "disc.iso"
    path.write_bytes(im.d)
    return path


def fill(im, start, n, seed):
    """Fills sectors start..start + n with a pattern of their own."""
    for s in range(start, start + n):
        im.sector(s)[:] = bytes(((i + s) % 251) ^ seed for i in range(S))


def sectors(im, start, n):
    return bytes(im.d[start * S:(start + n) * S])


def logged(text, f):
    """Whether a log line containing text was written while f ran."""
    install_log(lib.AV_LOG_VERBOSE)
    f()
    return any(text in line for line in LOG)


# ---- images ----

def test_image_files_come_from_the_chosen_file_system(tmp_path):
    # UDF 1.02 recorded in 2004: the ISO 9660 tree is chosen; the UDF tree points
    # VIDEO_TS.IFO somewhere else.
    udf_files = [("VIDEO_TS.IFO", NO_VMG, 4096), ("VTS_01_0.IFO", VTS1, 4096)]
    im = bridge(udf102(2004, udf_files), dvd_files())
    fill(im, NO_VMG, 2, 0x5a)
    v = im.sector(VMG)
    v[0x40:] = bytes(i % 249 for i in range(S - 0x40))
    src = opened(write_image(tmp_path, im))
    got = src.reader().read_bytes(0, DVD_READ_INFO_FILE, 4096)
    assert got == sectors(im, VMG, 2), "VIDEO_TS.IFO as the ISO 9660 tree records it"
    assert got != sectors(im, NO_VMG, 2)


def test_a_scattered_dvd_file_is_refused(tmp_path):
    # VTS_01_1.VOB in two pieces that do not follow each other.
    u = udf102(2009, dvd_files())
    u.pieces = [("VTS_01_1.VOB", [(PART_START + 50, 4096), (PART_START + 60, 4096)])]
    assert Src.open(write_image(tmp_path / "scattered", bridge(u, dvd_files()))) == PATCHWELCOME
    # Two pieces that follow each other are one piece on the disc.
    u = udf102(2009, dvd_files())
    u.pieces = [("VTS_01_1.VOB", [(PART_START + 50, 4096), (PART_START + 52, 4096)])]
    assert not isinstance(Src.open(write_image(tmp_path / "adjacent", bridge(u, dvd_files()))), int)


def test_images_that_are_not_dvd_video_are_refused(tmp_path):
    def f(name):
        return (name, 50, 100)
    im = Img()
    iso(im, "BD", [("BDMV", [f("index.bdmv"), f("MovieObject.bdmv")])])
    assert Src.open(write_image(tmp_path / "bluray", im)) == PATCHWELCOME
    im = Img()
    iso(im, "HDDVD", [("ADV_OBJ", [f("DISCID.DAT")]), ("HVDVD_TS", [f("HVA00001.VTI")])])
    assert Src.open(write_image(tmp_path / "hddvd", im)) == PATCHWELCOME
    im = Img()
    iso(im, "DATA", [("DOCS", [f("README.TXT")])])
    assert Src.open(write_image(tmp_path / "data", im)) == INVALIDDATA
    assert Src.open(write_image(tmp_path / "blank", Img())) == INVALIDDATA


def test_paths_that_are_not_discs_are_refused(tmp_path):
    assert Src.open(tmp_path / "nothing.iso") == averror(ENOENT)
    assert Src.open("/dev/null") == averror(EINVAL)


# ---- IFO blocks from the BUP ----

# Sector of the image's VIDEO_TS.IFO whose second block lies past the end of
# the image (the image is cut after it).
CUT_IFO = 449


def image_with_a_cut_ifo(bup):
    files = [("VIDEO_TS.IFO", CUT_IFO, 4096), ("VTS_01_0.IFO", VTS1, 4096)]
    if bup:
        files.append(("VIDEO_TS.BUP", BUP, 4096))
    im = bridge(udf102(2009, files), dvd_files())
    im.vmg(CUT_IFO, b"DVDVIDEO-VMG", 1)
    im.sector(CUT_IFO)[0x400:] = bytes(i % 241 for i in range(S - 0x400))
    # the BUP holds the IFO's bytes: its block 0 equals the IFO's, block 1 is
    # the copy of the IFO block the cut image lost
    im.sector(BUP)[:] = sectors(im, CUT_IFO, 1)
    fill(im, BUP + 1, 1, 0x33)
    im.d = im.d[:(CUT_IFO + 1) * S]
    return im


def test_an_unreadable_ifo_block_is_read_from_the_bup(tmp_path):
    # the read of both blocks fails on the IFO (try 1); the next try reads the
    # BUP, which then serves the rest
    im = image_with_a_cut_ifo(True)
    src = opened(write_image(tmp_path, im))
    want = sectors(im, CUT_IFO, 1) + sectors(im, BUP + 1, 1)

    def f():
        assert src.reader().read_bytes(0, DVD_READ_INFO_FILE, 4096) == want
    assert logged("VIDEO_TS.IFO: read at byte 0 failed (try 1 of 16); the next try reads the backup copy (BUP)", f)


def test_after_a_failed_try_the_reads_stay_on_the_bup(tmp_path):
    # a BUP whose block 0 differs from the IFO's shows where each block came
    # from: after the failed try on the IFO, block 0 too is read from the BUP
    im = image_with_a_cut_ifo(True)
    other = bytes(i % 239 for i in range(S))
    im.d[BUP * S:(BUP + 1) * S] = other
    src = opened(write_image(tmp_path, im))
    got = src.reader().read_bytes(0, DVD_READ_INFO_FILE, 4096)
    assert got[:S] == other, "block 0 from the BUP"
    assert got[S:] == sectors(im, BUP + 1, 1), "block 1 from the BUP"


def test_without_a_bup_the_unreadable_ifo_block_is_a_read_error(tmp_path):
    im = image_with_a_cut_ifo(False)
    src = opened(write_image(tmp_path, im))
    dvd = src.reader()
    f = lib.DVDOpenFile(dvd.ptr, 0, DVD_READ_INFO_FILE)
    assert f != ffi.NULL
    buf = ffi.new("uint8_t[]", 4096)

    def read():
        assert lib.DVDReadBytes(f, buf, 4096) < 4096
    assert logged("could not be read from the IFO or its backup copy in 16 tries", read)
    lib.DVDCloseFile(f)


# ---- the IFO layout checks (warnings) ----

def ifo_sizes(im, sector, last, ifo_last):
    """Sets vmg/vts_last_sector and vmgi/vtsi_last_sector of the IFO header
    at sector."""
    h = im.sector(sector)
    h[0x0c:0x10] = struct.pack(">I", last)
    h[0x1c:0x20] = struct.pack(">I", ifo_last)


def test_ifo_layout_warnings(tmp_path):
    # image: the header puts the BUP 7 sectors after the IFO, the image 2
    im = bridge(udf102(2009, dvd_files()), dvd_files())
    ifo_sizes(im, VMG, 8, 1)
    path = write_image(tmp_path / "layout", im)
    assert logged("puts the backup copy (BUP) 7 sectors after the IFO; on the image it is 2 sectors",
                  lambda: opened(path))
    # ... and where they agree, no warning
    im = bridge(udf102(2009, dvd_files()), dvd_files())
    ifo_sizes(im, VMG, 3, 1)
    path = write_image(tmp_path / "layout-ok", im)
    assert not logged("VIDEO_TS.IFO: its header puts the backup copy (BUP) 2 sectors", lambda: opened(path))
    # the provider identifier of a disc processed by DVDFab
    im = bridge(udf102(2009, dvd_files()), dvd_files())
    im.sector(VMG)[0x40:0x49] = b"(Fab4321)"
    path = write_image(tmp_path / "fab", im)
    assert logged("provider identifier '(Fab4321)'", lambda: opened(path))
    # folder: IFO (2 sectors) + VIDEO_TS.VOB (3 sectors) before the BUP; a header saying 99
    d = tmp_path / "layout-folder" / "VIDEO_TS"
    d.mkdir(parents=True)
    ifo = bytearray(2 * S)
    ifo[:12] = b"DVDVIDEO-VMG"
    ifo[0x0c:0x10] = struct.pack(">I", 100)
    ifo[0x1c:0x20] = struct.pack(">I", 1)
    (d / "VIDEO_TS.IFO").write_bytes(ifo)
    (d / "VIDEO_TS.VOB").write_bytes(bytes(3 * S))
    root = d.parent
    assert logged("BUP) 99 sectors after the IFO; the IFO and VOB files before it hold 5 sectors",
                  lambda: opened(root))
    ifo[0x0c:0x10] = struct.pack(">I", 6)
    (d / "VIDEO_TS.IFO").write_bytes(ifo)
    assert not logged("BUP) 5 sectors after the IFO", lambda: opened(root))


# ---- the title-set checks ----

MAP_DISTRUSTED = 1
CELL_PAST_VOBS = 2


def vts_ifo(cells, vobus, vob_sectors):
    """VTS_01_0.IFO with one PGC of cells (first sector, last VOBU start,
    last sector; one program), a cell address table of the same cells and a
    VOBU address map of vobus: VTSI_MAT in sector 0, VTS_PTT_SRPT in 1,
    VTS_PGCIT in 2, VTS_C_ADT in 3, VTS_VOBU_ADMAP in 4."""
    d = bytearray(5 * S)

    def be32(at, v):
        d[at:at + 4] = struct.pack(">I", v)

    def be16(at, v):
        d[at:at + 2] = struct.pack(">H", v)
    # VTSI_MAT
    d[:12] = b"DVDVIDEO-VTS"
    be32(0x0c, 5 + vob_sectors + 5 - 1)  # vts_last_sector: IFO, title VOBs, BUP
    be32(0x1c, 4)  # vtsi_last_sector
    be32(0x80, 0x3ff)  # vtsi_last_byte
    be32(0xc4, 5)  # vtstt_vobs
    be32(0xc8, 1)  # vts_ptt_srpt
    be32(0xcc, 2)  # vts_pgcit
    be32(0xe0, 3)  # vts_c_adt
    be32(0xe4, 4)  # vts_vobu_admap
    # VTS_PTT_SRPT: one title, one chapter (PGC 1, program 1)
    p = S
    be16(p, 1)
    be32(p + 4, 15)
    be32(p + 8, 12)
    be16(p + 12, 1)
    be16(p + 14, 1)
    # VTS_PGCIT: one PGC at byte 16
    t = 2 * S
    pgc = t + 16
    map_off, play_off = 0xec, 0xee
    pos_off = play_off + 24 * len(cells)
    pgc_len = pos_off + 4 * len(cells)
    be16(t, 1)
    be32(t + 4, 16 + pgc_len - 1)
    d[t + 8] = 0x81  # entry PGC of title 1
    be32(t + 12, 16)
    d[pgc + 2] = 1  # nr_of_programs
    d[pgc + 3] = len(cells)  # nr_of_cells
    be16(pgc + 0xe6, map_off)
    be16(pgc + 0xe8, play_off)
    be16(pgc + 0xea, pos_off)
    d[pgc + map_off] = 1  # program 1 starts at cell 1
    for k, (first, last_vobu, last) in enumerate(cells):
        c = pgc + play_off + 24 * k
        be32(c + 8, first)
        be32(c + 16, last_vobu)
        be32(c + 20, last)
        q = pgc + pos_off + 4 * k
        be16(q, 1)
        d[q + 3] = k + 1
    # VTS_C_ADT
    a = 3 * S
    be16(a, 1)
    be32(a + 4, 8 + 12 * len(cells) - 1)
    for k, (first, _, last) in enumerate(cells):
        e = a + 8 + 12 * k
        be16(e, 1)
        d[e + 2] = k + 1
        be32(e + 4, first)
        be32(e + 8, last)
    # VTS_VOBU_ADMAP
    m = 4 * S
    be32(m, 4 + 4 * len(vobus) - 1)
    for k, v in enumerate(vobus):
        be32(m + 4 + 4 * k, v)
    return d


def check_vts(folder, cells, vobus, vob_sectors):
    """The title-set checks on a folder holding the IFO and title VOBs of
    vob_sectors."""
    d = folder / "VIDEO_TS"
    d.mkdir(parents=True)
    (d / "VTS_01_0.IFO").write_bytes(vts_ifo(cells, vobus, vob_sectors))
    (d / "VTS_01_1.VOB").write_bytes(bytes(vob_sectors * S))
    src = opened(folder)
    dvd = src.reader()
    ifo = lib.ifoOpen(dvd.ptr, 1)
    assert ifo != ffi.NULL, "libdvdread accepts the built IFO"
    r = lib.ff_dvdvideo_check_vts(ffi.NULL, dvd.ptr, 1, ifo)
    lib.ifoClose(ifo)
    return r


VOBUS = [0, 10, 20, 30]


def test_title_set_cells_on_the_vobu_map_and_inside_the_vobs(tmp_path):
    assert check_vts(tmp_path / "ok", [(0, 10, 19), (20, 30, 39)], VOBUS, 40) == 0
    # a cell that starts on no VOBU of the map
    assert check_vts(tmp_path / "first", [(0, 10, 19), (21, 30, 39)], VOBUS, 40) == MAP_DISTRUSTED
    # a cell whose last VOBU is not in the map
    assert check_vts(tmp_path / "last", [(0, 15, 19), (20, 30, 39)], VOBUS, 40) == MAP_DISTRUSTED
    # a cell that ends past the end of the title VOBs: its data is missing
    assert check_vts(tmp_path / "past", [(0, 10, 19), (20, 30, 45)], VOBUS, 40) == CELL_PAST_VOBS
    # the last sector itself is the limit
    assert check_vts(tmp_path / "edge", [(0, 10, 19), (20, 30, 40)], VOBUS, 40) == CELL_PAST_VOBS
    assert check_vts(tmp_path / "both", [(0, 10, 19), (25, 30, 45)], VOBUS, 40) == MAP_DISTRUSTED | CELL_PAST_VOBS


def test_a_duplicate_of_a_program_chain_that_cannot_be_read_is_left_out(tmp_path):
    # two program-chain entries naming the same start, past the end of the IFO:
    # the first cannot be read, the second shares its start (crashed libdvdread)
    ifo = vts_ifo([(0, 10, 19)], VOBUS, 40)
    t = 2 * S
    ifo[t:t + 2] = struct.pack(">H", 2)
    for k, ident in [(0, 0x81), (1, 0x82)]:
        e = t + 8 + 8 * k
        ifo[e:e + 4] = bytes([ident, 0, 0, 0])
        ifo[e + 4:e + 8] = struct.pack(">I", 0x8000)
    d = tmp_path / "VIDEO_TS"
    d.mkdir()
    (d / "VTS_01_0.IFO").write_bytes(ifo)
    (d / "VTS_01_1.VOB").write_bytes(bytes(40 * S))
    src = opened(tmp_path)
    dvd = src.reader()
    h = lib.ifoOpen(dvd.ptr, 1)
    assert h != ffi.NULL, "the title set opens; both program chains are left out"
    lib.ifoClose(h)


# ---- folders ----

def folder_files():
    """The files of a small DVD-Video folder: VIDEO_TS.IFO and two title VOBs."""
    def pattern(n, seed):
        return bytes((i % 253) ^ seed for i in range(n))
    return [("VIDEO_TS.IFO", pattern(4096, 1)), ("VTS_01_1.VOB", pattern(3 * S, 2)),
            ("VTS_01_2.VOB", pattern(2 * S, 3))]


def write_files(folder, lower):
    folder.mkdir(parents=True)
    for name, data in folder_files():
        (folder / (name.lower() if lower else name)).write_bytes(data)


def test_every_folder_layout_opens(tmp_path):
    files = folder_files()
    # A folder holding VIDEO_TS; the VIDEO_TS folder itself; a folder with the
    # files directly; lower-case names.
    write_files(tmp_path / "Movie" / "VIDEO_TS", False)
    write_files(tmp_path / "Bare", False)
    write_files(tmp_path / "Lower" / "video_ts", True)
    for root in [tmp_path / "Movie", tmp_path / "Movie" / "VIDEO_TS", tmp_path / "Bare", tmp_path / "Lower"]:
        src = opened(root)
        dvd = src.reader()
        assert dvd.read_bytes(0, DVD_READ_INFO_FILE, 4096) == files[0][1], root
        # The title VOBs are read as one: blocks 2-3 run from VTS_01_1 into VTS_01_2.
        assert dvd.read_title_blocks(1, 2, 2) == files[1][1][2 * S:] + files[2][1][:S], root


# ---- CSS on a scrambled disc ----

CSS_BLOCKS = 4096


def descrambled(src, path, start, n):
    """Raw blocks of path from start, descrambled one by one by the source:
    (bytes, blocks that were scrambled)."""
    with open(path, "rb") as f:
        f.seek(start * S)
        raw = bytearray(f.read(n * S))
    assert len(raw) == n * S
    scrambled = 0
    for k in range(n):
        block = ffi.from_buffer(memoryview(raw)[k * S:(k + 1) * S])
        r = lib.ff_dvdvideo_source_descramble(src.ptr, 1, 0, block)
        assert r >= 0, f"descramble failed: {r}"
        scrambled += r
    return bytes(raw), scrambled


@pytest.mark.disc
def test_corpus_css_image_equals_libdvdcss():
    image = env_or_skip("DVDVIDEO_CSS_IMAGE")
    # Where VTS_01_1.VOB starts, as the chosen file system records it.
    v = Image.open(image).mount()
    assert not isinstance(v, int)
    ret, f = v.open_file("/VIDEO_TS/VTS_01_1.VOB")
    assert ret == 0
    start = f.extents[0].sector
    free_file(f)
    v.close()
    src = opened(image)
    ours, scrambled = descrambled(src, image, start, CSS_BLOCKS)
    assert scrambled > 0, "no scrambled block in the sample"
    assert ours == dvdcss_read(image, start, CSS_BLOCKS), "image: blocks differ"
    print(f"image: {scrambled} of {CSS_BLOCKS} blocks descrambled, equal to libdvdcss")


@pytest.mark.disc
def test_corpus_css_folder_equals_libdvdcss():
    folder = pathlib.Path(env_or_skip("DVDVIDEO_CSS_FOLDER"))
    vob = folder / "VIDEO_TS" / "VTS_01_1.VOB"
    src = opened(folder)
    ours, scrambled = descrambled(src, vob, 0, CSS_BLOCKS)
    assert scrambled > 0, "no scrambled block in the sample"
    assert ours == dvdcss_read(vob, 0, CSS_BLOCKS), "folder: blocks differ"
    print(f"folder: {scrambled} of {CSS_BLOCKS} blocks descrambled, equal to libdvdcss")


# ---- VOB groups ----

# Image blocks of the files of title set 1 in the VOB-group images.
VTS_IFO = VTS1  # 2 blocks
MENU_VOB = PART_START + 40
TITLE_VOB = PART_START + 50


def first_nav_pack(im, sector):
    """Makes sector the first NAV pack of a VOB (its PCI and DSI name sector 0)."""
    s = im.sector(sector)
    s[:] = bytes(S)
    s[:5] = bytes([0, 0, 1, 0xba, 0x44])
    s[0x0e:0x14] = bytes([0, 0, 1, 0xbb, 0, 0x12])
    s[0x26:0x2d] = bytes([0, 0, 1, 0xbf, 0x03, 0xd4, 0])
    s[0x400:0x407] = bytes([0, 0, 1, 0xbf, 0x03, 0xfa, 1])


def vob_image(menu_file, title_file, vtsm, vtstt):
    """An image with VTS_01_0.VOB (2 blocks) at menu_file and VTS_01_1.VOB
    (2 blocks) at title_file; the VTS IFO header puts the menu VOBs vtsm and
    the title VOBs vtstt blocks after the IFO. Every block from the menu
    file on holds a pattern of its own."""
    files = dvd_files() + [("VTS_01_0.VOB", menu_file, 4096), ("VTS_01_1.VOB", title_file, 4096)]
    im = bridge(udf102(2009, files), dvd_files())
    h = im.sector(VTS_IFO)
    h[:12] = b"DVDVIDEO-VTS"
    h[0xc0:0xc4] = struct.pack(">I", vtsm)
    h[0xc4:0xc8] = struct.pack(">I", vtstt)
    fill(im, MENU_VOB, 520 - MENU_VOB, 0x6c)
    return im


def test_image_vob_groups_reach_past_their_files(tmp_path):
    # the IFO and the file system agree: the title VOBs run to the end of the
    # image (beyond VTS_01_1.VOB), the menu VOBs up to the title VOBs
    im = vob_image(MENU_VOB, TITLE_VOB, MENU_VOB - VTS_IFO, TITLE_VOB - VTS_IFO)
    src = opened(write_image(tmp_path, im))
    assert src.vob_block(1, False, 0) == sectors(im, TITLE_VOB, 1)
    assert src.vob_block(1, False, 100) == sectors(im, TITLE_VOB + 100, 1), "past the file"
    assert src.vob_block(1, False, 519 - TITLE_VOB) == sectors(im, 519, 1)
    assert src.vob_block(1, False, 520 - TITLE_VOB) == EOF_, "past the image"
    assert src.vob_block(1, True, 9) == sectors(im, TITLE_VOB - 1, 1)
    assert src.vob_block(1, True, 10) == EOF_, "the menu VOBs end where the title VOBs start"


def test_image_vob_start_when_the_ifo_and_the_file_system_disagree(tmp_path):
    # the IFO says 4 blocks later than the file: the file's position holds the
    # first NAV pack of a VOB, the IFO's does not -> the file system's start
    im = vob_image(MENU_VOB, TITLE_VOB, MENU_VOB - VTS_IFO, TITLE_VOB + 4 - VTS_IFO)
    first_nav_pack(im, TITLE_VOB)
    src = opened(write_image(tmp_path / "fs", im))

    def f():
        assert src.vob_block(1, False, 0) == sectors(im, TITLE_VOB, 1)
    assert logged("the IFO puts its title VOBs 20 blocks after the IFO, the file system 16 blocks after it", f)
    # both positions start a VOB: the IFO's wins
    first_nav_pack(im, TITLE_VOB + 4)
    src = opened(write_image(tmp_path / "both", im))
    assert src.vob_block(1, False, 0) == sectors(im, TITLE_VOB + 4, 1)
    # the menu VOBs: neither position starts a VOB -> they start where the
    # title VOBs start, which is also their end: nothing to read
    im = vob_image(MENU_VOB, TITLE_VOB, MENU_VOB + 2 - VTS_IFO, TITLE_VOB - VTS_IFO)
    src = opened(write_image(tmp_path / "none", im))

    def g():
        assert src.vob_block(1, True, 0) == EOF_
    assert logged("neither position starts a VOB", g)


def test_image_menu_vobs_starting_after_the_title_vobs_reach_to_the_end_of_the_image(tmp_path):
    # the menu VOBs start 2 blocks after the title VOBs (the file is there too)
    im = vob_image(TITLE_VOB + 2, TITLE_VOB, TITLE_VOB + 2 - VTS_IFO, TITLE_VOB - VTS_IFO)
    src = opened(write_image(tmp_path, im))
    assert src.vob_block(1, True, 0) == sectors(im, TITLE_VOB + 2, 1)
    assert src.vob_block(1, True, 100) == sectors(im, TITLE_VOB + 102, 1)


def vmg_only():
    return b"DVDVIDEO-VMG" + bytes(S - 12)


def test_folder_vob_groups_are_their_files(tmp_path):
    vts = tmp_path / "VIDEO_TS"
    vts.mkdir()

    def block(b):
        return bytes([b]) * S
    (vts / "VIDEO_TS.IFO").write_bytes(vmg_only())
    (vts / "VTS_01_1.VOB").write_bytes(block(1) + block(2))
    (vts / "VTS_01_2.VOB").write_bytes(block(3) + bytes([4]) * 100)
    (vts / "VTS_01_4.VOB").write_bytes(block(9))
    src = opened(tmp_path)
    assert src.vob_block(1, False, 2) == block(3), "VTS_01_2.VOB follows VTS_01_1.VOB"
    assert src.vob_block(1, False, 3) == bytes([4]) * 100 + b"\xff" * (S - 100), "0xFF after the end"
    assert src.vob_block(1, False, 4) == EOF_, "VTS_01_4.VOB is not reached: VTS_01_3.VOB is missing"
    assert src.vob_block(1, True, 0) == averror(ENOENT), "no menu VOB"


def vob_folder(folder, parts):
    """A folder whose title VOBs are parts (in order, VTS_01_1.VOB on); a
    part None is a directory of that name."""
    vts = folder / "VIDEO_TS"
    vts.mkdir(parents=True)
    (vts / "VIDEO_TS.IFO").write_bytes(vmg_only())
    for k, part in enumerate(parts):
        path = vts / f"VTS_01_{k + 1}.VOB"
        if part is None:
            path.mkdir()
        else:
            path.write_bytes(part)
    return folder


def joined_blocks(data, start, n):
    """Blocks start..start + n of the joined bytes, 0xFF after their end."""
    out = bytearray(b"\xff" * (n * S))
    end = min(len(data), (start + n) * S)
    if start * S < end:
        out[:end - start * S] = data[start * S:end]
    return bytes(out)


def test_folder_vob_files_are_joined_as_bytes(tmp_path):
    # 7000 bytes of pattern split into files of 3000 (not whole blocks), 2500
    # and 1500 bytes: the blocks are those of the bytes joined, a block may
    # hold the end of one file and the start of the next; 0xFF after the end
    data = bytes(i % 251 for i in range(7000))
    src = opened(vob_folder(tmp_path, [data[:3000], data[3000:5500], data[5500:]]))
    for b in range(4):
        assert src.vob_block(1, False, b) == joined_blocks(data, b, 1), f"block {b}"
    assert src.vob_block(1, False, 4) == EOF_
    # libdvdread reads the title VOBs through the same blocks
    dvd = src.reader()
    assert dvd.read_title_blocks(1, 0, 4) == joined_blocks(data, 0, 4)
    assert dvd.read_title_blocks(1, 1, 2) == joined_blocks(data, 1, 2)


def test_an_empty_vob_file_adds_nothing(tmp_path):
    data = bytes(i % 249 for i in range(5000))
    src = opened(vob_folder(tmp_path, [data[:2048], b"", data[2048:]]))

    def f():
        assert src.vob_block(1, False, 1) == joined_blocks(data, 1, 1), "VTS_01_3.VOB follows VTS_01_1.VOB"
    assert logged("is empty: skipped", f)
    assert src.reader().read_title_blocks(1, 0, 3) == joined_blocks(data, 0, 3)


def test_a_vob_name_that_is_not_a_regular_file_ends_the_vob_files(tmp_path):
    src = opened(vob_folder(tmp_path, [bytes([1]) * S, None, bytes([3]) * S]))

    def f():
        assert src.vob_block(1, False, 0) == bytes([1]) * S
    assert logged("is not a regular file: the VOB files of the title set end before it", f)
    assert src.vob_block(1, False, 1) == EOF_, "VTS_01_3.VOB is not reached"


def test_a_vob_file_that_cannot_be_opened_makes_the_title_vobs_unreadable(tmp_path):
    folder = vob_folder(tmp_path, [bytes([1]) * S, bytes([2]) * S])
    part2 = folder / "VIDEO_TS" / "VTS_01_2.VOB"
    part2.chmod(0)
    try:
        if os.access(part2, os.R_OK):
            pytest.skip("the file stays readable (running as root?)")
        src = opened(folder)

        def f():
            assert isinstance(src.vob_block(1, False, 0), int), "not even VTS_01_1.VOB is read"
        assert logged("the title VOBs of title set 1 cannot be read", f)
        dvd = src.reader()
        assert lib.DVDOpenFile(dvd.ptr, 1, DVD_READ_TITLE_VOBS) == ffi.NULL, "libdvdread has no title VOBs either"
    finally:
        part2.chmod(stat.S_IRUSR | stat.S_IWUSR | stat.S_IRGRP | stat.S_IROTH)
