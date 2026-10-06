"""The compiled libaacs, libgcrypt and libgpg-error link statically and are
our versions."""

from helpers import ffi, lib


def test_the_compiled_versions_are_ours():
    v = [ffi.new("int *") for _ in range(3)]
    lib.aacs_get_version(*v)
    aacs = ".".join(str(x[0]) for x in v)
    assert aacs == ffi.string(lib.tb_aacs_header_version()).decode() == "0.12.0"
    gcrypt = ffi.string(lib.gcry_check_version(ffi.NULL)).decode()
    assert gcrypt == ffi.string(lib.tb_gcrypt_header_version()).decode() == "1.12.4"
