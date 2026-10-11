"""libdvdread's title VOB reading for ripping (dvdread/vob_read.h, through
libavformat/dvd_read.c): each sector is checked (no MPEG-2 pack followed by
a packet start code: left out as damaged), and damaged sectors one after the
other in reading order and of one kind make one damaged range, given back
once when it ends (a sector not part of it is read, or the caller ends it)
with the files and byte offsets of its first and last sector: a folder's
VTS_nn_k.VOB files joined, an image's title VOBs from where the IFO header
puts them, named by the VOB files' places on the image."""

from helpers import ffi, install_log, lib, logged
from synth import dvd
from synth.dvd import S
from test_dvd_disc import Disc, disc_files, image, vmg, vts
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


def test_a_title_set_s_css_key_is_one_for_all_its_readings_and_a_missing_one_fails_the_read(tmp_path):
    from test_dvd_read import KEY, css, css_scramble
    d = Disc.folder(folder_with(tmp_path, [css_scramble(KEY, ac3_pack()), css_scramble(KEY, ac3_pack())]))
    install_log(lib.AV_LOG_DEBUG)
    buf = ffi.new("uint8_t[]", S)
    # the title set's own reading finds the key ...
    assert lib.ff_dvd_title_vob_sector(d.d, d.ifo(1), 1, buf) == 1 and css(d) == (1, KEY)
    assert logged("DVD: VTS_01: sector 1 is scrambled: looking for the CSS title key")
    assert logged("DVD: VTS_01: CSS title key found")
    # ... and a reading of its own uses it: not looked for again, descrambled
    install_log(lib.AV_LOG_DEBUG)
    r = Reading(d)
    ret, _ = r.read(2)
    assert ret == 1 and not logged("looking for")
    r.close()
    d.close()

    # no key: the read fails, the search is said once and not made again
    bad = bytearray(ac3_pack(good_crc=False))
    d = Disc.folder(folder_with(tmp_path, [css_scramble(KEY, bytes(bad)), css_scramble(KEY, bytes(bad))]))
    install_log(lib.AV_LOG_DEBUG)
    r = Reading(d)
    assert r.read(1)[0] < 0 and css(d) == (-1, None)
    assert logged("DVD: VTS_01: the CSS title key is wrong on sector 1 (an AC-3 frame's CRC)")
    install_log(lib.AV_LOG_DEBUG)
    assert r.read(2)[0] < 0 and not logged("looking for")
    r.close()
    d.close()


# ---- where an image's title VOBs start ----

def test_the_ifo_header_s_place_is_kept_when_it_starts_with_the_vob_s_first_nav_pack():
    # the IFO header points at a copy of the title VOBs placed before the
    # file system's VTS_01_1.VOB, whose first sector is no navigation pack
    v = vts(vob_sectors=10)
    files, ifo = disc_files(v)
    files = dict(files)
    real = files["VTS_01_1.VOB"]
    files["VTS_01_1.VOB"] = ZERO + real[S:]
    order = ["VIDEO_TS.IFO", "VIDEO_TS.BUP", "VTS_01_0.IFO", "VTS_01_9.CPY", "VTS_01_1.VOB", "VTS_01_0.BUP"]
    files["VTS_01_9.CPY"] = real
    im, where = image([(n, files[n]) for n in order])
    ifo = bytearray(ifo)
    rel = where["VTS_01_9.CPY"] - where["VTS_01_0.IFO"]
    ifo[0xc4:0xc8] = rel.to_bytes(4, "big")
    for n in ("VTS_01_0.IFO", "VTS_01_0.BUP"):
        im.put(where[n], bytes(ifo))
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    assert d.ret == 0 and d.ifo(1).title_vob_start == where["VTS_01_9.CPY"]
    assert logged(f"the IFO header puts the title VOBs at sector {where['VTS_01_9.CPY']} (vtstt_vobs {rel}), the file "
                  f"system's VTS_01_1.VOB is at sector {where['VTS_01_1.VOB']}: they are read from the IFO header's: "
                  "its first sector is a VOB's first navigation pack")
    r = Reading(d)
    buf = ffi.new("uint8_t[]", S)
    assert lib.ff_dvd_title_vob_read(d.d, d.ifo(1), r.vr, 0, buf, ffi.NULL) == 1
    assert bytes(ffi.buffer(buf, S)) == real[:S]
    r.close()
    d.close()


def test_without_a_vob_s_first_nav_pack_at_either_place_the_ifo_header_s_is_kept():
    files, ifo = disc_files()
    files = dict(files)
    files["VTS_01_1.VOB"] = ZERO + files["VTS_01_1.VOB"][S:]
    im, where = image(list(files.items()), gaps={"VTS_01_1.VOB": 3})
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    header = where["VTS_01_1.VOB"] - 3
    assert d.ret == 0 and d.ifo(1).title_vob_start == header
    assert logged("they are read from the IFO header's: neither place starts with a VOB's first navigation pack")
    d.close()


# ---- the file view: an image's title VOBs through its VOB files ----

def marked(k):
    """A usable sector telling which title VOB sector it is."""
    p = bytearray(plain())
    p[200] = k
    return bytes(p)


def udf_disc(vob, pieces, extra=(), bad=()):
    """A disc image with an ISO 9660 tree and a UDF 1.02 volume over the same
    sectors (UDF is read: recent and valid), title set 1 laid out as on a
    disc from sector 340: IFOs, the title VOBs vob (16 sectors, the IFO
    header's place), the BUP, then the extra regions [(sector offset after
    the BUP, data)]. The UDF volume records VTS_01_1.VOB in pieces [(sector
    offset in the title VOBs or ("x", extra index), sectors)]; bad: title
    VOB sectors that fail to read."""
    from synth.image import Img, Udf, iso, udf
    v = vts(vob_sectors=len(vob) // S)
    m = bytes(vmg())
    ifo = bytes(dvd.vts_ifo(v))
    im = Img()
    at, placed = 340, []
    for name, data in (("VIDEO_TS.IFO", m), ("VIDEO_TS.BUP", m), ("VTS_01_0.IFO", ifo)):
        placed.append((name, at, data))
        at += -(-len(data) // S)
    vob_at = at
    im.put(vob_at, vob)
    bup_at = vob_at + len(vob) // S
    placed.append(("VTS_01_0.BUP", bup_at, ifo))
    extra_at = [bup_at + -(-len(ifo) // S) + off for off, _ in extra]
    for (_, data), x in zip(extra, extra_at):
        im.put(x, data)
    for _, sector, data in placed:
        im.put(sector, data)
    runs = [((extra_at[p[1]] if isinstance(p, tuple) else vob_at + p), n) for p, n in pieces]
    entries = [(n, s, len(dt)) for n, s, dt in placed]
    nxt = iso(im, "ISO_LABEL", [("VIDEO_TS", entries + [("VTS_01_1.VOB", runs[0][0], sum(n for _, n in runs) * S)])])
    udf(im, nxt, Udf("UDF_DISC_LABEL", 0x0102, 2009, entries, [("VTS_01_1.VOB", [(s, n * S) for s, n in runs])]))
    im.bad.extend(vob_at + b for b in bad)
    return im, vob_at


def sixteen(zero=()):
    """Title VOBs of 16 sectors: a NAV pack, then marked sectors (zero: these all zeros)."""
    vob = bytearray(dvd.title_vobs(16, [dvd.nav(0, 16, None)]))
    for k in range(1, 16):
        vob[k * S:(k + 1) * S] = ZERO if k in zero else marked(k)
    return bytes(vob)


def use_view(r, extents):
    sectors = ffi.new("uint32_t[]", [a for a, _ in extents])
    counts = ffi.new("uint32_t[]", [n for _, n in extents])
    return lib.DVDUseTitleVOBView_rec(r.vr, sectors, counts, len(extents))


def test_a_sector_that_cannot_be_read_where_the_title_vobs_start_is_read_through_the_vob_files():
    # VTS_01_1.VOB: the title VOBs' first 8 sectors, then a copy of the
    # last 8 elsewhere; title VOB sector 12 cannot be read at the start place
    vob = sixteen()
    im, _ = udf_disc(vob, [(0, 8), (("x", 0), 8)], extra=[(0, vob[8 * S:])], bad=[12])
    d = Disc.image(im)
    assert d.ret == 0
    install_log(lib.AV_LOG_VERBOSE)
    r = Reading(d)
    assert r.read(12)[0] < 0, "no file view for this segment yet"
    assert use_view(r, [(0, 16)]) == 1
    buf = ffi.new("uint8_t[]", S)
    assert lib.ff_dvd_title_vob_read(d.d, d.ifo(1), r.vr, 12, buf, ffi.NULL) == 1
    assert bytes(ffi.buffer(buf, S)) == marked(12)
    assert logged("VTS_01: title VOB sector 12 cannot be read where the title VOBs start: read through the VOB files")
    r.close()
    d.close()


def test_the_file_view_is_not_used_when_its_bytes_differ_or_a_file_is_missing():
    vob = sixteen()
    # the copy's last sector differs from the start place's
    copy = bytearray(vob[8 * S:])
    copy[-S:] = marked(99)
    im, _ = udf_disc(vob, [(0, 8), (("x", 0), 8)], extra=[(0, bytes(copy))], bad=[12])
    d = Disc.image(im)
    r = Reading(d)
    assert use_view(r, [(0, 16)]) == 0
    assert use_view(r, [(0, 8)]) == 1, "a segment in the part both hold the same"
    r.close()
    d.close()
    # VTS_01_1.VOB holds only 8 of the 16 sectors and there is no VTS_01_2.VOB
    im, _ = udf_disc(vob, [(0, 8)])
    d = Disc.image(im)
    install_log(lib.AV_LOG_DEBUG)
    r = Reading(d)
    assert use_view(r, [(0, 8)]) == 0
    assert logged("VTS_01: no file view: VTS_01_2.VOB is missing")
    r.close()
    d.close()


def test_a_damaged_range_in_a_vob_file_in_pieces_names_the_file():
    # VTS_01_1.VOB in two pieces on the image: title VOB sectors 0-7 and
    # 10-15 (sectors 8-9 at the start place belong to no file); sectors 12-13
    # are zeros
    vob = sixteen(zero={8, 9, 12, 13})
    im, _ = udf_disc(vob, [(0, 8), (10, 6)])
    d = Disc.image(im)
    assert d.ret == 0
    r = Reading(d)
    got = [r.read(s) for s in range(16)]
    # sectors 8-9: no file holds them
    assert got[10][1] == (lib.DVD_DAMAGE_ZERO, 8, 2, "", -1, "", -1)
    # sectors 12-13: the file's bytes 10 and 11 sectors in (8 of the first piece, then 2 of the second)
    assert got[14][1] == (lib.DVD_DAMAGE_ZERO, 12, 2, "VTS_01_1.VOB", 10 * S, "VTS_01_1.VOB", 11 * S)
    r.close()
    d.close()


# ---- VOBUs ----

def test_a_nav_pack_read_for_the_title_list_goes_through_libdvdread_s_checks(tmp_path):
    # the title VOBs' NAV pack at sector 0 with pci_gi's reserved field
    # zero1 (byte 0x33) not zero
    path = folder_with(tmp_path, [plain()])
    vob = path / "VIDEO_TS" / "VTS_01_1.VOB"
    data = bytearray(vob.read_bytes())
    data[0x33] = 1
    vob.write_bytes(bytes(data))
    d = Disc.folder(path)
    before = findings(d)
    n = ffi.new("DVDNav *")
    assert lib.ff_dvd_nav_read(d.d, d.ifo(1), 0, n) == 1
    assert n.vobu_ea == 5 and n.next_vobu & 0x3fffffff == 0x3fffffff   # 6 sectors, the end of the cell
    assert findings(d) == (before[0] + 1, before[1])
    install_log(lib.AV_LOG_VERBOSE)
    lib.ff_dvd_reports_log(d.d)
    assert logged("navigation packet at sector 0: pci->pci_gi.zero1 == 0 does not hold: kept")
    # no NAV pack: recorded, not read again
    assert lib.ff_dvd_nav_read(d.d, d.ifo(1), 1, n) == 0
    d.close()


def test_a_sector_missing_from_the_vobu_address_map_is_said_as_an_ifo_finding(tmp_path):
    d = Disc.folder(folder_with(tmp_path, [plain()]))
    assert lib.ff_dvd_map_trusted(d.d, d.ifo(1))
    assert lib.ff_dvd_vobu_start_check(d.d, d.ifo(1), 0) == 1, "on the map"
    assert lib.ff_dvd_vobu_start_check(d.d, d.ifo(1), 1) == 0, "off the map, and no NAV pack there"
    assert not lib.ff_dvd_map_trusted(d.d, d.ifo(1))
    install_log(lib.AV_LOG_WARNING)
    lib.ff_dvd_reports_log(d.d)
    assert logged("IFO check: VTS_01_0.IFO VTS_VOBU_ADMAP: title VOB sector 1 starts no VOBU of the VOBU address map: "
                  "its VOBU starts are not trusted (the VOBs must be scanned)")
    d.close()


def test_menu_vobs_start_where_the_vob_s_first_nav_pack_is_when_header_and_file_system_disagree():
    # VTS_01_0.VOB (the title set's menu VOBs, a NAV pack first) 3 sectors
    # after where the IFO header's vtsm_vobs puts them
    files, ifo = disc_files()
    files = dict(files)
    menu = bytes(dvd.title_vobs(4, [dvd.nav(0, 4, None)]))
    order = ["VIDEO_TS.IFO", "VIDEO_TS.BUP", "VTS_01_0.IFO", "VTS_01_0.VOB", "VTS_01_1.VOB", "VTS_01_0.BUP"]
    files["VTS_01_0.VOB"] = menu
    im, where = image([(n, files[n]) for n in order], gaps={"VTS_01_0.VOB": 3})
    ifo = bytearray(ifo)
    at = where["VTS_01_0.IFO"]
    ifo[0xc0:0xc4] = (where["VTS_01_0.VOB"] - 3 - at).to_bytes(4, "big")     # vtsm_vobs: 3 sectors early
    ifo[0xc4:0xc8] = (where["VTS_01_1.VOB"] - at).to_bytes(4, "big")         # vtstt_vobs: right
    for n in ("VTS_01_0.IFO", "VTS_01_0.BUP"):
        im.put(where[n], bytes(ifo))
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    assert d.ret == 0
    assert d.ifo(1).menu_vob_base == where["VTS_01_0.VOB"] and d.ifo(1).title_vob_base == where["VTS_01_1.VOB"]
    assert logged(f"IFO check: VTS_01_0.IFO VTSI_MAT at byte 0xc0 (bytes 00 00 00 {where['VTS_01_0.VOB'] - 3 - at:02x}): "
                  f"the IFO header puts the menu VOBs at sector {where['VTS_01_0.VOB'] - 3} (vtsm_vobs "
                  f"{where['VTS_01_0.VOB'] - 3 - at}), the file system's VTS_01_0.VOB is at sector "
                  f"{where['VTS_01_0.VOB']}: they are read from the file system's: its first sector is a VOB's first "
                  "navigation pack")
    d.close()
