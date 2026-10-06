"""Our libdvdcss additions: dvdcss_unscramble_sector on built sectors, the
log callback of dvdcss_open_stream_uncached, and with
DVDCSS_SCRAMBLED_IMAGE=<image>:<first block of a title>[,<block>...] the
title key and descrambler give the same sectors as libdvdcss's own
dvdcss_read with DVDCSS_READ_DECRYPT."""

import pytest

from helpers import ffi, lib
from helpers.css import Handle
from helpers.disc import env_or_skip

BLOCK_SIZE = 2048
KEY_SIZE = 5


def sector(bits):
    """A sector with an MPEG pack header and a PES header whose scrambling
    bits (byte 0x14) are bits."""
    s = bytearray(i % 251 for i in range(BLOCK_SIZE))
    s[:4] = bytes([0, 0, 1, 0xba])
    s[14:18] = bytes([0, 0, 1, 0xe0])
    s[0x14] = 0x80 | bits
    return s


def unscramble(key, s):
    buf = ffi.from_buffer(s)
    return lib.dvdcss_unscramble_sector(bytes(key), buf)


def test_a_clear_sector_is_left_as_it_is():
    s = sector(0)
    before = bytes(s)
    assert unscramble([1, 2, 3, 4, 5], s) == 0
    assert s == before


def test_a_scrambled_sector_without_a_key_is_left_as_it_is():
    s = sector(0x10)
    before = bytes(s)
    assert unscramble([0] * KEY_SIZE, s) == -1
    assert s == before


def test_a_scrambled_sector_is_descrambled_from_byte_128_and_its_bits_cleared():
    s = sector(0x30)
    before = bytes(s)
    assert unscramble([1, 2, 3, 4, 5], s) == 1
    assert s[0x14] == 0x80, "scrambling bits cleared"
    assert s[:0x14] == before[:0x14]
    assert s[0x15:0x80] == before[0x15:0x80], "the first 128 bytes are not scrambled"
    assert s[0x80:] != before[0x80:]


# ---- the log callback ----

def test_messages_go_to_the_log_callback(tmp_path):
    path = tmp_path / "dvdcss-log.bin"
    path.write_bytes(bytes(64 * BLOCK_SIZE))
    got = []
    h = Handle(path, log=got)
    key = ffi.new("unsigned char[5]")
    # zeros hold no MPEG pack: libdvdcss cannot crack a key there
    r = lib.dvdcss_title_key(h.css, 0, key)
    h.close()
    assert any(level == lib.DVDCSS_LOG_DEBUG and "stream API" in t for level, t in got), got
    assert r < 0
    assert any(level == lib.DVDCSS_LOG_ERROR for level, _ in got), f"the failure is reported as an error: {got}"


# ---- real discs ----

SECTORS = 4096


@pytest.mark.disc
def test_corpus_title_key_and_descrambler_equal_dvdcss_read():
    spec = env_or_skip("DVDCSS_SCRAMBLED_IMAGE")
    path, blocks = spec.rsplit(":", 1)
    scrambled_seen = 0
    for block in (int(b) for b in blocks.split(",")):
        # libdvdcss's own path.
        own = Handle(path)
        want = ffi.new("uint8_t[]", SECTORS * BLOCK_SIZE)
        assert lib.dvdcss_seek(own.css, block, lib.DVDCSS_SEEK_KEY) == block
        n = lib.dvdcss_read(own.css, want, SECTORS, lib.DVDCSS_READ_DECRYPT)
        want = bytes(want)[:n * BLOCK_SIZE]
        own.close()
        # Ours: the title key, then the raw sectors descrambled one by one.
        ours = Handle(path)
        key = ffi.new("unsigned char[5]")
        r = lib.dvdcss_title_key(ours.css, block, key)
        ours.close()
        assert r == 1, f"block {block}: title key result"
        with open(path, "rb") as f:
            f.seek(block * BLOCK_SIZE)
            raw = bytearray(f.read(len(want)))
        for k in range(0, len(raw), BLOCK_SIZE):
            s = memoryview(raw)[k:k + BLOCK_SIZE]
            if lib.dvdcss_unscramble_sector(bytes(key), ffi.from_buffer(s)) == 1:
                scrambled_seen += 1
            del s
        assert raw == want, f"block {block}: descrambled sectors differ from dvdcss_read"
        print(f"block {block}: key {bytes(key).hex()}, {len(want) // BLOCK_SIZE} sectors equal")
    assert scrambled_seen > 0, "no scrambled sector was seen"
    print(f"{scrambled_seen} scrambled sectors descrambled")
