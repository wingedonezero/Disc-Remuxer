"""HEVC (ITU-T H.265) NAL units built bit by bit: video, sequence and
picture parameter sets, slice segment headers, SEI messages (picture timing,
recovery point), delimiters, end and filler units, with emulation
prevention, for the disc-mode access-unit tests. Picture data after a slice
segment header is filler bytes (the parser reads headers only)."""

from synth.h264 import Bits, escape

# nal_unit_type (H.265 Table 7-1)
TRAIL_N, TRAIL_R, RASL_N, RASL_R = 0, 1, 8, 9
IDR_W_RADL, IDR_N_LP, CRA = 19, 20, 21
VPS, SPS, PPS, AUD, EOS, EOB, FD, SEI_PREFIX, SEI_SUFFIX = 32, 33, 34, 35, 36, 37, 38, 39, 40

WIDTH, HEIGHT, CTB_LOG2 = 3840, 2160, 6


def nal(t, rbsp, layer=0, tid=0, sc=b"\0\0\0\1"):
    """A NAL unit with its start code and 2-byte header."""
    return sc + bytes([t << 1 | layer >> 5, (layer & 31) << 3 | (tid + 1)]) + escape(rbsp)


def _ptl(b, interlaced=False):
    """profile_tier_level(1, 0): Main 10, level 5.1, progressive frames
    (interlaced: the source flags say progressive and interlaced)."""
    b.u(0, 2).u(0, 1).u(2, 5)                    # profile space, tier, Main 10
    b.u(1 << 30 | 1 << 29, 32)                   # compatible with Main and Main 10
    b.u(1, 1).u(int(interlaced), 1).u(0, 1).u(0 if interlaced else 1, 1)
    b.u(0, 32).u(0, 11).u(0, 1)                  # 43 reserved bits, inbld
    b.u(153, 8)                                  # general_level_idc 5.1


def vps(timing=None, sc=b"\0\0\0\1"):
    b = Bits()
    b.u(0, 4).u(1, 1).u(1, 1).u(0, 6).u(0, 3).u(1, 1).u(0xffff, 16)
    _ptl(b)
    b.u(1, 1).ue(4).ue(2).ue(0)                  # sub-layer ordering info
    b.u(0, 6).ue(0)                              # vps_max_layer_id, vps_num_layer_sets_minus1
    if timing:
        b.u(1, 1).u(timing[0], 32).u(timing[1], 32).u(0, 1).ue(0)
    else:
        b.u(0, 1)
    b.u(0, 1)                                    # vps_extension_flag
    return nal(VPS, b.trailing(), sc=sc)


def sps(log2_poc_lsb=8, vui=True, frame_field_info=True, field_seq=False, timing=(1001, 24000),
        interlaced=False, sc=b"\0\0\0\1"):
    """A Main 10 SPS, 3840 x 2160, CTB 64, with a VUI (timing info,
    frame_field_info_present_flag)."""
    b = Bits()
    b.u(0, 4).u(0, 3).u(1, 1)                    # VPS 0, one sub-layer, temporal id nesting
    _ptl(b, interlaced)
    b.ue(0).ue(1).ue(WIDTH).ue(HEIGHT).u(0, 1)   # SPS 0, 4:2:0, size, no conformance window
    b.ue(2).ue(2).ue(log2_poc_lsb - 4)           # 10 bit, POC lsb bits
    b.u(1, 1).ue(4).ue(2).ue(0)                  # sub-layer ordering info
    b.ue(0).ue(CTB_LOG2 - 3).ue(0).ue(3).ue(0).ue(0)    # coding / transform block sizes
    b.u(0, 1).u(0, 1).u(0, 1).u(0, 1)            # scaling lists, AMP, SAO, PCM
    b.ue(0).u(0, 1).u(0, 1).u(0, 1)              # short-term RPS 0, long-term refs, TMVP, smoothing
    b.u(int(vui), 1)
    if vui:
        b.u(0, 1).u(0, 1).u(0, 1).u(0, 1)        # aspect ratio, overscan, video signal, chroma loc
        b.u(0, 1).u(int(field_seq), 1).u(int(frame_field_info), 1).u(0, 1)
        if timing:
            b.u(1, 1).u(timing[0], 32).u(timing[1], 32).u(0, 1).u(0, 1)
        else:
            b.u(0, 1)
        b.u(0, 1)                                # bitstream_restriction_flag
    b.u(0, 1)                                    # sps_extension_present_flag
    return nal(SPS, b.trailing(), sc=sc)


def pps(pps_id=0, sc=b"\0\0\0\1"):
    b = Bits()
    b.ue(pps_id).ue(0).u(0, 1).u(0, 1).u(0, 3).u(0, 1).u(0, 1)
    b.ue(0).ue(0).se(0).u(0, 1).u(0, 1).u(0, 1).se(0).se(0)
    b.u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1)
    b.ue(0).u(0, 1).u(0, 1)
    return nal(PPS, b.trailing(), sc=sc)


def aud(pic_type=2):
    return nal(AUD, Bits().u(pic_type, 3).trailing())


def sei(payloads, suffix=False, sc=b"\0\0\0\1"):
    """An SEI NAL unit of (payloadType, payload bytes) messages."""
    body = bytearray()
    for t, p in payloads:
        body += bytes([t, len(p)]) + p
    body += b"\x80"
    return nal(SEI_SUFFIX if suffix else SEI_PREFIX, bytes(body), sc=sc)


def pic_timing(pic_struct):
    """A picture timing SEI payload with frame_field_info (no HRD)."""
    return (1, Bits().u(pic_struct, 4).u(1, 2).u(0, 1).trailing())


def recovery_point(cnt=0):
    return (6, Bits().se(cnt).u(1, 1).u(0, 1).trailing())


SLICE_B, SLICE_P, SLICE_I = 0, 1, 2
CTBS = ((WIDTH + 63) >> 6) * ((HEIGHT + 63) >> 6)


def slice_(t, kind, poc_lsb=0, first=True, address=0, pps_id=0, log2_poc_lsb=8, size=300, layer=0,
           sc=b"\0\0\1"):
    """A slice segment of nal_unit_type t: its header up to the picture
    order count, then picture data bytes."""
    b = Bits()
    b.u(int(first), 1)
    if 16 <= t <= 23:
        b.u(0, 1)                                # no_output_of_prior_pics_flag
    b.ue(pps_id)
    if not first:
        b.u(address, (CTBS - 1).bit_length())
    b.ue(kind)
    if t not in (IDR_W_RADL, IDR_N_LP):
        b.u(poc_lsb, log2_poc_lsb)
        b.u(0, 1)                                # short_term_ref_pic_set_sps_flag
    body = b.aligned() + bytes((0x55 + i) & 0xff or 1 for i in range(size))
    return nal(t, body, layer=layer)


def end_of_seq():
    return nal(EOS, b"")


def filler(n=16):
    return nal(FD, b"\xff" * n + b"\x80")


def nal_types(stream):
    """nal_unit_types of an Annex B stream, in order."""
    out, i = [], stream.find(b"\0\0\1")
    while i >= 0 and i + 3 < len(stream):
        out.append((stream[i + 3] >> 1) & 0x3f)
        i = stream.find(b"\0\0\1", i + 3)
    return out
