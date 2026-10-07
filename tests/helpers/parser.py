"""FFmpeg's parsers run the way libavformat runs them (bridge.c
tb_parser_run), MPEG-PS demuxers run over bytes in memory (tb_ps_read) and
the MPEG-TS demuxer in disc mode over bytes or a file (tb_ts_read)."""

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


def ps_read(stream, disc=True, lpcm_header=3):
    """The packets FFmpeg's MPEG-PS demuxer (mpeg.c) gives over stream
    (bytes): as disc sectors (option disc, with the LPCM header length) or as
    an ordinary program stream. (return code or packet count, [(stream id,
    codec id, payload, pts)])."""
    data = ffi.new("uint8_t[]", stream)
    out = []
    h = ffi.new_handle(out)
    ret = lib.tb_ps_read(1 if disc else 0, lpcm_header, data, len(stream), lib.tb_py_ps_packet, h)
    return ret, out


@extern
def tb_py_ts_packet(opaque, id, data, size, pts, pos):
    ffi.from_handle(opaque).append((id, bytes(ffi.buffer(data, size)), pts, pos))


NOPTS = -0x8000000000000000


def ts_read(stream=None, pids=(), path=None, max_bytes=0):
    """The payload pieces FFmpeg's MPEG-TS demuxer (mpegts.c) gives in disc
    mode for pids over stream (bytes) or the first max_bytes of the file at
    path: (return code or piece count, [(stream id = PID | key << 16,
    payload, pts, pos)])."""
    out = []
    h = ffi.new_handle(out)
    p = ffi.new("int[]", list(pids) or [0])
    if stream is not None:
        data = ffi.new("uint8_t[]", stream)
        ret = lib.tb_ts_read(data, len(stream), ffi.NULL, 0, p, len(pids), lib.tb_py_ts_packet, h)
    else:
        ret = lib.tb_ts_read(ffi.NULL, 0, str(path).encode(), max_bytes, p, len(pids), lib.tb_py_ts_packet, h)
    return ret, out
