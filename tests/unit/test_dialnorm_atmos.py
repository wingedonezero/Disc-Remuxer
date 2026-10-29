"""Dolby Atmos found in a finished stream file, for mkv.remove_dialog_normalization
(writer.c writer_atmos_units; parse/ac3_rule.c dr_eac3_atmos, parse/mlp_rule.c
dr_truehd_atmos): an E-AC-3 frame carries Atmos when its additional bitstream
information starts with flag_ec3_extension_type_a (ETSI TS 102 366 Annex E,
as FFmpeg's parser reads it); a TrueHD access unit when it starts with a major
sync with 4 substreams and the top bit of substream_info (FFmpeg's TrueHD +
Atmos). The frames here are built bit by bit: the headers, the rest zeros."""

from helpers import ffi, lib

EAC3, TRUEHD = 86056, 86060      # src/timeline/disctl.h


class Bits:
    def __init__(self, size):
        self.b = bytearray(size)
        self.p = 0

    def put(self, n, v):
        for i in range(n - 1, -1, -1):
            if (v >> i) & 1:
                self.b[self.p >> 3] |= 0x80 >> (self.p & 7)
            self.p += 1


def eac3(strmtyp=0, acmod=7, fscod=0, metadata=False, addbsi=False, atmos=False, size=256):
    """An E-AC-3 frame (bsid 16; 48 kHz, 6 blocks unless fscod is 3)."""
    w = Bits(size)
    lfe = 1
    dialogs = 1 if acmod else 2
    for n, v in ((16, 0x0B77), (2, strmtyp), (3, 0), (11, size // 2 - 1), (2, fscod), (2, 0 if fscod == 3 else 3),
                 (3, acmod), (1, lfe), (5, 16)):
        w.put(n, v)
    for _ in range(dialogs):
        w.put(5, 27)                 # dialnorm
        w.put(1, 0)                  # compre
    if strmtyp == 1:
        w.put(1, 0)                  # chanmape
    w.put(1, metadata)               # mixmdate
    if metadata:
        if acmod > 2:
            w.put(2, 2)              # dmixmod
        if acmod & 1:
            w.put(6, 0)
        if acmod & 4:
            w.put(6, 0)
        w.put(1, 1)                  # lfemixlevcode
        w.put(5, 10)
        if strmtyp == 0:
            for _ in range(dialogs):
                w.put(1, 1)          # pgmscle
                w.put(6, 5)
            w.put(1, 0)              # extpgmscle
            w.put(2, 3)              # mixdef
            w.put(5, 1)              # mixdeflen: 3 bytes
            w.put(24, 0xABCDEF)
            if acmod < 2:
                for _ in range(dialogs):
                    w.put(1, 1)      # paninfoe
                    w.put(14, 0)
            w.put(1, 1)              # frmmixcfginfoe
            for _ in range(6):
                w.put(1, 1)          # blkmixcfginfoe
                w.put(5, 0)
    w.put(1, metadata)               # infomdate
    if metadata:
        w.put(3, 0)                  # bsmod
        w.put(2, 0)
        if acmod == 2:
            w.put(4, 0)              # dsurmod, dheadphonmod
        if acmod >= 6:
            w.put(2, 0)              # dsurexmod
        for _ in range(dialogs):
            w.put(1, 1)              # audprodie
            w.put(8, 0)
        if fscod != 3:
            w.put(1, 0)              # sourcefscod
    w.put(1, addbsi)                 # addbsie
    if addbsi:
        w.put(6, 1)                  # addbsil: 2 bytes
        w.put(7, 0)
        w.put(1, atmos)              # flag_ec3_extension_type_a
        w.put(8, 16)                 # complexity_index_type_a
    return bytes(w.b)


def ac3():
    """An AC-3 frame: 48 kHz, frame size code 0 (64 words), bsid 8."""
    w = Bits(128)
    for n, v in ((16, 0x0B77), (16, 0), (2, 0), (6, 0), (5, 8), (3, 0), (3, 7)):
        w.put(n, v)
    return bytes(w.b)


def truehd(major=False, substreams=3, top_bit=False, sync=0xBA, words=40):
    """A TrueHD access unit: its length (words), a major sync when asked."""
    au = bytearray(words * 2)
    au[0] = (words >> 8) & 0xF
    au[1] = words & 0xFF
    if major:
        au[4:8] = bytes((0xF8, 0x72, 0x6F, sync))
        au[20] = substreams << 4
        au[21] = 0x80 if top_bit else 0x00
    return bytes(au)


def eac3_atmos(frame):
    return lib.dr_eac3_atmos(frame, len(frame))


def truehd_atmos(au):
    return lib.dr_truehd_atmos(au, len(au))


def units_in(path, codec):
    units, atmos = ffi.new("int64_t *"), ffi.new("int64_t *")
    ret = lib.writer_atmos_units(str(path).encode(), codec, units, atmos)
    return ret, units[0], atmos[0]


def test_an_eac3_frame_with_the_flag_is_atmos_whatever_comes_before_it():
    for acmod in (0, 1, 2, 7):
        for metadata in (False, True):
            assert eac3_atmos(eac3(acmod=acmod, metadata=metadata, addbsi=True, atmos=True)) == 1, (acmod, metadata)
    assert eac3_atmos(eac3(fscod=3, metadata=True, addbsi=True, atmos=True)) == 1, "reduced sample rate"
    assert eac3_atmos(eac3(strmtyp=1, metadata=True, addbsi=True, atmos=True)) == 1, "a dependent frame"


def test_an_eac3_frame_without_the_flag_or_without_additional_info_is_not_atmos():
    assert eac3_atmos(eac3(metadata=True, addbsi=True, atmos=False)) == 0
    assert eac3_atmos(eac3(metadata=True)) == 0
    assert eac3_atmos(eac3()) == 0
    assert eac3_atmos(ac3()) == 0, "AC-3"


def test_a_frame_that_cannot_be_read_says_so():
    frame = eac3(metadata=True, addbsi=True, atmos=True)
    assert eac3_atmos(frame[:12]) < 0, "cut off before its additional information"
    assert eac3_atmos(b"\x00" * 64) < 0, "no sync word"


def test_a_truehd_major_sync_with_4_substreams_and_the_top_bit_is_atmos():
    assert truehd_atmos(truehd(major=True, substreams=4, top_bit=True)) == 1
    assert truehd_atmos(truehd(major=True, substreams=4, top_bit=False)) == 0
    assert truehd_atmos(truehd(major=True, substreams=3, top_bit=True)) == 0
    assert truehd_atmos(truehd(major=True, substreams=4, top_bit=True, sync=0xBB)) == 0, "MLP"
    assert truehd_atmos(truehd()) == 0, "no major sync"


def test_a_truehd_file_is_read_to_its_end(tmp_path):
    path = tmp_path / "a.thd"
    plain = truehd(major=True, substreams=3) + truehd() * 9
    atmos = truehd(major=True, substreams=4, top_bit=True) + truehd() * 9
    path.write_bytes(plain * 2000 + atmos + plain * 2000)        # 1.6 MB: read in more than one piece
    assert units_in(path, TRUEHD) == (0, 40010, 1), "the one Atmos major sync in the middle is found"
    path.write_bytes(plain * 100)
    assert units_in(path, TRUEHD) == (0, 1000, 0)


def test_an_eac3_file_is_read_to_its_end_with_ac3_frames_among_them(tmp_path):
    path = tmp_path / "a.eac3"
    plain = ac3() + eac3(strmtyp=1, metadata=True)
    path.write_bytes(plain * 3000 + eac3(metadata=True, addbsi=True, atmos=True) + plain * 3000)
    assert units_in(path, EAC3) == (0, 12001, 1)
    path.write_bytes(plain * 10)
    assert units_in(path, EAC3) == (0, 20, 0)


def test_a_file_that_cannot_be_read_through_is_an_error(tmp_path):
    path = tmp_path / "a.thd"
    path.write_bytes(truehd(major=True) + truehd()[:30])
    assert units_in(path, TRUEHD)[0] < 0, "it does not end on a whole unit"
    path.write_bytes(eac3() + b"\x00" * 64)
    assert units_in(path, EAC3)[0] < 0, "no frame where one must start"
    assert units_in(tmp_path / "missing.thd", TRUEHD)[0] < 0
    assert units_in(path, 86020)[0] < 0, "DTS is not read"
