"""libavformat/discio_read.c: block reads with retries, the block-by-block
fallback after the first retry, 0xFF padding of a last partial block, and
reads at any offset."""

from helpers import EINVAL, EIO, averror, ffi, lib
from helpers.source import Source

DATA, FAIL = "data", "fail"


def short(n):
    """Return only n bytes."""
    return ("short", n)


class Mem:
    """Answers each read call (0-based call index) as told; records the
    calls as (pos, length)."""

    def __init__(self, data, answers=None):
        self.data = bytes(data)
        self.answers = dict(answers or {})
        self.calls = []

    def read_at(self, pos, length):
        answer = self.answers.get(len(self.calls), DATA)
        self.calls.append((pos, length))
        if answer == FAIL:
            return averror(EIO)
        k = length
        if isinstance(answer, tuple):
            k = min(k, answer[1])
        return self.data[pos:pos + k]


class Reader:
    def __init__(self, data, size=None, answers=None):
        self.mem = Mem(data, answers)
        self.src = Source(self.mem, size)

    def blocks(self, pos, buf, attempts):
        return lib.ff_discio_read_blocks(self.src.ptr, pos, ffi.from_buffer(buf), len(buf), attempts, 1)

    def bytes(self, pos, buf, attempts):
        return lib.ff_discio_read_bytes(self.src.ptr, pos, ffi.from_buffer(buf), len(buf), attempts, 1)


def pattern(n):
    return bytes(i % 251 for i in range(n))


def test_whole_blocks_read_in_one_call():
    r = Reader(pattern(4 * 2048))
    b = bytearray(2 * 2048)
    assert r.blocks(2048, b, 5) == 0
    assert b == pattern(4 * 2048)[2048:3 * 2048]
    assert len(r.mem.calls) == 1


def test_last_partial_block_is_padded_with_ff():
    r = Reader(pattern(2048 + 100))
    b = bytearray(2048)
    assert r.blocks(2048, b, 5) == 0
    assert b[:100] == pattern(2148)[2048:]
    assert all(x == 0xFF for x in b[100:])


def test_after_a_retry_the_rest_is_read_block_by_block():
    r = Reader(pattern(3 * 2048), answers={0: FAIL})
    b = bytearray(3 * 2048)
    assert r.blocks(0, b, 5) == 0
    assert b == pattern(3 * 2048)
    assert r.mem.calls == [(0, 6144), (0, 2048), (2048, 2048), (4096, 2048)]


def test_gives_up_after_the_attempt_count():
    r = Reader(pattern(2048), answers={0: FAIL, 1: FAIL, 2: FAIL})
    b = bytearray(2048)
    assert r.blocks(0, b, 3) < 0
    assert len(r.mem.calls) == 3


def test_zero_attempts_counts_as_one():
    r = Reader(pattern(2048), answers={0: FAIL})
    b = bytearray(2048)
    assert r.blocks(0, b, 0) < 0
    assert len(r.mem.calls) == 1


def test_no_data_is_a_failed_attempt():
    # the source says 4 blocks but holds 1 (a file cut short)
    r = Reader(pattern(2048), size=4 * 2048)
    b = bytearray(2 * 2048)
    assert r.blocks(2048, b, 5) < 0
    assert len(r.mem.calls) == 5


def test_a_part_of_a_block_is_retried():
    r = Reader(pattern(2 * 2048), answers={0: short(1000)})
    b = bytearray(2 * 2048)
    assert r.blocks(0, b, 5) == 0
    assert b == pattern(2 * 2048)
    assert r.mem.calls == [(0, 4096), (0, 2048), (2048, 2048)]


def test_unaligned_block_reads_are_refused():
    r = Reader(pattern(4096))
    assert r.blocks(1, bytearray(2048), 1) < 0
    assert r.blocks(0, bytearray(100), 1) < 0
    assert r.mem.calls == []


def test_bytes_at_any_offset():
    data = pattern(5 * 2048)
    r = Reader(data)
    for pos, length in [(0, 10), (100, 3000), (2047, 2), (1000, 3 * 2048), (4096, 2048), (5 * 2048 - 7, 7)]:
        b = bytearray(length)
        assert r.bytes(pos, b, 5) == 0, f"pos {pos} len {length}"
        assert b == data[pos:pos + length], f"pos {pos} len {length}"


def test_host_file_source(tmp_path):
    path = tmp_path / "image.bin"
    path.write_bytes(pattern(3 * 2048 + 5))
    src = ffi.new("DiscIOSource **")
    assert lib.ff_discio_source_open_file(ffi.NULL, str(path).encode(), src) == 0
    b = bytearray(2 * 2048)
    # blocks 2 and 3: block 3 holds the file's last 5 bytes, then 0xFF
    assert lib.ff_discio_read_blocks(src[0], 4096, ffi.from_buffer(b), 4096, 5, 1) == 0
    assert b[:2048] == pattern(3 * 2048)[4096:6144]
    assert b[2048:2053] == pattern(3 * 2048 + 5)[3 * 2048:]
    assert all(x == 0xFF for x in b[2053:])
    lib.ff_discio_source_free(src)
    assert src[0] == ffi.NULL


def test_only_regular_files_open_as_host_files(tmp_path):
    src = ffi.new("DiscIOSource **")
    ret = lib.ff_discio_source_open_file(ffi.NULL, str(tmp_path).encode(), src)
    assert ret == averror(EINVAL), "a directory is refused (EINVAL)"
    assert src[0] == ffi.NULL


def test_file_reads_cross_extents_and_not_recorded_runs_read_as_zeros():
    data = pattern(4 * 2048)
    r = Reader(data)
    fs = ffi.new("DiscIOFS *", {"src": r.src.ptr})
    # file = source block 3, then a block that is not recorded, then block 1
    ext = ffi.new("DiscIOExtent[]", [(3, 1), (lib.DISCIO_SECTOR_NOT_RECORDED, 1), (1, 1)])
    f = ffi.new("DiscIOFile *", {"size": 3 * 2048 - 10, "nb_extents": 3, "extents": ext})
    b = bytearray([0xAA]) * (3 * 2048 - 10)
    assert lib.ff_discio_file_read(fs, f, 0, ffi.from_buffer(b), len(b)) == 0
    assert b[:2048] == data[3 * 2048:]
    assert all(x == 0 for x in b[2048:4096])
    assert b[4096:] == data[2048:2 * 2048 - 10]
    # past the end of the file
    one = bytearray(20)
    assert lib.ff_discio_file_read(fs, f, 3 * 2048 - 15, ffi.from_buffer(one), 20) < 0
