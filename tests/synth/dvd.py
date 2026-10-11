"""DVD-Video discs built byte by byte (DVD-Video Book layout, field names
as libdvdread's ifo_types.h / nav_types.h): a VMG with a title search
table, title sets with program chains, cells, chapters and a VOBU address
map, and title VOBs whose blocks hold NAV packs. Written as a disc folder."""

import dataclasses
import shutil
import struct

S = 2048


def be16(d, at, v):
    d[at:at + 2] = struct.pack(">H", v)


def be32(d, at, v):
    d[at:at + 4] = struct.pack(">I", v)


def sectors_for(nbytes):
    return max((nbytes + S - 1) // S, 1)


def bcd(v):
    return (v // 10) << 4 | (v % 10)


def time(secs):
    """dvd_time_t for secs seconds at 25 frames per second (BCD, frame byte
    0x40 | frames)."""
    return bytes([bcd(secs // 3600), bcd(secs // 60 % 60), bcd(secs % 60), 0x40])


def bcd_secs(t):
    """Seconds of a dvd_time_t (frames dropped)."""
    def v(b):
        return (b >> 4) * 10 + (b & 0xf)
    return v(t[0]) * 3600 + v(t[1]) * 60 + v(t[2])


class flags:
    """cell_playback_t byte 0."""
    SEAMLESS_ANGLE = 0x01
    STC_DISCONTINUITY = 0x02
    INTERLEAVED = 0x04
    SEAMLESS_PLAY = 0x08
    ANGLE_BLOCK = 0x10      # block type 1: angle block
    FIRST_IN_BLOCK = 0x40   # block mode 1 / 2 / 3: first / middle / last cell of a block
    IN_BLOCK = 0x80
    LAST_IN_BLOCK = 0xc0


@dataclasses.dataclass
class Cell:
    """One cell (cell_playback_t): title-VOB sectors and the rest."""
    first_sector: int
    last_vobu_start_sector: int
    last_sector: int
    playback_time: bytes
    flags: int = 0
    still_time: int = 0
    cell_cmd_nr: int = 0
    first_ilvu_end_sector: int = 0
    vob_id_nr: int = 1              # cell_position_t (cell_nr 0: the cell's number in its chain)
    cell_nr: int = 0

    @classmethod
    def new(cls, first, last_vobu, last, secs):
        return cls(first, last_vobu, last, time(secs))

    def with_flags(self, f):
        return dataclasses.replace(self, flags=f)


@dataclasses.dataclass
class Pgc:
    """One program chain. programs: the first cell of each program (1-based;
    empty = one program at cell 1); playback_time None = the sum of the
    cells' times; entry_id: the search pointer's entry id (0x80 | title for
    a title's entry PGC); next_pgc_nr / prev_pgc_nr / goup_pgc_nr: the
    chain's links (0 = none)."""
    cells: list
    programs: list = dataclasses.field(default_factory=list)
    playback_time: bytes = None
    prohibited_ops: int = 0
    pg_playback_mode: int = 0
    pre: list = dataclasses.field(default_factory=list)
    post: list = dataclasses.field(default_factory=list)
    cell_cmds: list = dataclasses.field(default_factory=list)
    entry_id: int = 0
    audio_control: bytes = bytes(16)    # 8 x 16 bits as recorded
    subp_control: bytes = bytes(128)    # 32 x 32 bits as recorded
    next_pgc_nr: int = 0
    prev_pgc_nr: int = 0
    goup_pgc_nr: int = 0


@dataclasses.dataclass
class Vts:
    """A title set: its program chains, the parts of title of each of its
    titles as (pgcn, pgn), the VOBU address map and the size of its title
    VOBs."""
    pgcs: list = dataclasses.field(default_factory=list)
    ptts: list = dataclasses.field(default_factory=list)
    vobus: list = dataclasses.field(default_factory=list)
    vob_sectors: int = 0
    video_attr: bytes = bytes(2)        # VTSI_MAT vts_video_attr
    audio_attrs: list = dataclasses.field(default_factory=list)     # 8 bytes each
    subp_attrs: list = dataclasses.field(default_factory=list)      # 6 bytes each


def vts_ifo(v):
    """VTS_nn_0.IFO: VTSI_MAT in sector 0, then VTS_PTT_SRPT, VTS_PGCIT,
    VTS_C_ADT and VTS_VOBU_ADMAP each from a sector of its own."""
    # VTS_PTT_SRPT
    n_titles = len(v.ptts)
    entries = sum(len(t) for t in v.ptts)
    ptt = bytearray(8 + 4 * n_titles + 4 * entries)
    at = 8 + 4 * n_titles
    be16(ptt, 0, n_titles)
    for t, title in enumerate(v.ptts):
        be32(ptt, 8 + 4 * t, at)
        for pgcn, pgn in title:
            be16(ptt, at, pgcn)
            be16(ptt, at + 2, pgn)
            at += 4
    be32(ptt, 4, len(ptt) - 1)

    # VTS_PGCIT
    n = len(v.pgcs)
    pgcit = bytearray(8 + 8 * n)
    be16(pgcit, 0, n)
    for k, p in enumerate(v.pgcs):
        start = len(pgcit)
        pgcit[8 + 8 * k] = p.entry_id
        be32(pgcit, 8 + 8 * k + 4, start)
        pgcit += pgc_bytes(p)
    be32(pgcit, 4, len(pgcit) - 1)

    # VTS_C_ADT: one entry per cell id (VOB id, cell id as the program chains
    # write them), from the first cell that names it
    cells = {}
    for p in v.pgcs:
        for k, c in enumerate(p.cells):
            cells.setdefault((c.vob_id_nr, c.cell_nr or k + 1), (c.first_sector, c.last_sector))
    cadt = bytearray(8 + 12 * len(cells))
    be16(cadt, 0, max((vob for vob, _ in cells), default=0))
    be32(cadt, 4, len(cadt) - 1)
    for k, ((vob, ident), (first, last)) in enumerate(sorted(cells.items())):
        e = 8 + 12 * k
        be16(cadt, e, vob)
        cadt[e + 2] = ident
        be32(cadt, e + 4, first)
        be32(cadt, e + 8, last)

    # VTS_VOBU_ADMAP
    vmap = bytearray(4 + 4 * len(v.vobus))
    be32(vmap, 0, len(vmap) - 1)
    for k, s in enumerate(v.vobus):
        be32(vmap, 4 + 4 * k, s)

    d = bytearray(S)
    places = []
    for t in [ptt, pgcit, cadt, vmap]:
        places.append(len(d) // S)
        d += bytes(t) + bytes(sectors_for(len(t)) * S - len(t))
    ifo_sectors = len(d) // S
    d[:12] = b"DVDVIDEO-VTS"
    be32(d, 0x0c, 2 * ifo_sectors + v.vob_sectors - 1)  # vts_last_sector: IFO, title VOBs, BUP
    be32(d, 0x1c, ifo_sectors - 1)  # vtsi_last_sector
    be16(d, 0x20, 0x0010)  # specification version
    be32(d, 0x80, 0x3ff)  # vtsi_last_byte
    be32(d, 0xc4, ifo_sectors)  # vtstt_vobs
    be32(d, 0xc8, places[0])  # vts_ptt_srpt
    be32(d, 0xcc, places[1])  # vts_pgcit
    be32(d, 0xe0, places[2])  # vts_c_adt
    be32(d, 0xe4, places[3])  # vts_vobu_admap
    d[0x200:0x202] = v.video_attr
    be16(d, 0x202, len(v.audio_attrs))
    for k, a in enumerate(v.audio_attrs):
        d[0x204 + 8 * k:0x20c + 8 * k] = a
    be16(d, 0x254, len(v.subp_attrs))
    for k, a in enumerate(v.subp_attrs):
        d[0x256 + 6 * k:0x25c + 6 * k] = a
    return d


def pgc_bytes(p):
    """pgc_t with its command table, program map, cell playback and cell
    position tables."""
    n = len(p.cells)
    d = bytearray(0xec)
    d[2] = 0 if n == 0 else (1 if not p.programs else len(p.programs))
    d[3] = n
    total = sum(bcd_secs(c.playback_time) for c in p.cells)
    d[4:8] = p.playback_time if p.playback_time is not None else time(total)
    be32(d, 8, p.prohibited_ops)
    d[0x0c:0x1c] = p.audio_control
    d[0x1c:0x9c] = p.subp_control
    be16(d, 0x9c, p.next_pgc_nr)
    be16(d, 0x9e, p.prev_pgc_nr)
    be16(d, 0xa0, p.goup_pgc_nr)
    d[0xa2] = p.pg_playback_mode
    if p.pre or p.post or p.cell_cmds:
        be16(d, 0xe4, len(d))
        cmds = list(p.pre) + list(p.post) + list(p.cell_cmds)
        t = bytearray(8)
        be16(t, 0, len(p.pre))
        be16(t, 2, len(p.post))
        be16(t, 4, len(p.cell_cmds))
        be16(t, 6, 8 + 8 * len(cmds) - 1)
        for c in cmds:
            t += bytes(c)
        d += t
    if n > 0:
        be16(d, 0xe6, len(d))
        d += bytes([1]) if not p.programs else bytes(p.programs)
        if len(d) % 2 == 1:
            d.append(0)
        be16(d, 0xe8, len(d))
        for c in p.cells:
            b = bytearray(24)
            b[0] = c.flags
            b[2] = c.still_time
            b[3] = c.cell_cmd_nr
            b[4:8] = c.playback_time
            be32(b, 8, c.first_sector)
            be32(b, 12, c.first_ilvu_end_sector)
            be32(b, 16, c.last_vobu_start_sector)
            be32(b, 20, c.last_sector)
            d += b
        be16(d, 0xea, len(d))
        for k, c in enumerate(p.cells):
            d += bytes([c.vob_id_nr >> 8, c.vob_id_nr & 0xff, 0, c.cell_nr or k + 1])
    return d


@dataclasses.dataclass
class Title:
    """One entry of the title search table."""
    title_set_nr: int
    vts_ttn: int
    nr_of_ptts: int
    nr_of_angles: int = 1
    pb_ty: int = 0
    title_set_starting_sector: int = 0


def vmg_ifo(titles, nr_vts, first_play=None):
    """VIDEO_TS.IFO: VMGI_MAT in sector 0, TT_SRPT and VTS_ATRT after it;
    no menus. first_play: the first-play PGC (a Pgc, written after the
    VMGI_MAT in sector 0), or None for none."""
    tt = bytearray(8 + 12 * len(titles))
    be16(tt, 0, len(titles))
    be32(tt, 4, len(tt) - 1)
    for k, t in enumerate(titles):
        e = 8 + 12 * k
        tt[e] = t.pb_ty
        tt[e + 1] = t.nr_of_angles
        be16(tt, e + 2, t.nr_of_ptts)
        tt[e + 6] = t.title_set_nr
        tt[e + 7] = t.vts_ttn
        be32(tt, e + 8, t.title_set_starting_sector)
    n = nr_vts
    atrt = bytearray(8 + 4 * n + 542 * n)
    be16(atrt, 0, nr_vts)
    be32(atrt, 4, len(atrt) - 1)
    for k in range(n):
        off = 8 + 4 * n + 542 * k
        be32(atrt, 8 + 4 * k, off)
        be32(atrt, off, 541)  # last_byte of the entry
    d = bytearray(S)
    tt_at = len(d) // S
    d += bytes(tt) + bytes(sectors_for(len(tt)) * S - len(tt))
    atrt_at = len(d) // S
    d += bytes(atrt) + bytes(sectors_for(len(atrt)) * S - len(atrt))
    ifo_sectors = len(d) // S
    d[:12] = b"DVDVIDEO-VMG"
    be32(d, 0x0c, 2 * ifo_sectors - 1)  # vmg_last_sector: IFO + BUP
    be32(d, 0x1c, ifo_sectors - 1)  # vmgi_last_sector
    be16(d, 0x20, 0x0010)
    be16(d, 0x26, 1)  # vmg_nr_of_volumes
    be16(d, 0x28, 1)  # vmg_this_volume_nr
    d[0x2a] = 1  # disc_side
    be16(d, 0x3e, nr_vts)
    be32(d, 0x80, 0x3ff)  # vmgi_last_byte
    if first_play is not None:
        fp = pgc_bytes(first_play)
        assert len(fp) <= S - 0x400, "the first-play PGC must fit in sector 0"
        d[0x400:0x400 + len(fp)] = fp
        be32(d, 0x80, 0x400 + len(fp) - 1)  # vmgi_last_byte
        be32(d, 0x84, 0x400)  # first_play_pgc
    be32(d, 0xc4, tt_at)  # tt_srpt
    be32(d, 0xd0, atrt_at)  # vts_atrt
    return d


END_OF_CELL = 0x3fffffff
"""vobu_sri.next_vobu value for "the last VOBU of its cell"."""


@dataclasses.dataclass
class Nav:
    """A NAV pack (pack header, system header, PCI and DSI packets). lbn:
    nv_pck_lbn of the PCI and the DSI (pci_lbn: the PCI's, when it differs);
    vobu_ea: dsi_gi.vobu_ea; c_eltm: dsi_gi.c_eltm, the time elapsed in the
    cell as (hours, minutes, seconds, frames) at 25 frames/s; hli_ss:
    pci.hli.hl_gi.hli_ss (non-zero: the VOBU has buttons); category, ilvu_ea:
    sml_pbi; fwda: vobu_sri.fwda[0..15] (farthest first); next_vobu:
    vobu_sri.next_vobu (0x3fffffff = end of cell; bit 31 = a video VOBU)."""
    lbn: int = 0
    pci_lbn: int = None
    c_eltm: tuple = (0, 0, 0, 0)
    hli_ss: int = 0
    fwda: list = dataclasses.field(default_factory=list)
    vobu_s_ptm: int = 0
    vobu_e_ptm: int = 0
    vobu_ea: int = 0
    category: int = 0
    ilvu_ea: int = 0
    next_vobu: int = 0

    def block(self):
        s = bytearray(S)
        s[:5] = bytes([0, 0, 1, 0xba, 0x44])
        s[0x0e:0x14] = bytes([0, 0, 1, 0xbb, 0, 0x12])
        s[0x26:0x2d] = bytes([0, 0, 1, 0xbf, 0x03, 0xd4, 0])
        s[0x400:0x407] = bytes([0, 0, 1, 0xbf, 0x03, 0xfa, 1])
        be32(s, 0x2d, self.lbn if self.pci_lbn is None else self.pci_lbn)
        be32(s, 0x39, self.vobu_s_ptm)
        be32(s, 0x3d, self.vobu_e_ptm)
        be32(s, 0x40b, self.lbn)
        be32(s, 0x40f, self.vobu_ea)
        h, m, sec, f = self.c_eltm
        s[0x423:0x427] = bytes([bcd(h), bcd(m), bcd(sec), 0x40 | bcd(f)])  # 0x40: 25 frames/s
        # pci.hli.hl_gi.hli_ss: the low 2 bits of the PCI's 16-bit word 0x60
        # (PCI data from 0x2d: pci_gi 0x3c bytes, nsml_agli 0x24 bytes)
        s[0x2d + 0x61] = self.hli_ss & 3
        # dsi.vobu_sri.fwda[k]: DSI data from 0x407, vobu_sri at 0xea (after
        # dsi_gi, sml_pbi, sml_agli), fwda after next_video
        for k, fw in enumerate(self.fwda):
            be32(s, 0x407 + 0xea + 4 + 4 * k, fw)
        be16(s, 0x427, self.category)
        be32(s, 0x429, self.ilvu_ea)
        be32(s, 0x541, self.next_vobu)
        return s


def title_vobs(n, navs):
    """Title VOBs of n blocks: zeros, with navs at their blocks."""
    d = bytearray(n * S)
    for nv in navs:
        d[nv.lbn * S:(nv.lbn + 1) * S] = nv.block()
    return d


def copy_attributes(vmg, ifos):
    """VIDEO_TS.IFO with each title set's category and attributes (VTSI_MAT
    0x22, 0x100..0x315) copied into its VMG_VTS_ATRT entry, as on a disc."""
    vmg = bytearray(vmg)
    at = struct.unpack_from(">I", vmg, 0xd0)[0] * S
    if not at:
        return bytes(vmg)
    for k, ifo in enumerate(ifos):
        off = at + struct.unpack_from(">I", vmg, at + 8 + 4 * k)[0]
        vmg[off + 4:off + 8] = ifo[0x22:0x26]
        vmg[off + 8:off + 8 + 0x216] = ifo[0x100:0x316]
    return bytes(vmg)


def write_disc(folder, vmg, title_sets):
    """Writes a disc folder: VIDEO_TS/VIDEO_TS.IFO (+ .BUP) and per title set
    VTS_nn_0.IFO (+ .BUP) and VTS_nn_1.VOB. title_sets: [(ifo, vob)]; the
    title sets' attributes are copied into VIDEO_TS.IFO's VMG_VTS_ATRT."""
    v = folder / "VIDEO_TS"
    shutil.rmtree(folder, ignore_errors=True)
    v.mkdir(parents=True)
    vmg = copy_attributes(vmg, [bytes(ifo) for ifo, _ in title_sets])
    (v / "VIDEO_TS.IFO").write_bytes(vmg)
    (v / "VIDEO_TS.BUP").write_bytes(vmg)
    for k, (ifo, vob) in enumerate(title_sets):
        (v / f"VTS_{k + 1:02}_0.IFO").write_bytes(ifo)
        (v / f"VTS_{k + 1:02}_0.BUP").write_bytes(ifo)
        (v / f"VTS_{k + 1:02}_1.VOB").write_bytes(vob)


def build_disc(folder, titles, title_sets):
    """Writes a disc folder (VMG with titles, the title sets [(Vts, navs)]
    with their NAV packs)."""
    vmg = vmg_ifo(titles, len(title_sets))
    write_disc(folder, vmg, [(vts_ifo(v), title_vobs(v.vob_sectors, navs)) for v, navs in title_sets])


def nav(lbn, length, nxt):
    """A NAV pack at lbn of a VOBU of length blocks followed by the next one
    (nxt = None: the last VOBU of its cell)."""
    return Nav(lbn=lbn, vobu_ea=length - 1, next_vobu=END_OF_CELL if nxt is None else nxt - lbn)


def cell_navs(starts, end, t0):
    """NAV packs for the VOBUs starts (sorted) of a cell ending before block
    end, 0.5 s (45000 ticks) apart, times from t0."""
    out = []
    for k, s in enumerate(starts):
        nxt = starts[k + 1] if k + 1 < len(starts) else None
        length = (nxt if nxt is not None else end) - s
        t = t0 + 45000 * k
        n = nav(s, length, nxt)
        n.vobu_s_ptm = t
        n.vobu_e_ptm = t + 45000
        out.append(n)
    return out


# ---- title VOB sectors with real streams ----

def _pts(v):
    """A 5-byte PTS field (PTS_DTS_flags '10')."""
    return bytes([0x21 | (v >> 29 & 0x0e), v >> 22 & 0xff, (v >> 14 & 0xfe) | 1, v >> 7 & 0xff, (v << 1 & 0xfe) | 1])


def _sector(stream_id, head, payload):
    """One 2048-byte pack: pack header, a PES packet (header data head, then
    payload), a padding packet filling the rest."""
    pes = bytes([0x81, 0x80 if head else 0x00, len(head)]) + head + payload
    s = bytearray(bytes([0, 0, 1, 0xba, 0x44, 0, 4, 0, 4, 1, 1, 0x89, 0xc3, 0xf8]))
    s += bytes([0, 0, 1, stream_id]) + len(pes).to_bytes(2, "big") + pes
    room = S - len(s)
    assert room == 0 or room >= 6
    if room:
        s += bytes([0, 0, 1, 0xbe]) + (room - 6).to_bytes(2, "big") + bytes([0xff]) * (room - 6)
    return bytes(s)


def mux_vobus(m2v, pics, lbn, audio=(), dts0=0):
    """Title VOB sectors for the pictures pics (helpers.disctl.pictures
    entries of m2v, from a GOP start, each with its PTS: (payload start, end,
    display index, type, pts)) and audio [(AC-3 frame, pts)]: one VOBU per
    GOP (a NAV pack, then its packs), each picture from a pack of its own
    whose PES carries its PTS, the audio frames (private stream 1 sub-stream
    0x80, one per pack) before the picture whose decode time (dts0 + 3003 per
    picture) they do not pass. Returns (sectors, navs), lbn the first
    sector's number in the title VOBs."""
    sectors, navs, gop_pics, ai = [], [], [], 0
    audio = list(audio)
    for k, (a, b, disp, typ, pts) in enumerate(pics):
        data = m2v[a:b]
        if b"\x00\x00\x01\xb8" in data[:64] or not navs:
            navs.append(Nav(lbn=lbn + len(sectors)))
            gop_pics.append([])
            sectors.append(None)
        gop_pics[-1].append(pts)
        while ai < len(audio) and audio[ai][1] <= dts0 + k * 3003:
            frame, apts = audio[ai]
            sectors.append(_sector(0xbd, _pts(apts), bytes([0x80, 1, 0, 1]) + frame))
            ai += 1
        first = True
        while data:
            cap = S - 14 - 6 - 3 - (5 if first else 0) - 6
            chunk, data = data[:cap], data[cap:]
            sectors.append(_sector(0xe0, _pts(pts) if first else b"", chunk))
            first = False
    end = lbn + len(sectors)
    for g, nv in enumerate(navs):
        nxt = navs[g + 1].lbn if g + 1 < len(navs) else None
        nv.vobu_ea = (nxt if nxt is not None else end) - nv.lbn - 1
        nv.next_vobu = END_OF_CELL if nxt is None else nxt - nv.lbn
        nv.vobu_s_ptm = min(gop_pics[g])
        nv.vobu_e_ptm = max(gop_pics[g]) + 3003
        sectors[nv.lbn - lbn] = nv.block()
    return sectors, navs
