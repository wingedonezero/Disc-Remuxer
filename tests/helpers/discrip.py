"""The rip core's stages run from Python: callbacks (a Python callable
behind each opaque pointer), frames collected as Out values, the audio stage
run over built payloads."""

import dataclasses

from helpers import AV_NOPTS_VALUE, extern, ffi, lib

TICKS = 1080000000


def codec_id(name):
    """FFmpeg's codec id of a codec descriptor name, or None."""
    d = lib.avcodec_descriptor_get_by_name(name.encode())
    return None if d == ffi.NULL else d.id


def fn(name):
    """A C function of the bridge as a function pointer (to hand one stage
    to another)."""
    return ffi.addressof(lib, name)


# ---- callbacks: the opaque pointer is a handle to a Python callable ----

@extern
def tb_py_unit(opaque, unit):
    return ffi.from_handle(opaque)(unit)


@extern
def tb_py_frame(opaque, frame):
    return ffi.from_handle(opaque)(frame)


@extern
def tb_py_event(opaque, ev):
    ffi.from_handle(opaque)(ev)


@extern
def tb_py_join_out(opaque, track, frame):
    return ffi.from_handle(opaque)(track, frame)


@extern
def tb_py_video_max_time(opaque):
    return ffi.from_handle(opaque).max_time()


@extern
def tb_py_video_advance(opaque, target, ended):
    ret, e = ffi.from_handle(opaque).advance(target)
    ended[0] = int(e)
    return ret


class Handles:
    """Keeps the handles given to C alive as long as this object lives."""

    def __init__(self):
        self._keep = []

    def __call__(self, obj):
        h = ffi.new_handle(obj)
        self._keep.append(h)
        return h


@dataclasses.dataclass
class Out:
    """A frame as a stage handed it on."""
    bytes: bytes
    time: int
    dur: int
    flags: int


def frame_bytes(f):
    return bytes(ffi.buffer(f.data, f.size)) if f.size > 0 else b""


def collect(outs):
    """A DRFrameCb body that appends every frame to outs (as Out) and
    releases it."""
    def on_frame(f):
        outs.append(Out(frame_bytes(f), f.time, f.dur, f.flags))
        lib.ff_discrip_frame_unref(f)
        return 0
    return on_frame


def event_dict(ev):
    return {"kind": ev.kind, "track": ev.track, "pos": ev.pos, "dur": ev.dur, "skew": ev.skew, "count": ev.count}


@dataclasses.dataclass
class Run:
    outs: list
    audio: object
    cutter: object


def run(codec, payloads, marker=None, flags=0):
    """Payloads [(bytes, time or None)] of one segment through the cutter
    and the audio stage (flags: audio flags, e.g. DR_AUDIO_CORE_ONLY)."""
    cid = codec_id(codec)
    assert cid is not None
    outs = []
    keep = Handles()
    a = ffi.new("DRAudio **")
    c = ffi.new("DRCutter **")
    assert lib.ff_discrip_audio_open(a, ffi.NULL, cid, flags, lib.tb_py_frame, keep(collect(outs))) == 0
    assert lib.ff_discrip_cutter_open(c, ffi.NULL, cid, fn("ff_discrip_audio_unit"), a[0]) == 0
    for data, time in payloads:
        assert lib.ff_discrip_cutter_write(c[0], bytes(data), len(data),
                                           AV_NOPTS_VALUE if time is None else time) == 0
    assert lib.ff_discrip_cutter_flush(c[0]) == 0
    if marker is not None:
        assert lib.ff_discrip_audio_marker(a[0], marker) == 0
    assert lib.ff_discrip_audio_flush(a[0]) == 0
    audio = ffi.new("DRAudioStats *")
    cutter = ffi.new("DRCutterStats *")
    lib.ff_discrip_audio_stats(a[0], audio)
    lib.ff_discrip_cutter_stats(c[0], cutter)
    lib.ff_discrip_cutter_close(c)
    lib.ff_discrip_audio_close(a)
    return Run(outs, audio, cutter)


def dur(samples, rate):
    return samples * TICKS // rate


def timed(frames):
    """One payload per frame, each with a time (k * 1000)."""
    return [(f, k * 1000) for k, f in enumerate(frames)]


def pictures(s):
    """The pictures of an MPEG-2 video stream (ISO/IEC 13818-2): (payload
    start, payload end, display index, type 1..3). A payload starts at the
    sequence or GOP header before its picture, as PES packets on discs do."""
    sc = [(i, s[i + 3]) for i in range(len(s) - 3) if s[i] == 0 and s[i + 1] == 0 and s[i + 2] == 1]
    out = []
    gop_start, in_gop, start = 0, 0, None
    for i, code in sc:
        if code in (0xB3, 0xB8):
            if code == 0xB8:
                gop_start += in_gop
                in_gop = 0
            if start is None:
                start = i
        elif code == 0x00:
            tr = (s[i + 4] << 2) | (s[i + 5] >> 6)
            out.append([i if start is None else start, 0, gop_start + tr, (s[i + 5] >> 3) & 7])
            start = None
            in_gop += 1
    for k in range(len(out)):
        out[k][1] = out[k + 1][0] if k + 1 < len(out) else len(s)
    return [tuple(p) for p in out]


class Frame:
    """A DRFrame over bytes kept alive by this object (ptr: DRFrame *)."""

    def __init__(self, data, time, dur, flags, pos=0, src=None, samples=0, rate=0):
        self.data = ffi.new("uint8_t[]", bytes(data)) if data else ffi.NULL
        self.ptr = ffi.new("DRFrame *", {
            "buf": ffi.NULL, "data": self.data, "size": len(data), "time": time, "dur": dur, "pos": pos,
            "flags": flags, "samples": samples, "rate": rate, "src": time if src is None else src})


def video_ref(video, keep):
    """A DRVideoRef over a Python object with max_time() and advance(target)
    -> (return code, ended)."""
    return {"opaque": keep(video), "max_time": lib.tb_py_video_max_time, "advance": lib.tb_py_video_advance}


def events_into(lst):
    """A DREventCb body appending every event to lst (as a dict)."""
    return lambda ev: lst.append(event_dict(ev))
