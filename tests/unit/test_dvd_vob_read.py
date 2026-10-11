"""libdvdread's title VOB reading for ripping (dvdread/vob_read.h, through
libavformat/dvd_read.c): each sector is checked (no MPEG-2 pack followed by
a packet start code: left out as damaged), and damaged sectors one after the
other in reading order and of one kind make one damaged range, given back
once when it ends (a sector not part of it is read, or the caller ends it)
with the files and byte offsets of its first and last sector: a folder's
VTS_nn_k.VOB files joined, an image's title VOBs from where the IFO header
puts them, named by the VOB files' places on the image."""

from helpers import ffi, install_log, lib, logged
from synth.dvd import S
from test_dvd_disc import Disc, disc_files, image, vts
from test_dvd_read import ac3_pack, folder_with

ZERO = bytes(S)
JUNK = b"\xff" * S


def plain():
    p = ac3_pack()
    p[20] &= 0xcf                               # not scrambled
    return bytes(p)


class Reading:
    """A reading of title set 1's title VOBs of its own."""

    def __init__(self, d, vts_=1):
        self.d, self.f = d, d.ifo(vts_)
        self.vr = lib.ff_dvd_title_vobs_open(d.d, self.f)
        assert self.vr != ffi.NULL

    def read(self, sector):
        buf = ffi.new("uint8_t[]", S)
        ended = ffi.new("dvd_vob_damage_t *")
        ret = lib.ff_dvd_title_vob_read(self.d.d, self.f, self.vr, sector, buf, ended)
        return ret, damage(ended)

    def end(self):
        ended = ffi.new("dvd_vob_damage_t *")
        lib.DVDEndTitleVOBDamage_rec(self.vr, ended)
        return damage(ended)

    def close(self):
        lib.DVDCloseTitleVOBs_rec(self.vr)


def findings(d):
    """The disc's IFO / navigation findings: (harmless, rule breaks)."""
    harmless, breaks = ffi.new("int *"), ffi.new("int *")
    lib.ff_dvd_reports_count(d.d, harmless, breaks)
    return harmless[0], breaks[0]


def damage(g):
    if not g.nb_sectors:
        return None
    return (int(g.kind), g.sector, g.nb_sectors, ffi.string(g.file).decode(), g.offset,
            ffi.string(g.last_file).decode(), g.last_offset)


def test_damaged_sectors_make_one_range_per_run_and_kind_given_back_when_it_ends(tmp_path):
    d = Disc.folder(folder_with(tmp_path, [plain(), ZERO, ZERO, JUNK, plain(), ZERO]))
    install_log(lib.AV_LOG_DEBUG)
    r = Reading(d)
    assert r.read(1) == (1, None)
    assert r.read(2) == (0, None) and r.read(3) == (0, None)
    # another kind ends the run of zero sectors
    assert r.read(4) == (0, (lib.DVD_DAMAGE_ZERO, 2, 2, "VTS_01_1.VOB", 2 * S, "VTS_01_1.VOB", 3 * S))
    # a usable sector ends the next
    assert r.read(5) == (1, (lib.DVD_DAMAGE_NO_PACK, 4, 1, "VTS_01_1.VOB", 4 * S, "VTS_01_1.VOB", 4 * S))
    assert r.read(6) == (0, None)
    assert r.end() == (lib.DVD_DAMAGE_ZERO, 6, 1, "VTS_01_1.VOB", 6 * S, "VTS_01_1.VOB", 6 * S)
    assert r.end() is None, "said once"
    # sectors not one after the other: two ranges
    assert r.read(7) == (0, None)
    assert r.read(9) == (0, (lib.DVD_DAMAGE_ZERO, 7, 1, "VTS_01_1.VOB", 7 * S, "VTS_01_1.VOB", 7 * S))
    r.close()
    d.close()


def test_a_range_across_two_title_vob_files_is_one_range_naming_both(tmp_path):
    path = folder_with(tmp_path, [plain(), ZERO, ZERO, ZERO, plain()])
    vobs = path / "VIDEO_TS"
    data = (vobs / "VTS_01_1.VOB").read_bytes()
    (vobs / "VTS_01_1.VOB").write_bytes(data[:3 * S])
    (vobs / "VTS_01_2.VOB").write_bytes(data[3 * S:])
    d = Disc.folder(path)
    r = Reading(d)
    got = [r.read(s) for s in range(1, 6)]
    assert [ret for ret, _ in got] == [1, 0, 0, 0, 1]
    assert got[-1][1] == (lib.DVD_DAMAGE_ZERO, 2, 3, "VTS_01_1.VOB", 2 * S, "VTS_01_2.VOB", 1 * S)
    r.close()
    d.close()


def test_on_an_image_ranges_are_named_by_the_vob_files_place_on_it():
    v = vts(vob_sectors=10)
    files, ifo = disc_files(v)
    files = dict(files)
    vob = bytearray(files["VTS_01_1.VOB"])     # sector 0: a NAV pack, the rest zeros
    vob[5 * S:6 * S] = JUNK
    files["VTS_01_1.VOB"] = bytes(vob)
    im, where = image(list(files.items()))
    d = Disc.image(im)
    assert d.ret == 0 and d.ifo(1).title_vob_start == where["VTS_01_1.VOB"]
    r = Reading(d)
    got = [r.read(s) for s in range(10)]
    assert [ret for ret, _ in got] == [1, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    assert got[5][1] == (lib.DVD_DAMAGE_ZERO, 1, 4, "VTS_01_1.VOB", S, "VTS_01_1.VOB", 4 * S)
    assert got[6][1] == (lib.DVD_DAMAGE_NO_PACK, 5, 1, "VTS_01_1.VOB", 5 * S, "VTS_01_1.VOB", 5 * S)
    assert r.end() == (lib.DVD_DAMAGE_ZERO, 6, 4, "VTS_01_1.VOB", 6 * S, "VTS_01_1.VOB", 9 * S)
    r.close()
    d.close()


def test_the_title_set_s_own_reading_says_its_ranges_but_keeps_them_out_of_the_findings(tmp_path):
    # ff_dvd_title_vob_sector() (the stream pass, the key check): a range is
    # libdvdread's detail line only, not one of the disc's IFO / navigation
    # findings
    d = Disc.folder(folder_with(tmp_path, [ZERO, plain()]))
    before = findings(d)
    install_log(lib.AV_LOG_DEBUG)
    buf = ffi.new("uint8_t[]", S)
    assert lib.ff_dvd_title_vob_sector(d.d, d.ifo(1), 1, buf) == 0
    assert lib.ff_dvd_title_vob_sector(d.d, d.ifo(1), 2, buf) == 1
    assert logged("libdvdread: VTS_01_1.VOB: title VOB sectors 1-1 (VTS_01_1.VOB bytes 2048-4095): every byte is "
                  "zero: left out")
    assert findings(d) == before
    d.close()
