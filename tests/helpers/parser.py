"""FFmpeg's parsers run the way libavformat runs them (bridge.c
tb_parser_run)."""

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
