"""Blu-ray AACS (libavformat/bluray_aacs.c on our libaacs): disc folders
built here with their AACS files and encrypted stream files (tests/synth/
bluray.py), opened with the AACS step: the unit keys from the key files
(volume unique key, media key + volume ID, unit-key list; files in order),
the AACS files' rules (/AACS_bd, DUPLICATE, record size from the MKB type,
SKB, discs not encrypted after all), which clip uses which CPS unit (at the
open for the largest clips, else on the clip's first encrypted unit), the
aligned units, and the checks on discs without AACS directories."""

import hashlib

import pytest

from helpers import LOG, ffi, install_log, lib, logged
from synth import bluray as bd
from synth.hddvd import aes_e, aes_g, hexs

EACCES = -13
INVALIDDATA = -0x41444E49
PATCHWELCOME = -0x45574150

KEY_UNKNOWN, KEY_NONE = -1, 0

VUK = bytes([0x21, 3, 5, 7, 9, 11, 13, 15, 17, 19, 21, 23, 25, 27, 29, 0x42])
UK = [bytes([(n * 37 + i * 11 + 1) & 0xff for i in range(16)]) for n in range(1, 5)]


def sha1(data):
    return hashlib.sha1(data).digest()


class Disc:
    """A built disc opened with the AACS step (error: the open's error)."""

    def __init__(self, path, key_files=()):
        install_log()
        err = ffi.new("int *")
        keep = [ffi.new("char[]", str(k).encode()) for k in key_files]
        ptrs = ffi.new("const char *[]", keep) if keep else ffi.NULL
        self.b = lib.tb_bluray_open(str(path).encode(), ptrs, len(keep), err)
        self.error = err[0] if self.b == ffi.NULL else 0

    def close(self):
        if self.b != ffi.NULL:
            lib.tb_bluray_close(self.b)
            self.b = ffi.NULL

    def key_unit(self, clip):
        return lib.tb_bluray_key_unit(self.b, clip)

    def unit(self, clip, data):
        buf = ffi.new("uint8_t[]", bytes(data))
        r = lib.tb_bluray_unit(self.b, clip, buf)
        return r, bytes(ffi.buffer(buf, len(data)))

    def disc_id(self):
        out = ffi.new("uint8_t[20]")
        return bytes(out) if lib.tb_bluray_disc_id(self.b, out) else None

    def has_keys(self):
        return bool(lib.tb_bluray_has_keys(self.b))


@pytest.fixture
def opened():
    discs = []

    def op(path, key_files=()):
        d = Disc(path, key_files)
        discs.append(d)
        return d
    yield op
    for d in discs:
        d.close()


@pytest.fixture
def key_file(tmp_path):
    def write(name, text):
        p = tmp_path / name
        p.write_text(text)
        return str(p)
    return write


def build(tmp_path, clips, extra=None, name="disc"):
    """clips: {number: [aligned units of its stream file]}."""
    files = bd.bdmv({n: bd.clpi(packets=len(u) * 32) for n, u in clips.items()}, extra=extra)
    for n, u in clips.items():
        files[f"BDMV/STREAM/{n:05d}.m2ts"] = b"".join(u)
    return bd.write(tmp_path / name, files)


def aacs_files(keys, vuk=VUK, record=48, mkb_type=0x031003, skb=False, where="AACS"):
    uk = bd.unit_key_ro([aes_e(vuk, k) for k in keys], record=record, skb=skb)
    return {f"{where}/Unit_Key_RO.inf": uk, f"{where}/MKB_RO.inf": bd.mkb(mkb_type)}, hexs(sha1(uk))


def one_unit_disc(tmp_path, key_file, plain, extra_aacs=None):
    files, ident = aacs_files(UK[:1])
    files.update(extra_aacs or {})
    path = build(tmp_path, {1: [bd.encrypt_unit(p, UK[0]) for p in plain]}, files)
    return path, key_file("keydb.cfg", f"0x{ident} = Test disc | V | 0x{hexs(VUK)}\n"), ident


# ---- keys ----

def test_an_encrypted_unit_is_decrypted_with_the_vuk_from_the_key_file(tmp_path, key_file, opened):
    plain = [bd.ts_unit(1), bd.ts_unit(2)]
    path, kf, ident = one_unit_disc(tmp_path, key_file, plain)
    d = opened(path, [kf])
    assert not d.error
    assert d.disc_id().hex().upper() == ident, "the disc ID is the SHA-1 of Unit_Key_RO.inf"
    assert d.key_unit(1) == 1, "one CPS unit: every clip uses it"
    disc = [bd.encrypt_unit(p, UK[0]) for p in plain]
    for p, u in zip(plain, disc):
        assert d.unit(1, u) == (0, p), "decrypted, every copy permission mark cleared"
    assert logged("volume unique key from")


def test_units_not_marked_or_not_ts_are_left_as_they_are(tmp_path, key_file, opened):
    path, kf, _ = one_unit_disc(tmp_path, key_file, [bd.ts_unit(1)])
    d = opened(path, [kf])
    plain = bd.ts_unit(5)
    assert d.unit(1, plain) == (0, plain)
    other = bytearray(bd.encrypt_unit(bd.ts_unit(6), UK[0]))
    other[4] = 0x46                                     # not a TS sync byte
    assert d.unit(1, bytes(other)) == (0, bytes(other))


def test_the_vuk_comes_from_the_media_key_and_volume_id(tmp_path, key_file, opened):
    mk = bytes([0x31] * 16)
    vid = bytes(range(0x40, 0x50))
    vuk = aes_g(mk, vid)
    files, ident = aacs_files(UK[:1], vuk=vuk)
    plain = bd.ts_unit(3)
    path = build(tmp_path, {1: [bd.encrypt_unit(plain, UK[0])]}, files)
    kf = key_file("mk.cfg", f"{ident} = Test | M | 0x{hexs(mk)} | I | 0x{hexs(vid)}\n")
    d = opened(path, [kf])
    assert not d.error
    assert d.unit(1, bd.encrypt_unit(plain, UK[0])) == (0, plain)
    assert logged("from the media key and the volume ID")


def test_the_unit_keys_of_the_key_file_are_used_without_a_vuk(tmp_path, key_file, opened):
    files, ident = aacs_files(UK[:2], vuk=bytes([0xee] * 16))
    plain = bd.ts_unit(4)
    path = build(tmp_path, {1: [bd.encrypt_unit(plain, UK[1])]}, files)
    kf = key_file("uk.cfg", f"0x{ident} = Test | D | 2020-01-01 | U | 1-0x{hexs(UK[0])} | 2-0x{hexs(UK[1])}\n")
    d = opened(path, [kf])
    assert not d.error
    assert d.key_unit(1) == 2, "the largest clip's key is found at the open"
    assert d.unit(1, bd.encrypt_unit(plain, UK[1])) == (0, plain)
    # a list without every unit is not enough
    kf = key_file("uk1.cfg", f"0x{ident} = Test | U | 1-0x{hexs(UK[0])}\n")
    assert opened(path, [kf]).error == EACCES
    assert logged("lack unit 2 of 2")


def test_the_first_key_file_with_a_vuk_wins(tmp_path, key_file, opened):
    plain = [bd.ts_unit(1)]
    path, right, ident = one_unit_disc(tmp_path, key_file, plain)
    wrong = key_file("wrong.cfg", f"0x{ident} = Test | V | 0x{hexs(bytes([0xee] * 16))}\n")
    u = bd.encrypt_unit(plain[0], UK[0])
    assert opened(path, [right, wrong]).unit(1, u) == (0, plain[0])
    assert opened(path, [wrong, right]).unit(1, u)[1] != plain[0], "the wrong key decrypts to something else"


def test_without_a_key_the_disc_cannot_be_opened(tmp_path, key_file, opened):
    path, _, _ = one_unit_disc(tmp_path, key_file, [bd.ts_unit(1)])
    other = key_file("other.cfg", "0x0123456789012345678901234567890123456789 = Other | V | 0x" + "11" * 16 + "\n")
    assert opened(path, [other]).error == EACCES
    assert logged("the volume key is unknown for this disc")
    assert opened(path, []).error == EACCES
    assert logged("no key file is set")


# ---- the AACS files ----

def test_aacs_files_come_from_aacs_bd_and_duplicate_copies(tmp_path, key_file, opened):
    plain = bd.ts_unit(2)
    files, ident = aacs_files(UK[:1], where="AACS_bd")
    kf = key_file("k.cfg", f"0x{ident} = T | V | 0x{hexs(VUK)}\n")
    d = opened(build(tmp_path, {1: [bd.encrypt_unit(plain, UK[0])]}, files, "bd"), [kf])
    assert not d.error and d.unit(1, bd.encrypt_unit(plain, UK[0])) == (0, plain)
    files, _ = aacs_files(UK[:1])
    dup = {"AACS/DUPLICATE/Unit_Key_RO.inf": files["AACS/Unit_Key_RO.inf"], "AACS/MKB_RO.inf": files["AACS/MKB_RO.inf"]}
    d = opened(build(tmp_path, {1: [bd.encrypt_unit(plain, UK[0])]}, dup, "dup"), [kf])
    assert not d.error and d.unit(1, bd.encrypt_unit(plain, UK[0])) == (0, plain)
    assert logged("Unit_Key_RO.inf read from /AACS/DUPLICATE/Unit_Key_RO.inf")


def test_the_mkb_type_gives_the_unit_key_record_size(tmp_path, key_file, opened):
    plain = bd.ts_unit(7)
    for mkb_type, record in ((0x141003, 64), (0x151003, 64), (0x131003, 48), (0x031003, 48)):
        files, ident = aacs_files(UK[:2], record=record, mkb_type=mkb_type)
        kf = key_file(f"r{mkb_type:x}.cfg", f"0x{ident} = T | V | 0x{hexs(VUK)}\n")
        path = build(tmp_path, {1: [bd.encrypt_unit(plain, UK[1])]}, files, f"m{mkb_type:x}")
        d = opened(path, [kf])
        assert not d.error, hex(mkb_type)
        assert d.key_unit(1) == 2, hex(mkb_type)


def test_zero_keys_or_an_empty_mkb_mean_not_encrypted(tmp_path, key_file, opened):
    marked = bd.encrypt_unit(bd.ts_unit(1), UK[0])
    uk = bd.unit_key_ro([bytes(16)])
    d = opened(build(tmp_path, {1: [marked]}, {"AACS/Unit_Key_RO.inf": uk, "AACS/MKB_RO.inf": bd.mkb()}, "z"))
    assert not d.error and not d.has_keys()
    assert d.key_unit(1) == KEY_NONE
    assert d.unit(1, marked) == (0, marked), "a marked unit of a disc without keys is used as it is"
    assert d.disc_id() is None
    files, _ = aacs_files(UK[:1])
    files["AACS/MKB_RO.inf"] = bd.mkb(size=2047)
    d = opened(build(tmp_path, {1: [marked]}, files, "short"))
    assert not d.error and d.key_unit(1) == KEY_NONE
    files["AACS/MKB_RO.inf"] = bytes(4096)
    d = opened(build(tmp_path, {1: [marked]}, files, "zero"))
    assert not d.error and d.key_unit(1) == KEY_NONE


def test_unusable_unit_key_files(tmp_path, opened):
    files, _ = aacs_files(UK[:1], skb=True)
    assert opened(build(tmp_path, {1: [bd.ts_unit()]}, files, "skb")).error == PATCHWELCOME
    assert logged("SKB")
    for name, uk in (("type", bd.unit_key_ro([UK[0]], kind=(3, 1))),
                     ("version", bd.unit_key_ro([UK[0]], kind=(1, 2))),
                     ("none", bd.unit_key_ro([])),
                     ("short", bd.unit_key_ro([UK[0]])[:-4])):
        f = {"AACS/Unit_Key_RO.inf": uk, "AACS/MKB_RO.inf": bd.mkb()}
        assert opened(build(tmp_path, {1: [bd.ts_unit()]}, f, name)).error == INVALIDDATA, name
    f = {"AACS/MKB_RO.inf": bd.mkb()}
    assert opened(build(tmp_path, {1: [bd.ts_unit()]}, f, "nouk")).error == INVALIDDATA
    f = {"AACS/Unit_Key_RO.inf": bd.unit_key_ro([UK[0]])}
    assert opened(build(tmp_path, {1: [bd.ts_unit()]}, f, "nomkb")).error == INVALIDDATA


def test_recordable_aacs_is_not_supported_yet(tmp_path, opened):
    f = {"AACS2_av/MKB_RW.inf": bd.mkb()}
    assert opened(build(tmp_path, {1: [bd.ts_unit()]}, f, "aacs2")).error == PATCHWELCOME
    f = {"AACS/AACS_av/Unit_Key_RW.inf": bytes(64), "AACS/MKB_RW.inf": bd.mkb()}
    assert opened(build(tmp_path, {1: [bd.ts_unit()]}, f, "rw")).error == PATCHWELCOME
    f = {"AACS/Unit_Key_RO.inf": bd.unit_key_ro([UK[0]], kind=(2, 1)), "AACS/MKB_RO.inf": bd.mkb()}
    assert opened(build(tmp_path, {1: [bd.ts_unit()]}, f, "type2")).error == PATCHWELCOME


# ---- which clip uses which CPS unit ----

def test_the_three_largest_clips_get_their_unit_at_the_open_the_others_on_their_first_encrypted_unit(
        tmp_path, key_file, opened):
    # clip: (CPS unit, units of its stream file); sizes decide the order
    layout = {1: (2, 6), 2: (3, 5), 3: (1, 4), 4: (2, 3), 5: (3, 2)}
    clips = {}
    for n, (cps, count) in layout.items():
        units = [bd.ts_unit(n * 10 + i) for i in range(count)]
        # the first unit is not marked: the search goes on to the next
        clips[n] = [units[0]] + [bd.encrypt_unit(u, UK[cps - 1]) for u in units[1:]]
    files, ident = aacs_files(UK[:3])
    kf = key_file("k.cfg", f"0x{ident} = T | V | 0x{hexs(VUK)}\n")
    d = opened(build(tmp_path, clips, files), [kf])
    assert not d.error
    assert [d.key_unit(n) for n in (1, 2, 3, 4, 5)] == [2, 3, 1, KEY_UNKNOWN, KEY_UNKNOWN]
    plain = bd.ts_unit(99)
    assert d.unit(4, bd.encrypt_unit(plain, UK[1])) == (0, plain)
    assert d.key_unit(4) == 2
    assert logged("clip 00004 uses CPS unit 2")
    # a first marked unit with copy permission 1 gives no key
    r, _ = d.unit(5, bd.encrypt_unit(plain, UK[2], cpi=1))
    assert r == INVALIDDATA and d.key_unit(5) == KEY_UNKNOWN
    # no key fits
    r, _ = d.unit(5, bd.encrypt_unit(plain, UK[3]))
    assert r == EACCES
    assert logged("no unit key decrypts its first encrypted unit")


# ---- discs without AACS directories ----

def plain_disc(tmp_path, name, extra=None, playlists=None):
    files = bd.bdmv({1: bd.clpi(packets=32)}, playlists, extra=extra)
    files["BDMV/STREAM/00001.m2ts"] = bd.ts_unit()
    return bd.write(tmp_path / name, files)


def test_no_aacs_directory_means_not_encrypted(tmp_path, opened):
    d = opened(plain_disc(tmp_path, "plain"))
    assert not d.error and not d.has_keys()
    assert d.key_unit(1) == KEY_NONE
    assert d.disc_id() is None
    assert logged("no AACS directory: the disc is not encrypted")
    assert not any("WARNING" in l or "decrypted by" in l for l in LOG)


def test_signs_of_other_decryption_software_are_warned_about(tmp_path, opened):
    uk = bd.unit_key_ro([UK[0]])
    for name, extra, software in (
            ("any", {"ANY!/Unit_Key_RO.inf": uk}, "AnyDVD (/ANY! directory)"),
            ("sly", {"SLY!/x": b"1"}, "AnyDVD (/SLY! directory)"),
            ("otf", {"KEYS/MKB_RO.inf": b"1"}, "on-the-fly decryption software (AACS key files in /KEYS)")):
        d = opened(plain_disc(tmp_path, name, extra))
        assert not d.error and d.key_unit(1) == KEY_NONE, name
        assert logged(f"the disc looks decrypted by {software}"), name
    assert d.disc_id() is None
    d = opened(plain_disc(tmp_path, "any2", {"ANY!/Unit_Key_RO.inf": uk}))
    assert d.disc_id() == sha1(uk), "the disc ID of the Unit_Key file the software left"


def test_a_longer_backup_playlist_shows_other_software(tmp_path, opened):
    pl = bd.mpls([bd.Item()])
    for name, tail, software in (("zero", bytes(4), "AnyDVD"), ("data", b"\x01\x02", "on-the-fly decryption software")):
        extra = {"BDMV/BACKUP/PLAYLIST/00001.mpls": pl + tail}
        d = opened(plain_disc(tmp_path, name, extra, {1: pl}))
        assert not d.error
        assert logged(f"the disc looks decrypted by {software} (/BDMV/BACKUP/PLAYLIST/00001.mpls is {len(tail)} "
                      f"bytes longer than /BDMV/PLAYLIST/00001.mpls)"), name
    extra = {"BDMV/BACKUP/PLAYLIST/00001.mpls": pl + bytes(17)}
    opened(plain_disc(tmp_path, "long", extra, {1: pl}))
    assert not logged("decrypted by"), "more than 16 bytes longer: no sign"


def test_avchd_maker_data_is_accepted_silently(tmp_path, opened):
    pl = bd.mpls([bd.Item()], ext=[(0x1000, 0x100, b"PLEX" + bytes(12))])
    d = opened(plain_disc(tmp_path, "avchd", {"SLY!/x": b"1"}, {1: pl}))
    assert not d.error and d.key_unit(1) == KEY_NONE
    assert logged("AVCHD maker data found")
    assert not logged("decrypted by")

