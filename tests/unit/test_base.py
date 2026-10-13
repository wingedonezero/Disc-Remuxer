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
