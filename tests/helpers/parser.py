"""The clip demuxers (src/disc/clip_demux.c on src/mux) run over bytes in
memory: MPEG-PS (tb_ps_read, or FFmpeg's own mpeg.c for an ordinary program
stream) and MPEG-TS over bytes or a file (tb_ts_read)."""

from helpers import ffi, extern, lib


@extern
def tb_py_ps_packet(opaque, id, codec, data, size, pts):
    ffi.from_handle(opaque).append((id, codec, bytes(ffi.buffer(data, size)), pts))


def ps_read(stream, disc=True, lpcm_header=3):
    """The packets over stream (bytes): as disc sectors (our PS clip demuxer,
    src/mux/ps_disc.c, with the LPCM header length) or as an ordinary program
    stream (FFmpeg's own mpeg.c, for comparison). (return code or packet count, [(stream id,
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
    """The payload pieces our TS clip demuxer (src/mux/ts_disc.c) gives for pids over stream (bytes) or the first max_bytes of the file at
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
