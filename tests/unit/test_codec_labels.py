"""src/disc-remuxer/writer.c codec_label: the short codec names in the track
list, the GUI and the stream file names, by the codec and its profile (the
decoder's: DTS-HD MA with the DTS:X extension, E-AC-3 with Atmos)."""

from helpers import ffi, lib

MA, MA_X, MA_X_IMAX, HRA, ATMOS = 60, 61, 62, 50, 30
UNKNOWN = -99
AC3, DTS, EAC3, TRUEHD = 86019, 86020, 86056, 86060   # src/timeline/disctl.h


def label(codec, profile):
    return ffi.string(lib.codec_label(codec, profile)).decode()


def test_dts_names_by_profile():
    assert label(DTS, UNKNOWN) == "DTS"
    assert label(DTS, HRA) == "DTS-HD HR"
    assert label(DTS, MA) == "DTS-HD MA"
    assert label(DTS, MA_X) == "DTS:X"
    assert label(DTS, MA_X_IMAX) == "DTS:X"


def test_eac3_with_atmos_and_without():
    assert label(EAC3, UNKNOWN) == "DDplus"
    assert label(EAC3, ATMOS) == "DD+ Atmos"
    # the same profile number on AC-3 means nothing
    assert label(AC3, ATMOS) == "DD"
    assert label(TRUEHD, ATMOS) == "TrueHD Atmos"
