"""The static FFmpeg build links and is the one in libs/."""

from helpers import ffi, lib


def test_version_is_the_libs_copy():
    assert ffi.string(lib.av_version_info()).decode() == "9.0.2"


def test_disc_demuxers_are_built():
    assert lib.av_find_input_format(b"dvdvideo") != ffi.NULL
    assert lib.av_find_input_format(b"disc") == ffi.NULL    # the disc layer is ours (src/disc), not a demuxer
    assert lib.av_find_input_format(b"hddvd") == ffi.NULL   # a format part of the disc layer, not a demuxer
