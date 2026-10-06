"""HD DVD AACS (libavformat/hddvd_aacs.c on our libaacs): keys from the key
files, title keys, navigation packs, decrypted packs, on built discs. With
HDDVD_CORPUS (a folder of HD DVD images) and HDDVD_KEYDB (key files,
comma-separated): the encrypted 300 images are opened, the feature's first
4000 blocks decrypted (every E-AC-3 pack must hold a sync word), and the
same blocks decrypted again with a volume unique key computed from a device
key and the volume ID only."""

import hashlib
import pathlib

import pytest

from helpers.disc import env_or_skip
from helpers.hddvd import open_file, open_image
from synth.hddvd import (aes_g, data_pack, encrypt_pack, evob, hexs, image, nav_pack, playlist, tkf, title_keys,
                         tmap, vti)

INVALIDDATA = -0x41444E49
EACCES = -13

VUK = bytes([7, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0x42])
SEED = bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc])


def sha1(data):
    return hashlib.sha1(data).digest()


def scrambled(p):
    return p[17] != 0xbb and p[17] != 0xbf and p[20] & 0x30 != 0


class Blocks:
    """The EVOB's blocks, as plain packs and as the disc holds them: nav pack
    (title key id 5 = slot 4), scrambled audio, clear video, private stream 2
    with scrambling bits, scrambled video."""

    def __init__(self, key, mode_bits):
        self.plain = [
            nav_pack(mode_bits, 5, SEED),
            data_pack(0xbd, 1, 3),
            data_pack(0xe0, 0, 9),
            data_pack(0xbf, 1, 4),
            data_pack(0xe0, 2, 7),
        ]
        self.disc = [encrypt_pack(p, key, SEED) if scrambled(p) else p for p in self.plain]


def disc_with(blocks, aacs, playlists):
    xpl = playlist("", '<Title><PrimaryAudioVideoClip src="file:///dvddisc/HVDVD_TS/A.MAP"/></Title>')
    files = [("ADV_OBJ", "VPLST000.XPL", xpl)]
    if playlists > 1:
        files.append(("ADV_OBJ", "VPLST001.XPL", xpl))
    files.append(("HVDVD_TS", "HVA00001.VTI", vti([evob("A.EVO", 1, 10)])))
    files.append(("HVDVD_TS", "A.MAP", tmap([len(blocks)], 0, 0, False)))
    files.append(("HVDVD_TS", "A.EVO", b"".join(blocks)))
    files += aacs
    return image(files)


def cleared(p):
    c = bytearray(p)
    c[20] &= 0xcf
    return bytes(c)


def standard():
    keys = title_keys()
    b = Blocks(keys[4], 2)
    t = tkf(VUK, keys)
    return b, [("AACS", "VTKF000.AACS", t)], hexs(sha1(t))


@pytest.fixture
def key_file(tmp_path):
    def write(name, text):
        p = tmp_path / name
        p.write_text(text)
        return str(p)
    return write


def opened(im, key_files):
    d = open_image(im, key_files)
    assert not d.error, f"open failed: {d.error}"
    return d


def test_a_scrambled_pack_is_decrypted_with_the_title_key_its_navigation_pack_names(key_file):
    b, aacs, ident = standard()
    # the HD DVD key file dialect: no "0x"
    kf = key_file("bare.cfg", f"{ident} = Test disc | V | {hexs(VUK)}\n")
    d = opened(disc_with(b.disc, aacs, 1), [kf])
    for i, p in enumerate(b.plain):
        r, got = d.block(1, i)
        assert r == 1, f"block {i}"
        assert got == (cleared(p) if scrambled(p) else p), f"block {i}"


def test_the_vuk_comes_from_the_media_key_and_volume_id_too(key_file):
    keys = title_keys()
    mk = bytes([0x31] * 16)
    vid = bytes([0x40, 0, 0x15, 7, 3, 6, 0x19, 0x13]) + b"WGHDVM" + bytes(2)
    vuk = aes_g(mk, vid)
    b = Blocks(keys[4], 2)
    t = tkf(vuk, keys)
    kf = key_file("mk_vid.cfg", f"; comment\n| PK | 0x{hexs(bytes([9] * 16))}\n0x{hexs(sha1(t))} = Test | D | "
                                f"2007-01-01 | M | 0x{hexs(mk)} | I | 0x{hexs(vid)} ; note\n")
    d = opened(disc_with(b.disc, [("AACS", "VTKF000.AACS", t)], 1), [kf])
    assert d.block(1, 1)[1] == cleared(b.plain[1])


def test_the_first_key_file_with_a_vuk_wins(key_file):
    b, aacs, ident = standard()
    right = key_file("right.cfg", f"{ident} = Test | V | {hexs(VUK)}\n")
    wrong = key_file("wrong.cfg", f"{ident} = Test | V | {hexs(bytes([0xee] * 16))}\n")
    d = opened(disc_with(b.disc, aacs, 1), [right, wrong])
    assert d.block(1, 1)[1] == cleared(b.plain[1])
    d = opened(disc_with(b.disc, aacs, 1), [wrong, right])
    assert d.block(1, 1)[1] != cleared(b.plain[1]), "the wrong key decrypts to something else"


def test_without_a_vuk_or_a_volume_id_the_disc_cannot_be_opened(key_file):
    b, aacs, ident = standard()
    none = key_file("none.cfg",
                    "0123456789012345678901234567890123456789 = Other | V | 00112233445566778899AABBCCDDEEFF\n")
    assert open_image(disc_with(b.disc, aacs, 1), [none]).error == EACCES
    # a volume ID but no media key and no key that gives one (no MKBROM.AACS either)
    vid_only = key_file("vid_only.cfg", f"{ident} = Test | I | {hexs(bytes([1] * 16))}\n")
    assert open_image(disc_with(b.disc, aacs, 1), [vid_only]).error == EACCES
    assert open_image(disc_with(b.disc, aacs, 1), []).error == EACCES


def test_the_aacs_files_are_read_from_aacs_bak_when_aacs_lacks_them(key_file):
    b, aacs, ident = standard()
    aacs = [("AACS_BAK", aacs[0][1], aacs[0][2]), ("AACS", "DKF.AACS", bytes(64))]
    kf = key_file("bak.cfg", f"{ident} = Test | V | {hexs(VUK)}\n")
    d = opened(disc_with(b.disc, aacs, 1), [kf])
    assert d.block(1, 1)[1] == cleared(b.plain[1])


def test_a_bad_title_key_file_fails_the_open():
    b, aacs, _ = standard()
    t = bytearray(aacs[0][2])
    t[:12] = b"DVD_HD_V_TKX"
    assert open_image(disc_with(b.disc, [("AACS", "VTKF000.AACS", bytes(t))], 1), []).error == INVALIDDATA
    t[:12] = b"DVD_HD_V_TKF"
    t[12:16] = (0x9b1).to_bytes(4, "big")
    assert open_image(disc_with(b.disc, [("AACS", "VTKF000.AACS", bytes(t))], 1), []).error == INVALIDDATA
    # no VTKF000.AACS: no disc identifier
    assert open_image(disc_with(b.disc, [("AACS", "DKF.AACS", bytes(64))], 1), []).error == INVALIDDATA


def test_without_an_aacs_directory_scrambled_packs_cannot_be_used():
    b, _, _ = standard()
    d = opened(disc_with(b.disc, [], 1), [])
    assert d.block(1, 1)[0] == 0
    assert d.block(1, 2)[0] == 1


def test_the_navigation_pack_is_looked_for_back_from_the_pack(key_file):
    keys = title_keys()
    t = tkf(VUK, keys)
    kf = key_file("back.cfg", f"{hexs(sha1(t))} = Test | V | {hexs(VUK)}\n")
    # the extent's first block is no navigation pack; the one two blocks back is
    plain = [data_pack(0xe0, 0, 1), nav_pack(2, 5, SEED), data_pack(0xe0, 0, 2), data_pack(0xbd, 1, 5)]
    disc = list(plain)
    disc[3] = encrypt_pack(plain[3], keys[4], SEED)
    d = opened(disc_with(disc, [("AACS", "VTKF000.AACS", t)], 1), [kf])
    assert d.block(1, 3)[1] == cleared(plain[3])
    # no navigation pack at or before it: not usable
    plain = [data_pack(0xe0, 0, 1), data_pack(0xbd, 1, 5)]
    disc = [plain[0], encrypt_pack(plain[1], keys[4], SEED)]
    d = opened(disc_with(disc, [("AACS", "VTKF000.AACS", t)], 1), [kf])
    assert d.block(1, 1)[0] == 0


def test_only_key_mode_1_decrypts(key_file):
    keys = title_keys()
    t = tkf(VUK, keys)
    kf = key_file("mode.cfg", f"{hexs(sha1(t))} = Test | V | {hexs(VUK)}\n")
    # mode bits 0 -> mode 0, 1 -> mode 2, 3 -> invalid
    for bits in [0, 1, 3]:
        b = Blocks(keys[4], bits)
        d = opened(disc_with(b.disc, [("AACS", "VTKF000.AACS", t)], 1), [kf])
        assert d.block(1, 1)[0] == 0, f"mode bits {bits}"
        assert d.block(1, 2)[0] == 1, f"mode bits {bits}: a clear pack stays usable"


def test_title_key_ids_start_at_the_highest_playlist_naming_the_evob_times_256(key_file):
    # both playlists name A.EVO: its title keys come from VTKF001.AACS (ids 0x101..)
    keys0 = title_keys()
    keys1 = title_keys()
    keys1[4] = bytes([0x77] * 16)
    t0 = tkf(VUK, keys0)
    kf = key_file("pl.cfg", f"{hexs(sha1(t0))} = Test | V | {hexs(VUK)}\n")
    b = Blocks(keys1[4], 2)
    aacs = [("AACS", "VTKF000.AACS", t0), ("AACS", "VTKF001.AACS", tkf(VUK, keys1))]
    d = opened(disc_with(b.disc, aacs, 2), [kf])
    assert d.block(1, 1)[1] == cleared(b.plain[1])


# ---- real discs ----

def feature_blocks(d):
    """Decrypt 4000 blocks of the 300's feature (slot 7): (E-AC-3 packs, with
    a sync word, a hash of every block)."""
    eac3 = synced = 0
    every = bytearray()
    for blk in range(4000):
        r, s = d.block(7, blk)
        assert r == 1, f"block {blk}"
        h = 14 + ((s[0xd] & 7) if s[0xd] & 7 != 0 and s[0xe] != 0 else 0)
        if s[h + 3] == 0xbd:
            payload = s[h + 9 + s[h + 8]:]
            if 0xc0 <= payload[0] <= 0xcf:
                eac3 += 1
                synced += int(b"\x0b\x77" in payload)
        every += sha1(s)
    return eac3, synced, sha1(bytes(every))


@pytest.mark.disc
def test_corpus_the_encrypted_discs_decrypt_with_the_key_files(key_file):
    folder, keydb = env_or_skip("HDDVD_CORPUS", "HDDVD_KEYDB")
    kf = keydb.split(",")
    combo = pathlib.Path(folder) / "300 (HD-DVD Combo, HD-DVD Side).iso"
    d = open_file(combo, kf)
    assert not d.error
    eac3, synced, digest = feature_blocks(d)
    print(f"300 Combo: {eac3} E-AC-3 packs, {synced} with a sync word")
    assert eac3 > 100
    assert eac3 == synced
    d.close()

    # the same blocks with a volume unique key from a device key and the volume ID only
    full = pathlib.Path(next(f for f in kf if f.endswith("keydb.cfg"))).read_text()
    dks = "".join(line + "\n" for line in full.splitlines() if line.startswith("| DK |"))
    entry = next(line for line in full.splitlines() if line.startswith("0xA41555566FDA15BAC3C9DD9E52178D388787B67C"))
    vid = entry.split("| I | ")[1].split()[0]
    only = key_file("dk_vid.cfg", f"{dks}0xA41555566FDA15BAC3C9DD9E52178D388787B67C = 300 | I | {vid}\n")
    d = open_file(combo, [only])
    assert not d.error
    assert feature_blocks(d)[2] == digest, "same blocks with a computed VUK"
    d.close()

    # the Standalone disc: no key for it in the key files
    assert open_file(pathlib.Path(folder) / "300 (Standalone HD-DVD).iso", kf).error == EACCES
