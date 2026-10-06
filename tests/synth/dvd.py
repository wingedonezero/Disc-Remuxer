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


def time(secs):
    """dvd_time_t for secs seconds at 25 frames per second (BCD, frame byte
    0x40 | frames)."""
    def bcd(v):
        return (v // 10) << 4 | (v % 10)
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
    a title's entry PGC)."""
    cells: list
    programs: list = dataclasses.field(default_factory=list)
    playback_time: bytes = None
    prohibited_ops: int = 0
    pg_playback_mode: int = 0
    pre: list = dataclasses.field(default_factory=list)
    post: list = dataclasses.field(default_factory=list)
    cell_cmds: list = dataclasses.field(default_factory=list)
    entry_id: int = 0


@dataclasses.dataclass
class Vts:
    """A title set: its program chains, the parts of title of each of its
    titles as (pgcn, pgn), the VOBU address map and the size of its title
    VOBs."""
    pgcs: list = dataclasses.field(default_factory=list)
    ptts: list = dataclasses.field(default_factory=list)
    vobus: list = dataclasses.field(default_factory=list)
    vob_sectors: int = 0


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

    # VTS_C_ADT: one entry per cell of the first PGC that names it (VOB id 1)
    cells = []
    for p in v.pgcs:
        for k, c in enumerate(p.cells):
            ident = k + 1
            if not any(i == ident and f == c.first_sector for i, f, _ in cells):
                cells.append((ident, c.first_sector, c.last_sector))
    cadt = bytearray(8 + 12 * len(cells))
    be16(cadt, 0, 1)
    be32(cadt, 4, len(cadt) - 1)
    for k, (ident, first, last) in enumerate(cells):
        e = 8 + 12 * k
        be16(cadt, e, 1)
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
        for k in range(n):
            d += bytes([0, 1, 0, k + 1])
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


def vmg_ifo(titles, nr_vts):
    """VIDEO_TS.IFO: VMGI_MAT in sector 0, TT_SRPT and VTS_ATRT after it;
    no first-play PGC, no menus."""
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
    be32(d, 0xc4, tt_at)  # tt_srpt
    be32(d, 0xd0, atrt_at)  # vts_atrt
    return d


END_OF_CELL = 0x3fffffff
"""vobu_sri.next_vobu value for "the last VOBU of its cell"."""


@dataclasses.dataclass
class Nav:
    """A NAV pack (pack header, system header, PCI and DSI packets). lbn:
    nv_pck_lbn of the PCI and the DSI; vobu_ea: dsi_gi.vobu_ea; category,
    ilvu_ea: sml_pbi; next_vobu: vobu_sri.next_vobu (0x3fffffff = end of
    cell; bit 31 = a video VOBU)."""
    lbn: int = 0
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
        be32(s, 0x2d, self.lbn)
        be32(s, 0x39, self.vobu_s_ptm)
        be32(s, 0x3d, self.vobu_e_ptm)
        be32(s, 0x40b, self.lbn)
        be32(s, 0x40f, self.vobu_ea)
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


def write_disc(folder, vmg, title_sets):
    """Writes a disc folder: VIDEO_TS/VIDEO_TS.IFO (+ .BUP) and per title set
    VTS_nn_0.IFO (+ .BUP) and VTS_nn_1.VOB. title_sets: [(ifo, vob)]."""
    v = folder / "VIDEO_TS"
    shutil.rmtree(folder, ignore_errors=True)
    v.mkdir(parents=True)
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
