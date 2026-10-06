"""libavformat/hddvd_xpl.c: the HD DVD playlists. The parsing rules on built
XML; the loader (file numbering, AACS header, files left out) on built ISO
9660 images; with HDDVD_CORPUS (a folder of HD DVD images) and
HDDVD_XPL_REFERENCE (dumps of their VPLST000.XPL made by an independent
reader, one <image name>.VPLST000.txt each, every character but A-Z a-z 0-9 _
of the name replaced by _) the dumps must be equal."""

import pathlib
import re
import struct

import pytest

from helpers import EINVAL, averror, ffi, install_log, lib, logged
from helpers.disc import env_or_skip
from helpers.hddvd import take_text
from helpers.source import Memory, Source
from synth.image import S, Img, iso

INVALIDDATA = -0x41444E49


# ---- parsing from memory ----

def dump(x):
    d = lib.ff_hddvd_xpl_dump(x)
    assert d != ffi.NULL
    return take_text(d)


def parse(xml):
    """Parse xml whole: its dump, or the negative error code."""
    data = xml.encode()
    return parse_at(data, 0, len(data))[0]


def parse_at(data, offset, length):
    """(dump or error code, the reads made as (pos, length))."""
    install_log(lib.AV_LOG_DEBUG)
    reads = []

    def read(pos, n):
        reads.append((pos, n))
        if pos + n > len(data):
            return averror(EINVAL)
        return bytes(data[pos:pos + n])
    h = ffi.new_handle(read)
    x = ffi.new("HDDVDXpl **")
    ret = lib.ff_hddvd_xpl_parse(ffi.NULL, lib.tb_py_hddvd_read, h, offset, length, x)
    if ret < 0:
        assert x[0] == ffi.NULL
        return ret, reads
    d = dump(x[0])
    lib.ff_hddvd_xpl_free(x)
    return d, reads


PLAYLIST = 'Playlist description="" displayName="" majorVersion=0 minorVersion=0 type="Advanced"'
TITLESET = 'TitleSet defaultLanguage="" tickBase="" timeBase=""'
TITLE = ('Title alternativeSDDisplayMode="panscanOrLetterbox" base="" description="" displayName="" id="" '
         'onEnd="" parentalLevel="*:1" selectable=1 tickBaseDivisor="1" titleDuration="" titleNumber="" '
         'type="Advanced"')


# ---- rules ----

def test_every_attribute_of_a_type_starts_at_its_default():
    d = parse("<Playlist><TitleSet><Title><PrimaryAudioVideoClip/><ChapterList><Chapter/></ChapterList>"
              "</Title></TitleSet></Playlist>")
    want = (f"{PLAYLIST}\n  {TITLESET}\n    {TITLE}\n"
            '      PrimaryAudioVideoClip clipTimeBegin="00:00:00:00" dataSource="Disc" description="" id="" '
            'seamless=0 src="" titleTimeBegin="" titleTimeEnd=""\n      ChapterList\n        '
            'Chapter description="" displayName="" id="" titleTimeBegin=""\n')
    assert d == want


def test_unknown_elements_are_skipped_with_their_content_and_unknown_attributes_ignored():
    d = parse('<Playlist><Unknown><TitleSet/></Unknown><TitleSet timeBase="60fps" other="x"><Title><Video/>'
              '</Title></TitleSet></Playlist>')
    # the TitleSet inside <Unknown> is not read, <Video> is no child of <Title>
    assert d == f'{PLAYLIST}\n  TitleSet defaultLanguage="" tickBase="" timeBase="60fps"\n    {TITLE}\n'
    assert logged("XPL: <Unknown> in <Playlist> is not read")


def test_names_are_matched_exactly_after_a_namespace_prefix():
    d = parse('<x:Playlist xmlns:x="urn:x"><x:TitleSet x:timeBase="50fps" TimeBase="no"/><titleset/></x:Playlist>')
    assert d == f'{PLAYLIST}\n  TitleSet defaultLanguage="" tickBase="" timeBase="50fps"\n'


def test_numbers_are_read_as_strtoul_and_booleans_are_true_or_yes():
    d = parse('<Playlist majorVersion=" 12abc" minorVersion="x"><TitleSet><Title selectable="1"/>'
              '<Title selectable="YES"/><Title selectable="True"/><Title selectable="false"/></TitleSet></Playlist>')
    assert d.startswith('Playlist description="" displayName="" majorVersion=12 minorVersion=0'), d
    sel = ["1" if "selectable=1" in line else "0" for line in d.splitlines() if "Title " in line]
    assert sel == ["0", "1", "1", "0"]
    # a number beyond 32 bits is cut to its low 32 bits, a repeated attribute is XML's error
    d = parse('<Playlist majorVersion="4294967297"/>')
    assert "majorVersion=1 " in d, d


def test_an_element_without_text_gets_its_last_childs_text_and_what_follows_it():
    d = parse("<Playlist><TitleSet>\n<Title>abc</Title>\n</TitleSet></Playlist>")
    # Title: "abc"; TitleSet and Playlist: "abc" and the newline after </Title>
    # (the buffer is emptied only when an element starts)
    assert d == f'{PLAYLIST} text="abc\n"\n  {TITLESET} text="abc\n"\n    {TITLE} text="abc"\n'
    # the start of an element empties the buffer
    d = parse("<Playlist>lost<TitleSet/></Playlist>")
    assert "lost" not in d, d


def test_text_inside_a_skipped_element_is_collected_and_text_stops_at_4096_bytes():
    d = parse("<Playlist><TitleSet><Skipped>in skipped</Skipped></TitleSet></Playlist>")
    assert 'TitleSet defaultLanguage="" tickBase="" timeBase="" text="in skipped"' in d, d
    d = parse(f"<Playlist><TitleSet>{'y' * 5000}</TitleSet></Playlist>")
    t = d.splitlines()[1]
    assert t.count("y") == 4096, t


def test_a_root_other_than_playlist_leaves_an_empty_playlist():
    d = parse("<Other><TitleSet/></Other>")
    assert d == "Playlist description=(null) displayName=(null) majorVersion=0 minorVersion=0 type=(null)\n"


def test_xml_that_is_not_well_formed_is_an_error():
    assert parse("<Playlist><TitleSet></Playlist>") == INVALIDDATA
    assert logged("XPL: not well-formed XML: expat error 7 (mismatched tag)")
    assert parse("") == INVALIDDATA
    assert logged("XPL: not well-formed XML: expat error 3 (no element found)")


def test_reads_are_at_most_16_kib_and_stay_inside_2048_byte_blocks_until_aligned():
    xml = f"<Playlist>{' ' * 40000}</Playlist>".encode()
    length = len(xml)
    data = bytes(0x11b) + xml
    r, reads = parse_at(data, 0x11b, length)
    assert not isinstance(r, int)
    want = [(0x11b, 0x800 - 0x11b)]
    pos = 0x800
    while pos < 0x11b + length:
        n = min(0x11b + length - pos, 0x4000)
        want.append((pos, n))
        pos += n
    assert reads == want


# ---- the loader on built images ----

def load(im):
    """Mounts im and loads its playlists: their file numbers, or the
    negative error code."""
    install_log(lib.AV_LOG_DEBUG)
    src = Source(Memory(im.d), name="memory")
    fs = ffi.new("DiscIOFS **")
    assert lib.ff_discio_mount_image(src.ptr, ffi.NULL, fs) == 0
    xs = ffi.new("HDDVDXpl ***")
    nb = ffi.new("int *")
    ret = lib.ff_hddvd_xpl_load(ffi.NULL, fs[0], xs, nb)
    if ret < 0:
        out = ret
    else:
        out = [xs[0][i].file for i in range(nb[0])]
        lib.ff_hddvd_xpl_free_all(xs, nb[0])
    lib.ff_discio_fs_close(fs)
    src.close()
    return out


def image(files):
    """An ISO 9660 image with files in ADV_OBJ (name, content), from sector 100."""
    im = Img()
    entries = []
    for k, (name, data) in enumerate(files):
        sector = 100 + 4 * k
        assert len(data) <= 4 * S
        im.put(sector, data)
        entries.append((name, sector, len(data)))
    iso(im, "HDDVD", [("ADV_OBJ", entries)])
    return im


def aacs(kind, xml):
    d = bytearray(0x11b)
    d[:4] = b"AACS"
    d[4] = kind
    d[7:11] = struct.pack(">I", len(xml))
    return bytes(d) + xml.encode() + b"trailing bytes after the XML"


XML = "<Playlist><TitleSet/></Playlist>"


def test_playlists_are_read_up_to_the_first_missing_file_and_bad_ones_left_out():
    im = image([
        ("VPLST000.XPL", XML.encode()),
        ("VPLST001.XPL", aacs(0x12, XML)),
        ("VPLST002.XPL", b"<Playlist>"),
        ("VPLST003.XPL", aacs(0x05, XML)),
        ("VPLST004.XPL", aacs(0x21, XML)),
        ("VPLST005.XPL", aacs(0x02, XML)),
        ("VPLST006.XPL", b"AAC"),
        # VPLST007 missing: VPLST008 is not read
        ("VPLST008.XPL", XML.encode()),
    ])
    assert load(im) == [0, 1, 4, 5]
    assert logged("/ADV_OBJ/VPLST001.XPL: AACS header type 0x12, 32 bytes of XML at 283")
    assert logged("/ADV_OBJ/VPLST002.XPL: playlist left out")
    assert logged("/ADV_OBJ/VPLST003.XPL: AACS header type 0x05 (0x02, 0x12, 0x21); playlist left out")
    assert logged("/ADV_OBJ/VPLST006.XPL: cannot read its first bytes; playlist left out")


def test_without_a_readable_playlist_the_disc_has_none():
    assert load(image([("VPLST000.XPL", b"<x>")])) == INVALIDDATA
    assert load(image([("VPLST001.XPL", XML.encode())])) == INVALIDDATA
    assert logged("No playlist (/ADV_OBJ/VPLST000.XPL ...) could be read")


# ---- real discs ----

@pytest.mark.disc
def test_corpus_every_playlist_equals_the_independent_reading():
    folder, refdir = env_or_skip("HDDVD_CORPUS", "HDDVD_XPL_REFERENCE")
    install_log(lib.AV_LOG_DEBUG)
    images = sorted(p for p in pathlib.Path(folder).iterdir() if p.suffix.lower() == ".iso")
    assert images
    for img in images:
        stem = re.sub(r"[^A-Za-z0-9_]", "_", img.stem)
        want = (pathlib.Path(refdir) / f"{stem}.VPLST000.txt").read_text()
        src = ffi.new("DiscIOSource **")
        fs = ffi.new("DiscIOFS **")
        xs = ffi.new("HDDVDXpl ***")
        nb = ffi.new("int *")
        assert lib.ff_discio_source_open_file(ffi.NULL, str(img).encode(), src) == 0
        assert lib.ff_discio_mount_image(src[0], ffi.NULL, fs) == 0
        assert lib.ff_hddvd_xpl_load(ffi.NULL, fs[0], xs, nb) == 0
        assert nb[0] == 1, img
        got = dump(xs[0][0])
        if got != want:
            first = next((i for i, (a, b) in enumerate(zip(got.splitlines(), want.splitlines())) if a != b), None)
            pytest.fail(f"{img}: differs at line {first}")
        print(f"{img}: VPLST000.XPL equal ({len(got.splitlines())} lines)")
        lib.ff_hddvd_xpl_free_all(xs, nb[0])
        lib.ff_discio_fs_close(fs)
        lib.ff_discio_source_free(src)
