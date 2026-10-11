"""libavformat/dvd_read.c on libdvdread's title VOB reading (vob_read.h): a
title set's title VOB sectors read through the checks: a sector that is no
MPEG-2 pack with a packet is left out; a scrambled packet of the video, private stream 1 or MPEG audio
is descrambled with the title set's CSS title key, found by libdvdcss (no
drive: cracked from the sectors) when the first scrambled sector is read and
checked on an AC-3 frame's CRC or an MPEG-2 sequence header's extension; a
title set whose title VOBs lie inside another's takes that one's key. Built
discs: sectors are scrambled with a key by the descrambler itself (it is
tab1[x] ^ keystream, see css_scramble). With DVD_CSS_DISC (a scrambled DVD
image or folder) every title set's key must be found and checked."""

import pathlib

from helpers import ffi, install_log, lib, logged
from helpers.disc import env_or_skip
from synth import a52, dvd
from synth.dvd import S, be16, be32
from test_dvd_disc import Disc, disc_files, image, vmg, vts
from test_dvd_nav import open_disc

KEY = bytes([0x1f, 0x52, 0x9a, 0x03, 0xc4])
CLEAR = 0x80


def descramble(key, sec):
    b = ffi.new("uint8_t[]", bytes(sec))
    lib.dvdcss_descramble(key, b)
    return bytes(ffi.buffer(b, S))


def css_scramble(key, sec):
    """sec scrambled with key: the descrambler maps byte x at i >= 0x80 to
    tab1[x] ^ k[i] (k from the key and the sector's bytes 0x54-0x58), so
    with a = descramble(zeros) = tab1[0] ^ k, T[v] = tab1[v] ^ tab1[0] is
    the same at every i, and the byte that descrambles to p is T^-1[p ^ a]."""
    zeros = bytearray(sec)
    zeros[CLEAR:] = bytes(S - CLEAR)
    a = descramble(key, zeros)
    inv = {}
    for v in range(256):
        b = bytearray(sec)
        b[CLEAR:] = bytes([v]) * (S - CLEAR)
        inv[descramble(key, b)[CLEAR] ^ a[CLEAR]] = v
    out = bytearray(sec)
    for i in range(CLEAR, S):
        out[i] = inv[sec[i] ^ a[i]]
    return bytes(out)


def pack(stream_id, payload, scrambled):
    """One sector: an MPEG-2 pack header, one PES packet of stream_id with a
    PTS (scrambling bits 01 when scrambled) and payload filling it."""
    s = bytearray(S)
    s[:14] = bytes([0, 0, 1, 0xba, 0x44, 0, 4, 0, 4, 1, 1, 0x89, 0xc3, 0xf8])
    s[14:18] = bytes([0, 0, 1, stream_id])
    be16(s, 18, S - 20)
    s[20] = 0x81 | (0x10 if scrambled else 0)
    s[21] = 0x80
    s[22] = 5
    s[23:28] = bytes([0x21, 0, 1, 0, 1])
    s[28:28 + len(payload)] = payload
    return s


def ac3_pack(good_crc=True):
    """Private stream 1 AC-3 (sub-stream 0x80, one frame, the first access
    unit right after the header): a 512-byte frame of zeros (its CRCs set,
    or one data byte past the clear part changed) from byte 0x20."""
    frame = bytearray(a52.with_crcs(dvd_ac3_frame()))
    if not good_crc:
        frame[0x100] ^= 0x40
    return pack(0xbd, bytes([0x80, 1, 0, 1]) + bytes(frame), True)


def dvd_ac3_frame():
    f = bytearray(512)
    f[0:2] = b"\x0b\x77"
    f[4] = 16                   # 48 kHz, 128 kb/s: 512 bytes
    f[5] = 8 << 3               # bsid 8
    return bytes(f)


def video_pack():
    """MPEG-2 video: a sequence header loading both quantiser matrices (all
    16), then its sequence_extension, from the payload's start."""
    sh = bytearray([0, 0, 1, 0xb3, 0x2d, 0x01, 0xe0, 0x33, 0xff, 0xff, 0xe0, 0x02 | 0])
    sh += bytes([16]) * 63 + bytes([16 | 1])   # intra matrix; the last byte's low bit: load_non_intra
    sh += bytes([16]) * 64
    sh += bytes([0, 0, 1, 0xb5, 0x14, 0x8a, 0, 1, 0, 0])
    return pack(0xe0, bytes(sh), True)


def folder_with(tmp_path, sectors, v=None):
    """A disc folder whose title set's title VOBs hold sectors from sector 1
    (sector 0: a NAV pack, the rest zeros)."""
    v = v or vts(vob_sectors=1 + len(sectors) + 4)
    path = tmp_path / "disc"
    vob = bytearray(dvd.title_vobs(v.vob_sectors, [dvd.nav(0, v.vob_sectors, None)]))
    for k, sec in enumerate(sectors):
        vob[(1 + k) * S:(2 + k) * S] = sec
    dvd.write_disc(path, vmg(), [(dvd.vts_ifo(v), bytes(vob))])
    return path


def css(d, vts_=1):
    """Title set vts_'s CSS title key as libdvdread keeps it: (state, key);
    state 1 found, 0 not looked for, -1 none."""
    key = ffi.new("uint8_t[]", 5)
    state = lib.ff_dvd_css_state(d.d, d.ifo(vts_), key)
    return state, bytes(ffi.buffer(key, 5)) if state == 1 else None


def read(d, sector, vts_=1):
    buf = ffi.new("uint8_t[]", S)
    ret = lib.ff_dvd_title_vob_sector(d.d, d.ifo(vts_), sector, buf)
    return ret, bytes(ffi.buffer(buf, S))


def test_built_sectors_descramble_back_to_their_plain_text():
    plain = bytes(ac3_pack())
    scr = css_scramble(KEY, plain)
    assert scr[:CLEAR] == plain[:CLEAR] and scr[CLEAR:] != plain[CLEAR:]
    assert descramble(KEY, scr) == plain


def test_a_scrambled_title_set_gets_its_key_cracked_and_checked_on_an_ac3_frame(tmp_path):
    plain = [ac3_pack(), ac3_pack()]
    d = Disc.folder(folder_with(tmp_path, [css_scramble(KEY, p) for p in plain]))
    install_log(lib.AV_LOG_DEBUG)
    ret, got = read(d, 1)
    assert ret == 1
    want = bytearray(plain[0])
    want[20] &= 0xcf                            # the scrambling bits cleared
    assert got == bytes(want)
    assert css(d) == (1, KEY)
    assert logged("VTS_01: sector 1 is scrambled: looking for the CSS title key")
    assert logged("VTS_01: the CSS title key is right on sector 1 (an AC-3 frame's CRC)")
    assert logged("VTS_01: CSS title key found")
    # the next one with the key found
    install_log(lib.AV_LOG_DEBUG)
    assert read(d, 2)[0] == 1 and not logged("looking for")
    d.close()


def test_the_key_is_checked_on_an_mpeg2_sequence_header(tmp_path):
    plain = video_pack()
    v = vts(vob_sectors=6, video_attr=bytes([0x40, 0]))     # MPEG-2 video (coding mode 1)
    d = Disc.folder(folder_with(tmp_path, [css_scramble(KEY, plain)], v))
    install_log(lib.AV_LOG_DEBUG)
    ret, got = read(d, 1)
    assert ret == 1 and got[CLEAR:] == bytes(plain[CLEAR:])
    assert logged("is right on sector 1 (an MPEG-2 sequence header's extension)")
    d.close()


def test_a_key_the_check_finds_wrong_leaves_the_scrambled_sectors_unreadable(tmp_path):
    # the frame's CRC is wrong in the plain text itself: the check says the key is
    d = Disc.folder(folder_with(tmp_path, [css_scramble(KEY, ac3_pack(good_crc=False))]))
    install_log(lib.AV_LOG_DEBUG)
    ret, _ = read(d, 1)
    assert ret < 0 and css(d) == (-1, None)
    assert logged("the CSS title key is wrong on sector 1 (an AC-3 frame's CRC)")
    install_log(lib.AV_LOG_DEBUG)
    assert read(d, 1)[0] < 0 and not logged("looking for"), "not looked for again"
    d.close()


def test_sectors_that_are_no_pack_are_left_out_and_others_used_as_they_are(tmp_path):
    plain = ac3_pack()
    plain[20] &= 0xcf                           # not scrambled
    padding = bytearray(pack(0xbe, b"", False))
    padding[20] |= 0x30                         # a padding stream: never descrambled
    mpeg1 = bytearray(plain)
    mpeg1[4] = 0x21                             # an MPEG-1 pack header
    d = Disc.folder(folder_with(tmp_path, [bytes(plain), bytes(padding), bytes(mpeg1)]))
    install_log(lib.AV_LOG_DEBUG)
    assert read(d, 1) == (1, bytes(plain))
    assert read(d, 2) == (1, bytes(padding))
    assert read(d, 3)[0] == 0 and read(d, 5)[0] == 0      # MPEG-1 pack; a zero sector
    # libdvdread says each damaged range once, when it ends (reading sector 5
    # ends sector 3's; closing ends sector 5's)
    assert logged("VTS_01_1.VOB: title VOB sectors 3-3 (VTS_01_1.VOB bytes 6144-8191): no MPEG-2 pack with a "
                  "packet: left out")
    assert not logged("title VOB sectors 5-5")
    assert css(d) == (0, None), "no scrambled sector read"
    d.close()
    assert logged("VTS_01_1.VOB: title VOB sectors 5-5 (VTS_01_1.VOB bytes 10240-12287): every byte is zero: left out")


def test_a_title_set_inside_another_ones_title_vobs_takes_its_key():
    # an image: both IFO headers put their title VOBs at the same sector
    # (VTS_02's ending earlier), each counted from its own IFO (vtstt_vobs)
    v = vts(vob_sectors=12)
    m = bytes(vmg(nr_vts=2))
    ifo1 = bytearray(dvd.vts_ifo(v))
    ifo2 = bytearray(dvd.vts_ifo(vts(vob_sectors=10)))
    vob = bytearray(dvd.title_vobs(v.vob_sectors, [dvd.nav(0, v.vob_sectors, None)]))
    vob[S:2 * S] = css_scramble(KEY, ac3_pack())
    files = [("VIDEO_TS.IFO", m), ("VIDEO_TS.BUP", m), ("VTS_01_0.IFO", bytes(ifo1)), ("VTS_02_0.IFO", bytes(ifo2)),
             ("VTS_01_1.VOB", bytes(vob[:10 * S])), ("VTS_02_0.BUP", bytes(ifo2)), ("VTS_01_0.BUP", bytes(ifo1))]
    im, where = image(files)
    vob_at = where["VTS_01_1.VOB"]
    for name, ifo, bups in (("VTS_01_0", ifo1, ("VTS_01_0.BUP",)), ("VTS_02_0", ifo2, ("VTS_02_0.BUP",))):
        be32(ifo, 0xc4, vob_at - where[name + ".IFO"])
        for f in (name + ".IFO",) + bups:
            im.put(where[f], bytes(ifo))
    install_log(lib.AV_LOG_DEBUG)
    d = Disc.image(im)
    assert d.ret == 0
    f1, f2 = d.ifo(1), d.ifo(2)
    assert f1.title_vob_base == f2.title_vob_base == vob_at
    assert read(d, 1, 1)[0] == 1 and css(d, 1)[0] == 1
    install_log(lib.AV_LOG_DEBUG)
    ret, got = read(d, 1, 2)
    assert ret == 1 and got[CLEAR:] == bytes(ac3_pack()[CLEAR:])
    assert css(d, 2) == (1, KEY)
    assert logged("VTS_02: CSS title key taken from VTS_01 (its title VOBs hold these)")
    d.close()


def test_corpus_scrambled_disc_every_title_set_key_found_and_checked():
    path = pathlib.Path(env_or_skip("DVD_CSS_DISC"))
    d = open_disc(path)
    assert d.ret == 0
    install_log(lib.AV_LOG_DEBUG)
    found = 0
    buf = ffi.new("uint8_t[]", S)
    for i in range(1, d.d.nb_ifos):
        f = d.d.ifos[i]
        if f == ffi.NULL or f.failed:
            continue
        for s in range(min(f.title_vob_blocks, 20000)):
            assert lib.ff_dvd_title_vob_sector(d.d, f, s, buf) >= 0, (i, s)
            if lib.ff_dvd_css_state(d.d, f, ffi.NULL):
                break
        state = lib.ff_dvd_css_state(d.d, f, ffi.NULL)
        assert state in (0, 1), i
        found += state == 1
    assert found and not logged("is wrong")
    d.close()
