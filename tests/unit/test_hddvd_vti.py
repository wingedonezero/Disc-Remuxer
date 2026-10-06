"""libavformat/hddvd_vti.c: the HD DVD Advanced VTS information file
(HVDVD_TS/HVA00001.VTI), built byte by byte; with HDDVD_CORPUS (a folder of
HD DVD images) every image's VTI is read through the disc readers and each
EVOB record's size is compared with its EVO file."""

import dataclasses
import pathlib
import struct

import pytest

from helpers import EINVAL, averror, ffi, install_log, lib, logged
from helpers.disc import Image, env_or_skip, free_file

INVALIDDATA = -0x41444E49


# ---- building a VTI ----

@dataclasses.dataclass
class Attr:
    audio: int
    subpic: int
    word0: int = 0x01020304


@dataclasses.dataclass
class Rec:
    name: bytes
    attr: int
    slot: int
    start: int = 1000
    end: int = 91000
    sectors: int = 77


def rec(name, attr_, slot):
    return Rec(name.encode(), attr_, slot)


def attr(audio, subpic):
    return Attr(audio, subpic)


def vti(attrs, attr_count, recs, rec_count):
    """Header in sector 0, attribute table in sector 1, EVOB table after it."""
    a = bytearray(8 + 4 * len(attrs))
    a[0:2] = struct.pack(">H", attr_count)
    for i, at in enumerate(attrs):
        a[8 + 4 * i:12 + 4 * i] = struct.pack(">I", len(a))
        r = bytearray(0x206)
        r[0] = i & 0xff  # marks the record
        r[0x0e:0x10] = struct.pack(">H", at.audio)
        r[0xe4:0xe6] = struct.pack(">H", at.subpic)
        r[0x186:0x18a] = struct.pack(">I", at.word0)
        a += r
    b = bytearray(8 + 4 * len(recs))
    b[0:4] = struct.pack(">I", rec_count)
    for i, e in enumerate(recs):
        b[8 + 4 * i:12 + 4 * i] = struct.pack(">I", len(b))
        r = bytearray(0x140)
        r[2:2 + len(e.name)] = e.name
        r[0x106:0x10a] = struct.pack(">I", e.attr)
        r[0x10a:0x10e] = struct.pack(">I", e.start)
        r[0x10e:0x112] = struct.pack(">I", e.end)
        r[0x112:0x116] = struct.pack(">I", e.sectors)
        r[0x116:0x118] = struct.pack(">H", e.slot)
        b += r
    a_sector = 1
    b_sector = a_sector + (len(a) + 2047) // 2048
    f = bytearray(2048)
    f[:12] = b"ADVANCED-VTS"
    f[0xb8:0xbc] = struct.pack(">I", a_sector)
    f[0xbc:0xc0] = struct.pack(">I", b_sector)
    a += bytes((len(a) + 2047) // 2048 * 2048 - len(a))
    return f + a + b


def good():
    return vti([attr(1, 0), attr(5, 4)], 2,
               [rec("FEATURE_2.EVO", 2, 3), rec("BLACK.EVO", 1, 1), rec("FEATURE_1.EVO", 2, 2)], 3)


# ---- parsing ----

class Parsed:
    def __init__(self, ptr):
        self.ptr = ptr

    def __del__(self):
        lib.ff_hddvd_vti_free(ffi.new("HDDVDVTI **", self.ptr))

    def vti(self):
        return self.ptr

    def evobs(self):
        """(slot, name, base, attribute, start, end, sectors) of every slot in use."""
        out = []
        for i in range(lib.HDDVD_VTI_MAX_EVOBS):
            e = self.ptr.evobs[i]
            if e != ffi.NULL:
                out.append((e.slot, ffi.string(e.name).decode("utf-8", "replace"),
                            ffi.string(e.base).decode("utf-8", "replace"), e.attr, e.start_ptm, e.end_ptm, e.sectors))
        return out


def parse(data):
    """A Parsed, or the negative error code."""
    install_log(lib.AV_LOG_DEBUG)
    data = bytes(data)

    def read(pos, length):
        if pos + length > len(data):
            return averror(EINVAL)  # as ff_discio_file_read past the end of a file
        return data[pos:pos + length]
    h = ffi.new_handle(read)
    out = ffi.new("HDDVDVTI **")
    ret = lib.ff_hddvd_vti_parse(ffi.NULL, lib.tb_py_hddvd_read, h, out)
    if ret < 0:
        assert out[0] == ffi.NULL
        return ret
    return Parsed(out[0])


def ok(data):
    p = parse(data)
    assert not isinstance(p, int), f"parse failed: {p}"
    return p


# ---- tests ----

def test_a_valid_file_gives_its_records_by_slot():
    p = ok(good())
    v = p.vti()
    assert (v.nb_attrs, v.nb_evobs) == (2, 3)
    a = v.attrs
    assert (a[0].nb_audio, a[0].nb_subpic, a[0].raw[0]) == (1, 0, 0)
    assert (a[1].nb_audio, a[1].nb_subpic, a[1].raw[0]) == (5, 4, 1)
    assert a[1].words[0] == 0x01020304
    assert p.evobs() == [
        (1, "BLACK.EVO", "BLACK", 1, 1000, 91000, 77),
        (2, "FEATURE_1.EVO", "FEATURE_1", 2, 1000, 91000, 77),
        (3, "FEATURE_2.EVO", "FEATURE_2", 2, 1000, 91000, 77),
    ]
    assert logged("VTI check EVOB record 3 at +0x")


def test_the_identifier_must_be_advanced_vts():
    f = good()
    f[:12] = b"STANDARD-VTS"
    assert parse(f) == INVALIDDATA
    assert logged('identifier "STANDARD-VTS" ("ADVANCED-VTS"): FAIL')


def test_the_attribute_table_holds_1_to_511_records():
    recs = [rec("A.EVO", 1, 1)]
    # no record at all is an error too, not only too many
    assert parse(vti([], 0, [], 0)) == INVALIDDATA
    assert logged("attribute record count 0 (1..511): FAIL")
    many = [attr(0, 0) for _ in range(511)]
    assert not isinstance(parse(vti(many, 511, recs, 1)), int)
    assert parse(vti(many, 512, recs, 1)) == INVALIDDATA
    assert logged("attribute record count 512 (1..511): FAIL")


def test_an_attribute_record_has_up_to_8_audio_and_32_sub_picture_streams():
    recs = [rec("A.EVO", 1, 1)]
    assert not isinstance(parse(vti([attr(8, 32)], 1, recs, 1)), int)
    assert parse(vti([attr(9, 0)], 1, recs, 1)) == INVALIDDATA
    assert logged("attribute record 1: audio stream count 9 (0..8): FAIL")
    assert parse(vti([attr(0, 33)], 1, recs, 1)) == INVALIDDATA
    assert logged("attribute record 1: sub-picture stream count 33 (0..32): FAIL")


def test_the_evob_table_holds_0_to_1998_records():
    assert ok(vti([attr(0, 0)], 1, [], 0)).vti().nb_evobs == 0
    every = [rec(f"E{i}.EVO", 1, i) for i in range(1, 1999)]
    assert ok(vti([attr(0, 0)], 1, every, 1998)).vti().nb_evobs == 1998
    assert parse(vti([attr(0, 0)], 1, every, 1999)) == INVALIDDATA
    assert logged("EVOB record count 1999 (0..1998): FAIL")


def test_an_evob_record_names_an_existing_attribute_record():
    at = [attr(0, 0), attr(0, 0)]
    assert parse(vti(at, 2, [rec("A.EVO", 0, 1)], 1)) == INVALIDDATA
    assert logged("EVOB record 1 (A.EVO): attribute record 0 (1..2): FAIL")
    assert parse(vti(at, 2, [rec("B.EVO", 3, 1)], 1)) == INVALIDDATA
    assert logged("EVOB record 1 (B.EVO): attribute record 3 (1..2): FAIL")


def test_an_evob_record_has_its_own_slot_from_1_to_1998():
    at = [attr(0, 0)]
    assert parse(vti(at, 1, [rec("A.EVO", 1, 0)], 1)) == INVALIDDATA
    assert logged("EVOB record 1 (A.EVO): slot 0 (1..1998): FAIL")
    assert parse(vti(at, 1, [rec("B.EVO", 1, 1999)], 1)) == INVALIDDATA
    assert logged("EVOB record 1 (B.EVO): slot 1999 (1..1998): FAIL")
    assert parse(vti(at, 1, [rec("C.EVO", 1, 7), rec("D.EVO", 1, 7)], 2)) == INVALIDDATA
    assert logged("EVOB record 2 (D.EVO): slot 7 already taken by C.EVO: FAIL")


def test_a_table_or_record_past_the_end_of_the_file_is_a_read_error():
    at = [attr(0, 0)]
    # the count says 2 records, the offset table and the records hold 1
    f = vti(at, 1, [rec("A.EVO", 1, 1)], 2)
    f = f[:len(f) - 0x140 + 0x100]  # and the one record is cut short
    assert parse(f) == averror(EINVAL)
    assert logged("VTI: cannot read an EVOB record (320 bytes at")
    f = good()[:0xbf]
    assert parse(f) == averror(EINVAL)
    assert logged("VTI: cannot read the header (192 bytes at 0)")


def test_the_name_is_at_most_255_bytes_and_the_base_name_drops_the_last_extension():
    long = bytearray(b"N" * 300)
    long[250:254] = b".EVO"
    r = rec("", 1, 1)
    r.name = bytes(long[:254])  # fills the 255-byte field but one byte: NUL there
    r2 = rec("A.B.EVO", 1, 2)
    r3 = rec("", 1, 3)
    r3.name = b"X" * 255  # no NUL in the field: cut at 255
    e = ok(vti([attr(0, 0)], 1, [r, r2, r3], 3)).evobs()
    assert len(e[0][1]) == 254
    assert len(e[0][2]) == 250
    assert (e[1][1], e[1][2]) == ("A.B.EVO", "A.B")
    assert (len(e[2][1]), len(e[2][2])) == (255, 255)


# ---- real discs ----

@pytest.mark.disc
def test_corpus_every_evob_record_matches_its_evo_file():
    folder = pathlib.Path(env_or_skip("HDDVD_CORPUS"))
    install_log(lib.AV_LOG_DEBUG)
    images = sorted(p for p in folder.iterdir() if p.suffix.lower() == ".iso")
    assert images, f"no .iso in {folder}"
    for img in images:
        v = Image.open(img).mount()
        assert not isinstance(v, int)
        out = ffi.new("HDDVDVTI **")
        assert lib.ff_hddvd_vti_open(ffi.NULL, v.fs, out) == 0, img
        p = Parsed(out[0])
        folder_name = ffi.string(p.vti().folder).decode()
        evobs = p.evobs()
        assert evobs
        for slot, name, _, _, start, end, sectors in evobs:
            ret, f = v.open_file(f"/{folder_name}/{name}")
            assert ret == 0, name
            assert f.size == sectors * 2048, f"{img} slot {slot} {name}"
            free_file(f)
            assert start <= end, name
        print(f"{img}: {len(evobs)} EVOB records match their files")
        del p
        v.close()
