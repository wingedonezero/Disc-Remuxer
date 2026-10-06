"""The VobSub index header of a sub-picture track (discrip_spu.c)."""

from helpers import ffi, lib


def vobsub_header(width, height, palette):
    buf = ffi.new("char[1024]")
    n = lib.ff_discrip_vobsub_header(buf, 1024, width, height,
                                     ffi.new("uint32_t[16]", palette) if palette else ffi.NULL)
    assert 0 < n < 1024
    return ffi.string(buf).decode()


def test_the_index_header_converts_the_palette_as_the_reference_does():
    # 0x00 Y Cr Cb: black, white, and a colour whose green shows the
    # coefficients (0.714 x Cb, 0.346 x Cr as the reference applies them)
    pal = [0x00108080] * 16
    pal[1] = 0x00eb8080
    pal[2] = 0x00515a5a  # Y 81, Cr -38, Cb -38
    hdr = vobsub_header(1920, 1080, pal)
    assert hdr.startswith("# VobSub index file, v7 (do not modify this line!)\n")
    assert "\nsize: 1920x1080\n" in hdr
    # Y 81: (255 * 65) / 219 = 75; R = 75 - 1.40222 x 38, G = 75 + (0.71448 + 0.34569) x 38, B = 75 - 1.771 x 38
    luma = 75 * 65536
    red = (luma - 91894 * 38) >> 16
    green = (luma + 46825 * 38 + 22655 * 38) >> 16
    blue = (luma - 116064 * 38) >> 16
    assert f"palette: 000000, ffffff, {red:02x}{green:02x}{blue:02x}, 000000," in hdr, hdr
