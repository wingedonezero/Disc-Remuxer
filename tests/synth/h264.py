"""H.264 (ITU-T H.264) NAL units built bit by bit: sequence and picture
parameter sets, slice headers, SEI messages (picture timing, recovery
point), delimiters, filler and end units, with emulation prevention, for
the disc-mode access-unit tests. Picture data after a slice header is
filler bytes (the parser reads headers only)."""


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.cur = 0
        self.n = 0

    def u(self, v, bits):
        for i in range(bits - 1, -1, -1):
            self.cur = (self.cur << 1) | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.cur)
                self.cur = 0
                self.n = 0
        return self

    def ue(self, v):
        v += 1
        k = v.bit_length()
        return self.u(0, k - 1).u(v, k)

    def se(self, v):
        return self.ue(2 * v - 1 if v > 0 else -2 * v)

    def trailing(self):
        """rbsp_trailing_bits: a 1, then zero bits to the byte boundary."""
        self.u(1, 1)
        while self.n:
            self.u(0, 1)
        return bytes(self.out)

    def aligned(self):
        """The bits so far, zero-padded to whole bytes (no trailing 1)."""
        while self.n:
            self.u(0, 1)
        return bytes(self.out)


def escape(rbsp):
    """emulation_prevention_three_byte inserted (H.264 7.4.1)."""
    out, zeros = bytearray(), 0
    for b in rbsp:
        if zeros >= 2 and b <= 3:
            out.append(3)
            zeros = 0
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def nal(t, rbsp, ref_idc=3, sc=b"\0\0\0\1"):
    """A NAL unit with its start code."""
    return sc + bytes([ref_idc << 5 | t]) + escape(rbsp)


def sps(sps_id=0, log2_frame_num=4, poc_type=0, log2_poc_lsb=6, frame_mbs_only=1, timing=(1001, 48000),
        hrd=True, pic_struct=True, sc=b"\0\0\0\1"):
    """A High profile SPS, 1920 x 1080, with a VUI (timing info, NAL HRD
    parameters, pic_struct_present_flag)."""
    b = Bits()
    b.u(100, 8).u(0, 8).u(41, 8).ue(sps_id)
    b.ue(1).ue(0).ue(0).u(0, 1).u(0, 1)          # 4:2:0, 8 bit, no scaling matrices
    b.ue(log2_frame_num - 4).ue(poc_type)
    if poc_type == 0:
        b.ue(log2_poc_lsb - 4)
    b.ue(4).u(0, 1)                              # max_num_ref_frames, gaps
    b.ue(119).ue(67 if frame_mbs_only else 33).u(frame_mbs_only, 1)
    if not frame_mbs_only:
        b.u(0, 1)                                # mb_adaptive_frame_field_flag
    b.u(1, 1)                                    # direct_8x8_inference_flag
    b.u(1, 1).ue(0).ue(0).ue(0).ue(4)            # cropping to 1080 lines
    b.u(1, 1)                                    # vui_parameters_present_flag
    b.u(0, 1).u(0, 1).u(0, 1).u(0, 1)            # aspect ratio, overscan, video signal, chroma loc
    if timing:
        b.u(1, 1).u(timing[0], 32).u(timing[1], 32).u(1, 1)
    else:
        b.u(0, 1)
    if hrd:
        b.u(1, 1)                                # nal_hrd_parameters_present_flag
        b.ue(0).u(4, 4).u(6, 4).ue(1000).ue(2000).u(0, 1)
        b.u(23, 5).u(23, 5).u(23, 5).u(24, 5)    # delay lengths 24 bits, time_offset_length
    else:
        b.u(0, 1)
    b.u(0, 1)                                    # vcl_hrd_parameters_present_flag
    if hrd:
        b.u(0, 1)                                # low_delay_hrd_flag
    b.u(int(pic_struct), 1).u(0, 1)              # pic_struct_present_flag, bitstream_restriction_flag
    return nal(7, b.trailing(), sc=sc)


def pps(pps_id=0, sps_id=0, bottom_field_pic_order=0, sc=b"\0\0\0\1"):
    b = Bits()
    b.ue(pps_id).ue(sps_id).u(1, 1).u(bottom_field_pic_order, 1).ue(0)
    b.ue(2).ue(1).u(0, 1).u(0, 2).se(0).se(0).se(0).u(1, 1).u(0, 1).u(0, 1)
    return nal(8, b.trailing(), sc=sc)


def aud(primary_pic_type=7):
    return nal(9, Bits().u(primary_pic_type, 3).trailing(), ref_idc=0)


def sei(payloads, sc=b"\0\0\0\1"):
    """An SEI NAL unit of (payloadType, payload bytes) messages."""
    body = bytearray()
    for t, p in payloads:
        body += bytes([t]) + bytes([len(p)])
        body += p
    body += b"\x80"
    return nal(6, bytes(body), ref_idc=0, sc=sc)


def pic_timing(pic_struct, delays=True):
    """A picture timing SEI payload: CpbDpbDelays (24 bits each, as sps()
    sets them), pic_struct and no clock timestamps."""
    b = Bits()
    if delays:
        b.u(0, 24).u(2, 24)
    if pic_struct is not None:
        b.u(pic_struct, 4)
        n = {0: 1, 1: 1, 2: 1, 3: 2, 4: 2, 5: 3, 6: 3, 7: 2, 8: 3}[pic_struct]
        for _ in range(n):
            b.u(0, 1)                            # clock_timestamp_flag
    return (1, b.trailing())


def recovery_point(frames=0):
    return (6, Bits().ue(frames).u(1, 1).u(0, 1).u(0, 2).trailing())


SLICE_P, SLICE_B, SLICE_I = 0, 1, 2


def slice_(kind, frame_num, poc_lsb, idr=False, ref=True, first_mb=0, pps_id=0, log2_frame_num=4, log2_poc_lsb=6,
           field=None, idr_id=0, size=300, slice_type=None, mmco5=False, sc=b"\0\0\1"):
    """A slice (type 1, or 5 for an IDR picture): its header up to
    dec_ref_pic_marking, then picture data bytes. field: None (frame), 'top'
    or 'bottom' (an interlaced SPS). mmco5: dec_ref_pic_marking with
    memory_management_control_operation 5."""
    b = Bits()
    b.ue(first_mb).ue(kind + 5 if slice_type is None else slice_type).ue(pps_id).u(frame_num, log2_frame_num)
    if field is not None:
        b.u(1 if field else 0, 1)
        if field:
            b.u(1 if field == "bottom" else 0, 1)
    if idr:
        b.ue(idr_id)
    b.u(poc_lsb, log2_poc_lsb)
    if kind == SLICE_B:
        b.u(1, 1)                                # direct_spatial_mv_pred_flag
    if kind in (SLICE_P, SLICE_B):
        b.u(0, 1)                                # num_ref_idx_active_override_flag
        b.u(0, 1)                                # ref_pic_list_modification_flag_l0
        if kind == SLICE_B:
            b.u(0, 1)
    if ref:
        if idr:
            b.u(0, 1).u(0, 1)                    # no_output_of_prior_pics, long_term_reference_flag
        elif mmco5:
            b.u(1, 1).ue(5).ue(0)                # adaptive marking: operation 5, end
        else:
            b.u(0, 1)                            # adaptive_ref_pic_marking_mode_flag
    b.se(0)                                      # slice_qp_delta
    body = b.aligned() + bytes((0x55 + i) & 0xff or 1 for i in range(size))
    return nal(5 if idr else 1, body, ref_idc=3 if ref else 0, sc=sc)


def filler(n=16):
    return nal(12, b"\xff" * n + b"\x80", ref_idc=0)


def end_of_seq():
    return nal(10, b"", ref_idc=0)


def nal_types(stream):
    """nal_unit_types of an Annex B stream, in order."""
    out, i = [], stream.find(b"\0\0\1")
    while i >= 0 and i + 3 < len(stream):
        out.append(stream[i + 3] & 0x1f)
        i = stream.find(b"\0\0\1", i + 3)
    return out
