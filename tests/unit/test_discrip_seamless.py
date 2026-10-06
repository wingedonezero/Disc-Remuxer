"""The seamless overlap search of the rip core's junction
(discrip_seamless.c, discrip_junction.c): TrueHD units decoded to one
channel of 32-bit samples, the integer correlation of two stretches, and a
join whose next segment repeats the last two access units of the one before
it. The TrueHD stream is tests/data/sine_4813.thd (FFmpeg's encoder, 16-bit
stereo, a major sync every 16 access units)."""

import math

from helpers import DATA, ffi, lib
from helpers.discrip import Frame, Handles, codec_id, events_into, video_ref

THD = (DATA / "sine_4813.thd").read_bytes()
AU = 900000  # 40 samples at 48 kHz
I64_MAX = (1 << 63) - 1


def units():
    """The access units (bytes, major sync)."""
    v, o = [], 0
    while o + 8 <= len(THD):
        n = (((THD[o] & 0xF) << 8) | THD[o + 1]) * 2
        v.append((THD[o:o + n], THD[o + 4:o + 8] == bytes([0xF8, 0x72, 0x6F, 0xBA])))
        o += n
    return v


def frame_array(datas):
    """A DRFrame[] over datas (with what keeps their bytes alive)."""
    keep = [ffi.new("uint8_t[]", d) for d in datas]
    arr = ffi.new("DRFrame[]", max(len(datas), 1))
    for i, (d, b) in enumerate(zip(datas, keep)):
        arr[i].data = b
        arr[i].size = len(d)
        arr[i].dur = AU
    return arr, keep


def decode(pre, rng):
    """ff_discrip_seamless_decode of units rng after pre: the samples, or None."""
    every = units()
    p, kp = frame_array([every[k][0] for k in pre])
    r, kr = frame_array([every[k][0] for k in rng])
    out = ffi.new("int32_t **")
    n = ffi.new("int *")
    ret = lib.ff_discrip_seamless_decode(ffi.NULL, codec_id("truehd"), 48000, p, len(pre), r, len(rng), out, n)
    assert ret >= 0
    if ret == 0:
        return None
    v = [out[0][i] for i in range(n[0])]
    lib.av_free(out[0])
    return v


def corr(a, b):
    n = min(len(a), len(b))
    return lib.ff_discrip_seamless_corr(ffi.new("int32_t[]", a[:n]), ffi.new("int32_t[]", b[:n]), n)


def pearson(a, b):
    """The Pearson correlation in floating point."""
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    ma, mb = sum(a) / n, sum(b) / n
    cov = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    va = sum((x - ma) ** 2 for x in a)
    vb = sum((y - mb) ** 2 for y in b)
    return cov / math.sqrt(va * vb)


def test_units_decode_to_one_channel_and_a_preroll_is_dropped():
    u = units()
    syncs = [k for k in range(len(u)) if u[k][1]]
    assert len(syncs) >= 3 and syncs[0] == 0
    whole = decode([], list(range(120)))
    assert len(whole) == 120 * 40
    assert any(abs(s) > 1 << 24 for s in whole), "16-bit samples are scaled to 32 bits"
    # from inside a group of units: the units since its major sync first
    m = syncs[1]
    assert decode(list(range(m, m + 3)), list(range(m + 3, m + 10))) == whole[(m + 3) * 40:(m + 10) * 40]
    # without them the decoder cannot start there
    assert decode([], list(range(m + 3, m + 10))) is None


def test_the_integer_correlation():
    whole = decode([], list(range(120)))
    a = whole[400:800]
    assert corr(a, a) == 0xFFFFFFFF, "identical"
    assert corr(a, [-x for x in a]) == 1, "opposite"
    quiet = [max(-99, min(99, x >> 26)) for x in a]
    assert corr(quiet, quiet) == 0, "silence"
    # shifted stretches of the sine: as the floating-point Pearson value
    for shift in [1, 7, 20, 40, 55]:
        b = whole[400 + shift:800 + shift]
        want = pearson(a, b)
        got = corr(a, b) / 4294967296.0
        if want > 0.0:
            assert abs(got - want) < 1e-6, f"shift {shift}: {got} against {want}"
        else:
            assert corr(a, b) == 1, f"shift {shift}: no positive correlation"


# ---- a join ----

class NoEnd:
    def max_time(self):
        return I64_MAX

    def advance(self, target):
        return 0, False


def join(codec):
    """Segment A = units [0, m + 2) (its last 10 read from the segment's tail;
    unit m there is a copy without its sync flag), segment B = units [m, 100)
    starting at unit m's time: the next segment repeats A's last two units."""
    u = units()
    m = next(k for k in range(1, len(u)) if u[k][1] and k >= 40)
    frames = []
    for k, (data, sync) in enumerate(u[:m + 2]):
        flags = lib.DR_F_KEY
        if sync and k < m:
            flags |= lib.DR_F_SYNC
        if k + 10 >= m + 2:
            flags |= lib.DR_F_TAIL
        frames.append(Frame(data, k * AU, AU, flags))
    for k in range(m, 100):
        data, sync = u[k]
        frames.append(Frame(data, k * AU, AU, lib.DR_F_KEY | (lib.DR_F_SYNC if sync else 0)))
    out, events = [], []

    def on_out(f):
        out.append((f.time, f.size))
        lib.ff_discrip_frame_unref(f)
        return 0
    keep = Handles()
    cfg = ffi.new("DRJunctionConfig *", {
        "track": 1, "frame_dur": AU, "tolerance": 0, "video": video_ref(NoEnd(), keep),
        "out": lib.tb_py_frame, "out_opaque": keep(on_out),
        "event": lib.tb_py_event, "event_opaque": keep(events_into(events)), "codec": codec, "rate": 48000})
    j = ffi.new("DRJunction **")
    st = ffi.new("DRJunctionStats *")
    assert lib.ff_discrip_junction_open(j, ffi.NULL, cfg) == 0
    for f in frames:
        assert lib.ff_discrip_junction_push(j[0], f.ptr) == 0
    assert lib.ff_discrip_junction_finish(j[0]) == 0
    lib.ff_discrip_junction_stats(j[0], st)
    lib.ff_discrip_junction_close(j)
    return out, events, st


def test_duplicated_units_at_a_join_are_found_and_dropped():
    u = units()
    m = next(k for k in range(1, len(u)) if u[k][1] and k >= 40)
    out, events, st = join(codec_id("truehd"))
    # A's two repeated units dropped: every unit once, back to back
    assert st.dropped == 2
    assert len(out) == 100
    assert all(o[0] == k * AU for k, o in enumerate(out))
    search = [e for e in events if e["kind"] == lib.DR_EV_SEAMLESS_SEARCH]
    assert len(search) == 1
    assert (search[0]["count"], search[0]["dur"], search[0]["skew"]) == (0xFFFFFFFF, 2, 1)
    drop = [e for e in events if e["kind"] == lib.DR_EV_SEAMLESS_DROP]
    assert len(drop) == 1
    assert (drop[0]["count"], drop[0]["dur"], drop[0]["skew"], drop[0]["pos"]) == (2, 2 * AU, 0, m * AU)


def test_without_decoding_the_join_takes_the_duration_rule():
    # the same join, not compared: one unit re-timed to end at the next
    # segment's major sync, the overlap then cut by the skew rule, which here
    # drops the same two units (as on the discs where only this rule ran)
    out, events, st = join(0)
    assert any(e["kind"] == lib.DR_EV_SEAMLESS_SEARCH and e["count"] == 0 and e["skew"] == 0 for e in events)
    assert not any(e["kind"] == lib.DR_EV_SEAMLESS_DROP for e in events)
    assert any(e["kind"] == lib.DR_EV_DROP and e["count"] == 2 for e in events), events
    assert st.dropped == 2
