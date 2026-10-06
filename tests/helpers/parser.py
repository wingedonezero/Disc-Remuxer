"""FFmpeg's parsers run the way libavformat runs them (bridge.c
tb_parser_run), and MPEG-PS demuxers run over bytes in memory (tb_ps_read)."""

from helpers import ffi, extern, lib


@extern
def tb_py_parser_frame(opaque, size, pts, dts):
    ffi.from_handle(opaque).append((size, pts, dts))


def run(codec_name, packets):
    """The frames FFmpeg's parser for codec_name returns over packets
    [(bytes, pts)]: (return code, [(size, pts, dts)])."""
    datas = [ffi.new("uint8_t[]", bytes(d) or b"\0") for d, _ in packets]
    ptrs = ffi.new("const uint8_t *[]", datas or [ffi.NULL])
    sizes = ffi.new("int[]", [len(d) for d, _ in packets] or [0])
    pts = ffi.new("int64_t[]", [t for _, t in packets] or [0])
    frames = []
    h = ffi.new_handle(frames)
    ret = lib.tb_parser_run(codec_name.encode(), ptrs, sizes, pts, len(packets), lib.tb_py_parser_frame, h)
    return ret, frames


@extern
def tb_py_ps_packet(opaque, id, codec, data, size, pts):
    ffi.from_handle(opaque).append((id, codec, bytes(ffi.buffer(data, size)), pts))


def ps_read(stream, ours=True):
    """The packets an MPEG-PS demuxer gives over stream (bytes): ours
    (discrip_ps.c) or FFmpeg's mpeg.c. (return code, [(stream id, codec id,
    payload, pts)])."""
    data = ffi.new("uint8_t[]", stream)
    out = []
    h = ffi.new_handle(out)
    ret = lib.tb_ps_read(1 if ours else 0, data, len(stream), lib.tb_py_ps_packet, h)
    return ret, out
