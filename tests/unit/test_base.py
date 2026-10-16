"""src/base: errors (err.c), memory (mem.c) and refcounted buffers (buf.c)
— the pieces every module of the program may use, with no FFmpeg under
them."""

from helpers import ffi, lib


# ---- errors -----------------------------------------------------------------

def err_str(err):
    buf = ffi.new("char[128]")
    return ffi.string(lib.dr_err_str(err, buf, 128)).decode()


def test_own_codes_have_words():
    assert err_str(lib.DR_OK) == "success"
    assert err_str(lib.DR_ERR_EOF) == "end of the data"
    assert err_str(lib.DR_ERR_INVALID) == "data that breaks its own rules"
    assert err_str(lib.DR_ERR_NOMEM) == "out of memory"
    assert err_str(lib.DR_ERR_LIMIT) == "a bound was reached"
    assert err_str(lib.DR_ERR_CANCELLED) == "stopped"
    assert err_str(lib.DR_ERR_EXTERNAL) == "a library or helper failed"


def test_own_codes_are_distinct_and_negative():
    codes = [lib.DR_ERR_EOF, lib.DR_ERR_INVALID, lib.DR_ERR_NOMEM,
             lib.DR_ERR_LIMIT, lib.DR_ERR_CANCELLED, lib.DR_ERR_EXTERNAL]
    assert len(set(codes)) == len(codes)
    assert all(c < 0 for c in codes)
    assert lib.DR_OK == 0


def test_system_errors_use_errno_words():
    import errno
    import os
    assert err_str(-errno.ENOENT) == os.strerror(errno.ENOENT)


# ---- memory -----------------------------------------------------------------

def test_memdup_copies_and_null_stays_null():
    p = lib.dr_memdup(b"abc", 3)
    assert ffi.buffer(p, 3)[:] == b"abc"
    lib.dr_free(p)
    assert lib.dr_memdup(ffi.NULL, 3) == ffi.NULL
    assert lib.dr_strdup(ffi.NULL) == ffi.NULL


def test_strdup_copies():
    p = lib.dr_strdup(b"hello")
    assert ffi.string(p) == b"hello"
    lib.dr_free(p)


def test_freep_clears_the_pointer():
    holder = ffi.new("char **")
    holder[0] = lib.dr_strdup(b"x")
    lib.dr_freep(holder)
    assert holder[0] == ffi.NULL
    lib.dr_freep(holder)            # NULL again: nothing happens


def test_calloc_checks_overflow():
    assert lib.dr_calloc(2**63, 16) == ffi.NULL
    assert lib.dr_realloc_array(ffi.NULL, 2**63, 16) == ffi.NULL
    p = lib.dr_calloc(4, 4)
    assert ffi.buffer(p, 16)[:] == bytes(16)
    lib.dr_free(p)


# ---- buffers ----------------------------------------------------------------

def test_a_buffer_holds_its_bytes_and_zeroed_padding():
    b = ffi.new("DrBuf **")
    b[0] = lib.dr_buf_from(b"hello", 5)
    assert lib.dr_buf_size(b[0]) == 5
    data = lib.dr_buf_data(b[0])
    assert ffi.buffer(data, 5)[:] == b"hello"
    assert ffi.buffer(data + 5, lib.DR_BUF_PADDING)[:] == bytes(lib.DR_BUF_PADDING)
    lib.dr_buf_unref(b)
    assert b[0] == ffi.NULL


def test_writable_only_while_one_owner():
    b = ffi.new("DrBuf **")
    b[0] = lib.dr_buf_alloc(8)
    assert lib.dr_buf_writable(b[0]) == 1
    other = ffi.new("DrBuf **")
    other[0] = lib.dr_buf_ref(b[0])
    assert lib.dr_buf_writable(b[0]) == 0
    lib.dr_buf_unref(other)
    assert lib.dr_buf_writable(b[0]) == 1
    lib.dr_buf_unref(b)


def test_the_last_unref_frees_and_earlier_ones_do_not():
    b = ffi.new("DrBuf **")
    b[0] = lib.dr_buf_from(b"data", 4)
    keep = ffi.new("DrBuf **")
    keep[0] = lib.dr_buf_ref(b[0])
    lib.dr_buf_unref(b)
    # still alive through the second owner
    assert ffi.buffer(lib.dr_buf_data(keep[0]), 4)[:] == b"data"
    lib.dr_buf_unref(keep)


def test_trim_shrinks_and_keeps_the_padding_rule():
    b = ffi.new("DrBuf **")
    b[0] = lib.dr_buf_from(b"abcdef", 6)
    lib.dr_buf_trim(b[0], 3)
    assert lib.dr_buf_size(b[0]) == 3
    data = lib.dr_buf_data(b[0])
    assert ffi.buffer(data, 3)[:] == b"abc"
    assert ffi.buffer(data + 3, lib.DR_BUF_PADDING)[:] == bytes(lib.DR_BUF_PADDING)
    lib.dr_buf_trim(b[0], 9)        # never grows
    assert lib.dr_buf_size(b[0]) == 3
    lib.dr_buf_unref(b)


def test_an_empty_buffer_works():
    b = ffi.new("DrBuf **")
    b[0] = lib.dr_buf_alloc(0)
    assert lib.dr_buf_size(b[0]) == 0
    assert ffi.buffer(lib.dr_buf_data(b[0]), lib.DR_BUF_PADDING)[:] == bytes(lib.DR_BUF_PADDING)
    lib.dr_buf_unref(b)


# ---- base/str.h: the growing text buffer ----

def test_str_appends_formatted_pieces_into_one_string():
    s = ffi.new("DrStr *")
    lib.dr_str_init(s)
    lib.dr_str_catf(s, b"%s", ffi.cast("char *", ffi.new("char[]", b"hello")))
    lib.dr_str_catf(s, b", %d", ffi.cast("int", 42))
    assert lib.dr_str_ok(s)
    assert ffi.string(s.str) == b"hello, 42"
    assert s.len == len(b"hello, 42")
    lib.dr_str_free(s)


def test_str_grows_past_its_first_buffer():
    s = ffi.new("DrStr *")
    lib.dr_str_init(s)
    piece = b"x" * 100
    for _ in range(10):
        lib.dr_str_catf(s, b"%s", ffi.cast("char *", ffi.new("char[]", piece)))
    assert lib.dr_str_ok(s)
    assert ffi.string(s.str) == piece * 10
    lib.dr_str_free(s)


def test_str_take_hands_the_string_over_and_resets():
    s = ffi.new("DrStr *")
    lib.dr_str_init(s)
    lib.dr_str_catf(s, b"kept")
    p = lib.dr_str_take(s, 0)
    assert ffi.string(p) == b"kept"
    lib.dr_free(p)
    assert s.len == 0 and ffi.string(s.str) == b""
    assert lib.dr_str_take(s, 0) == ffi.NULL, "nothing appended: NULL"
    q = lib.dr_str_take(s, 1)
    assert ffi.string(q) == b"", "keep_empty: an empty string"
    lib.dr_free(q)


# ---- base/mem.h: the array appends ----

def test_array_add_ptr_appends_and_keeps_order():
    arr = ffi.new("char **[1]")
    nb = ffi.new("int *")
    keep = []
    for i in range(10):
        item = ffi.new("char[]", b"i%d" % i)
        keep.append(item)
        assert lib.dr_array_add_ptr(arr, nb, item) == 0
    assert nb[0] == 10
    for i in range(10):
        assert ffi.string(arr[0][i]) == b"i%d" % i


def test_array_add_copies_values():
    arr = ffi.new("int *[1]")
    nb = ffi.new("int *")
    for i in range(9):
        v = ffi.new("int *", i * 7)
        assert lib.dr_array_add(arr, nb, ffi.sizeof("int"), v) != ffi.NULL
    assert nb[0] == 9
    assert [arr[0][i] for i in range(9)] == [i * 7 for i in range(9)]


# ---- base/sha.h, base/crc32.h ----

def _sha(bits, data):
    s = ffi.new("DrSha *")
    out = ffi.new("uint8_t[]", bits // 8)
    lib.dr_sha_init(s, bits)
    lib.dr_sha_update(s, data, len(data))
    lib.dr_sha_final(s, out)
    return bytes(ffi.buffer(out))


def test_sha1_matches_the_fips_vectors():
    assert _sha(160, b"abc").hex() == "a9993e364706816aba3e25717850c26c9cd0d89d"
    assert _sha(160, b"").hex() == "da39a3ee5e6b4b0d3255bfef95601890afd80709"
    assert (_sha(160, b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex()
            == "84983e441c3bd26ebaae4aa1f95129e5e54670f1")


def test_sha256_matches_the_fips_vectors():
    assert _sha(256, b"abc").hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    assert (_sha(256, b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").hex()
            == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")


def test_sha_pads_across_block_boundaries():
    # lengths around the 56/64-byte padding edges, against hashlib
    import hashlib
    for n in (55, 56, 57, 63, 64, 65, 119, 120, 128):
        data = bytes(range(256))[:n] * 1
        assert _sha(160, data) == hashlib.sha1(data).digest(), n
        assert _sha(256, data) == hashlib.sha256(data).digest(), n


def test_crc32_mpeg_vector_and_zero_on_appended_crc():
    v = lib.dr_crc32_mpeg(0xFFFFFFFF, b"123456789", 9)
    assert v == 0x0376E6E7
    data = b"123456789" + v.to_bytes(4, "big")
    assert lib.dr_crc32_mpeg(0xFFFFFFFF, data, len(data)) == 0
