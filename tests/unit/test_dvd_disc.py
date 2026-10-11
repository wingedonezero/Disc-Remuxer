"""libavformat/dvd_disc.c, dvd_ifo.c, dvddec.c: a DVD-Video disc's
structure (step D1): the VMG, the title table, the text data, every title
set's IFO with its BUP and checks; disc folders built with synth/dvd.py,
images (ISO 9660, sectors that cannot be read) with synth/image.py. With the
corpus (31 DVDs) the names and the IFO checks' warnings equal the reference
program's info, but for the BUP position of disc folders (checked by file
sizes here, which only one corpus folder fails), and every program chain,
cell and VOBU address map equals libdvdread's (implementation notes)."""

import json
import struct
import subprocess

import pytest

from helpers import LOG, PROGRAM, ffi, install_log, lib, logged
from helpers.disc import Image
from synth import dvd
from synth.dvd import Cell, Pgc, Title, Vts
from synth.image import Img, iso

S = 2048


def be16(d, at, v):
    d[at:at + 2] = struct.pack(">H", v)


def be32(d, at, v):
    d[at:at + 4] = struct.pack(">I", v)


def vts(**kw):
    """A title set of one program chain with one cell of 10 sectors."""
    v = dict(pgcs=[Pgc(cells=[Cell.new(0, 0, 9, 1)])], ptts=[[(1, 1)]], vobus=[0], vob_sectors=10)
    v.update(kw)
    return Vts(**v)


def vmg(titles=None, nr_vts=1, provider=b"", text=None):
    """VIDEO_TS.IFO; text: text data (TXTDT_MG) added after its tables."""
    d = bytearray(dvd.vmg_ifo(titles or [Title(1, 1, 1)], nr_vts))
    d[0x40:0x40 + len(provider)] = provider
    if text is not None:
        at = len(d) // S
        d += bytes(text) + bytes(-len(text) % S)
        be32(d, 0xd4, at)
        be32(d, 0x1c, len(d) // S - 1)          # vmgi_last_sector
        be32(d, 0x0c, 2 * len(d) // S - 1)      # vmg_last_sector: IFO + BUP
    return d


def txtdt(texts, charset=0x01):
    """Text data of one language unit: the disc's node (kind 1) with texts
    [(kind, bytes)], each ending with a TAB."""
    X = 0x20
    n = 1 + len(texts)
    entries, strings = bytearray([1, 0, 0, 0]), bytearray()
    for kind, t in texts:
        pos = X + 0xd0 + 4 * n + len(strings)
        entries += bytes([kind, 0]) + struct.pack(">H", pos - X - 0xcc)
        strings += t + b"\t"
    lu = bytearray(0xd0)
    be16(lu, 0xcc, n)
    body = bytearray(X) + lu + entries + strings
    be16(body, 0x0e, 1)
    be32(body, 0x10, len(body) - 1)
    body[0x14:0x16] = b"en"
    body[0x17] = charset
    be32(body, 0x18, X)
    be16(body, X + 2, len(body) - X - 1)
    return body


def folder(tmp_path, v=None, m=None):
    """A disc folder with VIDEO_TS and one title set; returns its path."""
    v = v or vts()
    path = tmp_path / "disc"
    navs = [dvd.nav(0, v.vob_sectors, None)]
    dvd.write_disc(path, m or vmg(), [(dvd.vts_ifo(v), dvd.title_vobs(v.vob_sectors, navs))])
    return path


class Disc:
    """ff_dvd_disc_open() on a folder or an image."""

    def __init__(self, fs, folder_, label=None, keep=None, dir_=b"/VIDEO_TS"):
        self.fs = fs
        self._keep = keep
        self.d = ffi.new("DVDDisc *")
        self.ret = lib.ff_dvd_disc_open(ffi.NULL, fs, dir_, folder_, ffi.NULL if label is None else label, self.d)

    @classmethod
    def folder(cls, path, dir_=b"/VIDEO_TS"):
        fs = ffi.new("DiscIOFS **")
        assert lib.ff_discio_mount_folder(ffi.NULL, str(path).encode(), 5, fs) == 0
        return cls(fs[0], 1, dir_=dir_)

    @classmethod
    def image(cls, im):
        img = Image.memory(im)
        vol = img.mount()
        assert vol != 0 and not isinstance(vol, int)
        return cls(vol.fs, 0, label=ffi.string(vol.fs.label), keep=(img, vol))

    def ifo(self, n):
        return self.d.ifos[n]

    def name(self):
        return ffi.string(self.d.volume_name).decode()

    def close(self):
        lib.ff_dvd_disc_free(self.d)
        if self._keep is None:
            lib.ff_discio_fs_close(ffi.new("DiscIOFS **", self.fs))
        else:
            self._keep[1].close()
            self._keep[0].close()


def warnings():
    return [line for line in LOG if "DVD:" in line]


# ---- folders ----

def test_a_disc_folder_is_read_and_named_by_its_provider_identifier(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(provider=b"KANGAROO_JACK")))
    assert d.ret == 0
    assert d.d.nb_ifos == 2 and d.d.nr_of_title_sets == 1
    assert not d.ifo(0).failed and not d.ifo(1).failed
    assert d.ifo(1).cache != ffi.NULL, "the whole IFO read once"
    assert d.ifo(1).nb_pgcs == 1 and d.ifo(1).nb_vobu_map == 1
    assert d.name() == "KANGAROO_JACK"
    assert d.d.text_name == ffi.NULL
    assert warnings() == [], "a consistent disc: no warning"
    d.close()


def test_a_wrong_ifo_signature_reads_the_bup(tmp_path):
    path = folder(tmp_path)
    ifo = path / "VIDEO_TS" / "VTS_01_0.IFO"
    b = bytearray(ifo.read_bytes())
    b[:12] = b"DVDVIDEO-XXX"
    ifo.write_bytes(b)
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(path)
    assert d.ret == 0 and not d.ifo(1).failed and d.ifo(1).bup_only
    assert logged("VTS_01.IFO does not start with DVDVIDEO-VTS: its BUP is read instead")
    d.close()


def test_a_title_set_whose_ifo_and_bup_are_both_wrong_stays_a_placeholder(tmp_path):
    path = folder(tmp_path)
    for ext in ("IFO", "BUP"):
        f = path / "VIDEO_TS" / f"VTS_01_0.{ext}"
        b = bytearray(f.read_bytes())
        b[:12] = b"DVDVIDEO-XXX"
        f.write_bytes(b)
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(path)
    assert d.ret == 0, "the disc goes on without the title set"
    assert d.ifo(1).failed
    assert logged("neither the IFO nor the BUP starts with DVDVIDEO-VTS")
    assert logged("title set 1 cannot be read: the disc is read without it")
    d.close()


def test_a_vmg_that_cannot_be_read_is_no_dvd(tmp_path):
    path = folder(tmp_path)
    for ext in ("IFO", "BUP"):
        f = path / "VIDEO_TS" / f"VIDEO_TS.{ext}"
        b = bytearray(f.read_bytes())
        b[:12] = b"DVDVIDEO-XXX"
        f.write_bytes(b)
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(path)
    assert d.ret < 0
    assert logged("VIDEO_TS.IFO cannot be read: no DVD-Video")
    d.close()


def test_a_missing_ifo_is_read_from_its_bup(tmp_path):
    path = folder(tmp_path)
    (path / "VIDEO_TS" / "VTS_01_0.IFO").unlink()
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(path)
    assert d.ret == 0 and not d.ifo(1).failed
    assert logged("VTS_01.IFO is missing: its BUP is read as the IFO")
    d.close()


def test_the_bup_position_of_a_folder_is_checked_by_the_file_sizes(tmp_path):
    v = vts()
    ifo = bytearray(dvd.vts_ifo(v))
    be32(ifo, 0x0c, struct.unpack(">I", ifo[0x0c:0x10])[0] + 16)   # vts_last_sector 16 sectors more
    path = tmp_path / "disc"
    dvd.write_disc(path, vmg(), [(ifo, dvd.title_vobs(v.vob_sectors, [dvd.nav(0, 10, None)]))])
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(path)
    assert d.ret == 0
    ifo_sectors = len(ifo) // S
    assert logged(f"VTS_01: the header puts the BUP {ifo_sectors + 10 + 16} sectors after the IFO")
    assert logged(f"the IFO and VOB files make {ifo_sectors + 10} sectors")
    d.close()


def test_a_title_in_a_title_set_the_disc_does_not_have_refuses_the_disc(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(titles=[Title(1, 1, 1), Title(2, 1, 1)])))
    assert d.ret < 0
    assert logged("title 2 of the title table is in title set 2, which the disc does not have (it has 1)")
    d.close()


def test_broken_title_table_entries_are_not_checked(tmp_path):
    # no chapters / no title number in the set: the entry is skipped
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(titles=[Title(1, 1, 1), Title(5, 1, 0), Title(6, 0, 1)])))
    assert d.ret == 0 and warnings() == []
    d.close()


def test_title_set_starts_that_disagree_are_reported(tmp_path):
    titles = [Title(1, 1, 1, title_set_starting_sector=7), Title(1, 1, 1, title_set_starting_sector=9)]
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(titles=titles)))
    assert d.ret == 0
    assert logged("the title table starts title set 1 at sector 9 (title 2) and at 7")
    # the folder's own start by the VMG header differs from both
    assert logged("title set 1 starts at sector")
    d.close()


def test_more_than_99_title_sets_refuse_the_disc(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(nr_vts=100)))
    assert d.ret < 0 and logged("the disc has 100 title sets, more than 99")
    d.close()


def test_a_cell_off_the_vobu_address_map_makes_it_untrusted(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, v=vts(vobus=[1])))
    assert d.ret == 0 and d.ifo(1).vobu_map_distrusted
    assert logged("program chain 1 cell 1: sector 0 is no VOBU start of the VOBU address map")
    d.close()


def test_a_dvdfab_provider_identifier_is_reported(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(provider=b"DVD(Fab)")))
    assert d.ret == 0 and logged("processed by DVDFab / MacTheRipper")
    d.close()


def test_a_program_chain_with_more_than_128_commands_is_left_out(tmp_path):
    pgcs = [Pgc(cells=[Cell.new(0, 0, 9, 1)], pre=[bytes(8)] * 129), Pgc(cells=[Cell.new(0, 0, 9, 1)])]
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, v=vts(pgcs=pgcs)))
    assert d.ret == 0 and d.ifo(1).nb_pgcs == 2
    assert logged("program chain 1 has 129 commands (more than 128): it is left out")
    d.close()


def test_a_program_chain_starting_where_another_starts_is_left_out(tmp_path):
    v = vts(pgcs=[Pgc(cells=[Cell.new(0, 0, 9, 1)]), Pgc(cells=[Cell.new(0, 0, 9, 1)])])
    ifo = bytearray(dvd.vts_ifo(v))
    pgcit = struct.unpack(">I", ifo[0xcc:0xd0])[0] * S
    ifo[pgcit + 0x14:pgcit + 0x18] = ifo[pgcit + 0x0c:pgcit + 0x10]    # search pointer 2 = pointer 1
    path = tmp_path / "disc"
    dvd.write_disc(path, vmg(), [(ifo, dvd.title_vobs(10, [dvd.nav(0, 10, None)]))])
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(path)
    assert d.ret == 0 and d.ifo(1).pgc_same_as[1] == 0
    assert logged("program chain 2 starts where program chain 1 does: it is left out")
    d.close()


def two_pgc_disc(tmp_path, patch):
    """A title set with two program chains; patch(ifo, pgcit, second) changes
    its IFO (and BUP): pgcit = the table's byte offset, second = program
    chain 2's offset in the table."""
    v = vts(pgcs=[Pgc(cells=[Cell.new(0, 0, 9, 1)]), Pgc(cells=[Cell.new(0, 0, 9, 1)], programs=[1])])
    ifo = bytearray(dvd.vts_ifo(v))
    pgcit = struct.unpack(">I", ifo[0xcc:0xd0])[0] * S
    second = struct.unpack(">I", ifo[pgcit + 0x14:pgcit + 0x18])[0]
    patch(ifo, pgcit, second)
    path = tmp_path / "disc"
    dvd.write_disc(path, vmg(), [(ifo, dvd.title_vobs(10, [dvd.nav(0, 10, None)]))])
    return path


def table_bytes(ifo, pgcit):
    """The table's bytes the title list reads: whole sectors up to its last byte."""
    return (struct.unpack(">I", ifo[pgcit + 4:pgcit + 8])[0] & 0xfffff800) + S


def test_a_program_chain_starting_outside_the_table_is_left_out_and_reported_with_its_place(tmp_path):
    def patch(ifo, pgcit, second):
        last = struct.unpack(">I", ifo[pgcit + 4:pgcit + 8])[0]
        ifo[pgcit + 0x14:pgcit + 0x18] = struct.pack(">I", last + 8)
        patch.at = pgcit + 0x14
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(two_pgc_disc(tmp_path, patch))
    assert d.ret == 0 and d.ifo(1).nb_pgcs == 2 and d.ifo(1).pgcs[1] == ffi.NULL
    assert logged(f"IFO check: VTS_01_0.IFO VTS_PGCIT at byte {patch.at:#x}: program chain 2 starts outside "
                  "VTS_PGCIT")
    d.close()


def test_a_program_chain_whose_command_table_runs_past_the_table_is_left_out(tmp_path):
    def patch(ifo, pgcit, second):
        # its command table's header ends past the table's last sector
        ct = table_bytes(ifo, pgcit) - second - 4
        ifo[pgcit + second + 0xe4:pgcit + second + 0xe6] = struct.pack(">H", ct)
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(two_pgc_disc(tmp_path, patch))
    assert d.ret == 0 and d.ifo(1).pgcs[0] != ffi.NULL and d.ifo(1).pgcs[1] == ffi.NULL
    assert logged("program chain 2 runs past the end of VTS_PGCIT: it is left out")
    d.close()


def test_fields_past_the_table_read_as_0_and_the_program_chain_stays(tmp_path):
    def patch(ifo, pgcit, second):
        # its program map lies past the table's last sector
        pm = table_bytes(ifo, pgcit) - second + 16
        ifo[pgcit + second + 0xe6:pgcit + second + 0xe8] = struct.pack(">H", pm)
        ifo[pgcit + second + pm:pgcit + second + pm + 1] = b"\x07"   # in the file, but not read
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(two_pgc_disc(tmp_path, patch))
    p = d.ifo(1).pgcs[1]
    assert d.ret == 0 and p != ffi.NULL and p.nr_of_programs == 1 and p.program_map[0] == 0
    assert logged("program chain 2: fields past the table's end are read as 0")
    d.close()


def test_a_cell_table_pointer_of_0_reads_the_cells_from_the_program_chain_s_start(tmp_path):
    # as the title list's reference reads them; looked into again later
    def patch(ifo, pgcit, second):
        ifo[pgcit + second + 0xe8:pgcit + second + 0xea] = bytes(2)
        patch.first = struct.unpack(">I", ifo[pgcit + second + 8:pgcit + second + 12])[0]
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(two_pgc_disc(tmp_path, patch))
    p = d.ifo(1).pgcs[1]
    assert d.ret == 0 and p != ffi.NULL and p.cells[0].first_sector == patch.first
    assert logged("program chain 2 has 1 cell(s) but its cell playback table pointer is 0: they are read from the "
                  "program chain's start")
    d.close()


def test_an_unreadable_menu_vobu_map_leaves_the_title_set_s_vobu_starts_untrusted(tmp_path):
    # either VOBU address map of a title set that cannot be read: its VOBU
    # starts are checked from the VOBs (the map past the IFO's end here)
    ifo = bytearray(dvd.vts_ifo(vts()))
    ifo[0xdc:0xe0] = struct.pack(">I", 0x7fff)
    path = tmp_path / "disc"
    dvd.write_disc(path, vmg(), [(ifo, dvd.title_vobs(10, [dvd.nav(0, 10, None)]))])
    install_log(lib.AV_LOG_VERBOSE)
    d = Disc.folder(path)
    assert d.ret == 0 and not d.ifo(1).failed and d.ifo(1).vobu_map_distrusted
    assert logged("VTS_01: the VOBU address map at sector 32767 cannot be read: its VOBU starts are not trusted")
    assert logged(f"IFO check: VTS_01_0.IFO VTSM_VOBU_ADMAP at byte {0x7fff * S:#x}: VTSM_VOBU_ADMAP at sector 32767 "
                  "cannot be read")
    d.close()


def test_the_text_data_names_the_disc(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(text=txtdt([(0x38, b"Other"), (0x30, b"DVD Demystified 2.0")]))))
    assert d.ret == 0
    assert ffi.string(d.d.text_name) == b"DVD Demystified 2.0", "item 0x30 first"
    assert warnings() == []
    d.close()


def test_text_bytes_outside_0x20_to_0x7a_are_dropped_and_logged(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(text=txtdt([(0x30, b"Caf\xe9 {1}")]))))
    assert ffi.string(d.d.text_name) == b"Caf 1"
    assert logged('text data item 0x30: 3 byte(s) outside 0x20-0x7A dropped (kept: "Caf 1")')
    d.close()


def test_text_data_in_another_character_set_is_not_used(tmp_path):
    install_log(lib.AV_LOG_WARNING)
    d = Disc.folder(folder(tmp_path, m=vmg(text=txtdt([(0x30, b"Name")], charset=0x02))))
    assert d.ret == 0 and d.d.text_name == ffi.NULL
    assert logged("text data in character set 0x02 is not read")
    d.close()


# ---- images ----

def image(files, label="DVD_LABEL", gaps=None, bad=()):
    """An ISO 9660 image with VIDEO_TS holding files [(name, bytes)] one after
    the other from sector 40 (gaps: {name: sectors left free before it});
    bad: sectors (of names: "VTS_01_0.IFO+1" = its second sector) that fail."""
    im = Img()
    at, entries, where = 40, [], {}
    for name, data in files:
        at += (gaps or {}).get(name, 0)
        n = -(-len(data) // S)
        if (at + n) * S > len(im.d):
            im.d += bytes((at + n) * S - len(im.d))
        im.put(at, data)
        entries.append((name, at, len(data)))
        where[name] = at
        at += n
    iso(im, label, [("VIDEO_TS", entries)])
    for b in bad:
        name, plus = b.split("+")
        im.bad.append(where[name] + int(plus))
    return im, where


def disc_files(v=None, bup_ifo=None):
    v = v or vts()
    m, ifo = bytes(vmg()), bytes(dvd.vts_ifo(v))
    vob = bytes(dvd.title_vobs(v.vob_sectors, [dvd.nav(0, v.vob_sectors, None)]))
    return [("VIDEO_TS.IFO", m), ("VIDEO_TS.BUP", m), ("VTS_01_0.IFO", ifo), ("VTS_01_1.VOB", vob),
            ("VTS_01_0.BUP", bup_ifo or ifo)], ifo


def test_an_image_is_named_by_its_label_and_finds_its_title_vobs():
    files, ifo = disc_files()
    im, where = image(files)
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    assert d.ret == 0 and d.name() == "DVD_LABEL"
    f = d.ifo(1)
    assert f.ifo_sector == where["VTS_01_0.IFO"] and f.bup_sector == where["VTS_01_0.BUP"]
    assert f.title_vob_start == where["VTS_01_1.VOB"], "IFO position + vtstt_vobs, the file system agreeing"
    assert f.title_vob_blocks == 10
    assert warnings() == []
    d.close()


def test_an_unreadable_ifo_piece_is_read_from_the_bup():
    files, _ = disc_files()
    im, _ = image(files, bad=["VTS_01_0.IFO+0"])
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    assert d.ret == 0 and not d.ifo(1).failed
    assert logged("VTS_01: IFO bytes 0-1023 read from the BUP")
    # the whole IFO: the first try (IFO, whole) fails, the second (BUP, one
    # sector) reads, and the copy that read is kept
    assert logged("reading the whole IFO") and logged("from the IFO failed") and logged("try 1 of 16")
    assert not logged("try 2 of 16")
    assert d.ifo(1).cache != ffi.NULL
    d.close()


def test_an_ifo_table_unreadable_in_both_copies_leaves_the_title_set_out():
    files, _ = disc_files()
    im, _ = image(files, bad=["VTS_01_0.IFO+1", "VTS_01_0.BUP+1"])
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    assert d.ret == 0 and d.ifo(1).failed
    assert logged("try 16 of 16"), "16 tries, alternating"
    assert logged("cannot be read: its tables are read piece by piece")
    assert logged("title set 1 cannot be read")
    d.close()


def test_the_bup_position_of_an_image_comes_from_the_file_system():
    files, ifo = disc_files()
    im, _ = image(files, gaps={"VTS_01_0.BUP": 16})
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    n = len(ifo) // S + 10
    assert logged(f"VTS_01: the header puts the BUP {n} sectors after the IFO")
    assert logged(f"the file system puts it {n + 16} sectors")
    d.close()


def test_title_vobs_the_header_and_the_file_system_disagree_on_are_decided_later():
    files, ifo = disc_files()
    im, where = image(files, gaps={"VTS_01_1.VOB": 3})
    install_log(lib.AV_LOG_WARNING)
    d = Disc.image(im)
    assert d.ret == 0 and d.ifo(1).title_vob_start == 0
    assert logged(f"the file system at {where['VTS_01_1.VOB']}: decided when they are read")
    d.close()


# ---- the program ----

def info(path):
    out = subprocess.run([*PROGRAM, "--json", "info", str(path)], capture_output=True, text=True)
    lines = [json.loads(line) for line in out.stdout.splitlines() if line.startswith("{")]
    return [x for x in lines if x.get("type") == "disc"], lines


def test_info_shows_a_dvd_folder(tmp_path):
    discs, _ = info(folder(tmp_path, m=vmg(provider=b"MERLIN")))
    assert discs and (discs[0]["kind"], discs[0]["label"], discs[0]["titles"]) == ("DVD-Video", "MERLIN", 0)


@pytest.mark.parametrize("name", ["VIDEO_TS", "Movie.dvdmedia"])
def test_a_video_ts_folder_or_a_dvdmedia_folder_opens_itself(tmp_path, name):
    path = folder(tmp_path, m=vmg(provider=b"MERLIN"))
    top = tmp_path / name
    (path / "VIDEO_TS").rename(top) if name != "VIDEO_TS" else None
    discs, _ = info(top if name != "VIDEO_TS" else path / "VIDEO_TS")
    assert discs and discs[0]["label"] == "MERLIN"


def test_a_folder_of_another_name_holding_the_files_is_no_disc(tmp_path):
    path = folder(tmp_path)
    (path / "VIDEO_TS").rename(tmp_path / "files")
    discs, lines = info(tmp_path / "files")
    assert not discs
    assert any("holds no disc" in x.get("text", "") for x in lines)
