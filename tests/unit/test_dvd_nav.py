"""libavformat/dvd_nav.c: libdvdnav on the disc as the disc layer reads it
(step D2). libdvdread reads a virtual VIDEO_TS folder served from the
disc's structure: the IFOs as dvd_ifo.c read them (the BUP name gives the
same bytes, reads past the IFO's size give zeros), the VOBs from the disc
folder's files (title VOB files joined as bytes) or the image's sectors the
IFO header gives. A title then plays as when libdvdread opens the folder
itself."""

from helpers import LOG, install_log, lib, logged
from helpers.dvdnav import Pgc, build, cell, cells, dvd, navs, stopped, trace, trace_disc, vobus
from synth.image import Img, iso
from test_dvd_disc import Disc

S = 2048


def two_cells(tmp_path):
    p = Pgc(cells=[cell(0, 9), cell(10, 19)], programs=[1, 2])
    return build(tmp_path, [p], [[(1, 1), (1, 2)]], vobus(0, 9, 5) + vobus(10, 19, 5), 20)


def files_of(folder):
    v = folder / "VIDEO_TS"
    return [(p.name, p.read_bytes()) for p in sorted(v.iterdir())]


def image_of(files, order):
    """An ISO 9660 image with VIDEO_TS holding files in order, one after the
    other from sector 40."""
    data = dict(files)
    im, at, entries = Img(), 40, []
    for name in order:
        d = data[name]
        n = -(-len(d) // S)
        if (at + n) * S > len(im.d):
            im.d += bytes((at + n) * S - len(im.d))
        im.put(at, d)
        entries.append((name, at, len(d)))
        at += n
    iso(im, "NAV_TEST", [("VIDEO_TS", entries)])
    return im


ORDER = ["VIDEO_TS.IFO", "VIDEO_TS.BUP", "VTS_01_0.IFO", "VTS_01_1.VOB", "VTS_01_0.BUP"]


def test_a_title_plays_through_the_disc_layer_as_through_libdvdread_s_own_reader(tmp_path):
    folder = two_cells(tmp_path)
    want = trace(folder, 1)[1]
    d = Disc.folder(folder)
    res, ev = trace_disc(d.d, 1)
    assert res.status == 0 and stopped(res, ev)
    assert (cells(ev), navs(ev)) == (cells(want), navs(want)) == ([(1, 1), (1, 2)], [0, 5, 10, 15])
    assert res.data_blocks == 16
    d.close()


def test_an_image_plays_its_title_vobs_from_where_the_ifo_header_puts_them(tmp_path):
    folder = two_cells(tmp_path)
    d = Disc.image(image_of(files_of(folder), ORDER))
    assert d.ret == 0
    res, ev = trace_disc(d.d, 1)
    assert cells(ev) == [(1, 1), (1, 2)] and navs(ev) == [0, 5, 10, 15]
    assert stopped(res, ev) and res.data_blocks == 16
    d.close()


def test_a_folder_s_title_vob_files_are_joined_as_bytes(tmp_path):
    # VTS_01_1.VOB split into two files 100 bytes into sector 12: libdvdread
    # on its own would read each file as whole sectors
    folder = two_cells(tmp_path)
    v = folder / "VIDEO_TS"
    vob = (v / "VTS_01_1.VOB").read_bytes()
    cut = 12 * S + 100
    (v / "VTS_01_1.VOB").write_bytes(vob[:cut])
    (v / "VTS_01_2.VOB").write_bytes(vob[cut:])
    assert navs(trace(folder, 1)[1]) != [0, 5, 10, 15], "libdvdread on its own misreads the second file"
    d = Disc.folder(folder)
    res, ev = trace_disc(d.d, 1)
    assert navs(ev) == [0, 5, 10, 15] and stopped(res, ev)
    d.close()


def test_navigation_reads_past_the_ifo_s_size_get_zeros(tmp_path):
    # the first-play program chain lies past VIDEO_TS.IFO's size: read as a
    # chain of zeros (libdvdread on its own fails the IFO, and its BUP)
    folder = two_cells(tmp_path)
    ifo = folder / "VIDEO_TS" / "VIDEO_TS.IFO"
    b = bytearray(ifo.read_bytes())
    b[0x84:0x88] = (len(b) + 0x10).to_bytes(4, "big")
    ifo.write_bytes(b)
    (folder / "VIDEO_TS" / "VIDEO_TS.BUP").write_bytes(b)
    assert trace(folder, 1)[0].status == -1, "libdvdread's own reader: the disc does not open"
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder)
    res, ev = trace_disc(d.d, 1)
    assert res.status != -1, "the navigator opens"
    assert logged("navigation reads VIDEO_TS.IFO bytes") and logged("zeros given [untested on real discs]")
    d.close()


def test_a_title_set_that_could_not_be_loaded_has_no_files(tmp_path):
    folder = two_cells(tmp_path)
    for name in ("VTS_01_0.IFO", "VTS_01_0.BUP"):
        p = folder / "VIDEO_TS" / name
        b = bytearray(p.read_bytes())
        b[:12] = b"NOTADVDVIDEO"
        p.write_bytes(b)
    d = Disc.folder(folder)
    assert d.ifo(1).failed
    res, ev = trace_disc(d.d, 1)
    assert cells(ev) == [], "no title set to play"
    d.close()


# ---- the corpus ----

def corpus_discs(root):
    """Disc folders (holding VIDEO_TS) and .iso images under root."""
    import pathlib
    out = []
    for p in sorted(pathlib.Path(root).rglob("*")):
        if p.is_dir() and (p / "VIDEO_TS").is_dir():
            out.append(p)
        elif p.is_file() and p.suffix.lower() == ".iso":
            out.append(p)
    return out


def open_disc(path):
    from helpers.disc import Image
    if path.is_dir():
        return Disc.folder(path)
    img = Image.open(str(path))
    vol = img.mount()
    return Disc(vol.fs, 0, label=bytes(ffi_string(vol.fs.label)), keep=(img, vol))


def ffi_string(p):
    from helpers import ffi
    return ffi.string(p)


def test_corpus_titles_play_the_same_through_the_disc_layer():
    # every title of every corpus DVD, navigation only, the player's way
    # (pre-command jumps followed): the same cells and NAV packets as
    # libdvdread reading the disc itself
    from helpers.disc import env_or_skip
    root = env_or_skip("DVD_CORPUS")
    opts = lib.DR_NAV_NAV_ONLY | lib.DR_NAV_FOLLOW_JUMPS
    differ = []
    for path in corpus_discs(root):
        d = open_disc(path)
        assert d.ret == 0, path
        for t in range(1, 100):
            ours = trace_disc(d.d, t, opts, 30000)
            theirs = trace(path, t, opts, 30000)
            if ours[0].status == theirs[0].status == -2:
                continue
            a = (ours[0].status, cells(ours[1]), navs(ours[1]))
            b = (theirs[0].status, cells(theirs[1]), navs(theirs[1]))
            if a != b:
                differ.append((path.name, t))
        d.close()
    assert differ == []


def test_a_duplicate_of_a_program_chain_libdvdread_cannot_read_is_left_out(tmp_path):
    # program chains 2 and 3 of the title set share a start far past the IFO
    # (beyond what the navigator's files report): libdvdread cannot read the
    # first and took a reference on it through NULL (the process crashed)
    p = Pgc(cells=[cell(0, 9)])
    folder = build(tmp_path, [p, Pgc(cells=[cell(0, 9)]), Pgc(cells=[cell(0, 9)])], [[(1, 1)]], vobus(0, 9), 10)
    for name in ("VTS_01_0.IFO", "VTS_01_0.BUP"):
        f = folder / "VIDEO_TS" / name
        b = bytearray(f.read_bytes())
        pgcit = int.from_bytes(b[0xcc:0xd0], "big") * S
        for k in (1, 2):
            b[pgcit + 8 + 8 * k + 4:pgcit + 8 + 8 * k + 8] = (0xf0000000).to_bytes(4, "big")
        f.write_bytes(b)
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(folder)
    res, ev = trace_disc(d.d, 1)
    assert cells(ev) == [(1, 1)] and stopped(res, ev)
    assert any("duplicate of PGC 2" in line for line in LOG)
    d.close()
