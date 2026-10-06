"""The DVD-Video demuxer's parts: built discs opened as the demuxer opens
them, the navigation scan and the title plan as Python values."""

import dataclasses

from helpers import ffi, lib


@dataclasses.dataclass
class ScanResult:
    """One result of the navigation scan: the cells a title's program chain
    played (name: the start point it was found from; title / pgcn: low
    bytes; cells in the order played, repeats removed)."""
    name: str
    title: int
    pgcn: int
    cells: list


@dataclasses.dataclass
class Scan:
    """What a navigation scan found; failure: why it failed (it then has no
    results); entered: title sets (0 = the video manager) whose cells the
    navigation played."""
    results: list = dataclasses.field(default_factory=list)
    failure: str = None
    entered: list = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class Chapter:
    pgcn: int
    pgn: int
    cell: int
    segment: int
    offset: int
    time: int


@dataclasses.dataclass
class Segment:
    extents: list   # [(logical, sector, count)]
    size: int
    label: str


@dataclasses.dataclass
class Title:
    name: str
    vtsn: int
    pgcn: int
    angle: int
    angles: int
    chapters: list
    scan: str        # its navigation-scan result's start name, or None
    cells: list
    segments: list
    declared_secs: int
    measured_secs: int
    not_selected: int


@dataclasses.dataclass
class Plan:
    titles: list = dataclasses.field(default_factory=list)
    events: list = dataclasses.field(default_factory=list)  # (finding, its arguments, tab-separated)
    scan: Scan = None


def text(arr):
    return ffi.string(arr).decode("utf-8", "replace")


def scan_from_c(s):
    results = []
    for i in range(s.nb_results):
        r = s.results[i]
        results.append(ScanResult(text(r.name), r.title & 0xff, r.pgcn & 0xff,
                                  [r.cells[k] for k in range(r.nb_cells)]))
    return Scan(results, text(s.failure) if s.failed else None, [i for i in range(100) if s.entered[i]])


def plan_from_c(p, scan):
    titles = []
    for i in range(p.nb_titles):
        t = p.titles[i]
        chapters = [Chapter(c.pgcn, c.pgn, c.cell, c.segment, c.offset, c.time)
                    for c in (t.chapters[k] for k in range(t.nb_chapters))]
        segments = []
        for k in range(t.nb_segments):
            g = t.segments[k]
            segments.append(Segment([(e.logical, e.sector, e.count) for e in (g.extents[j] for j in range(g.nb_extents))],
                                    g.size, text(g.label)))
        name = scan.results[t.scan].name if scan is not None and 0 <= t.scan < len(scan.results) else None
        titles.append(Title(text(t.name), t.vtsn, t.pgcn, t.angle, t.angles, chapters, name,
                            [t.cells[k] for k in range(t.nb_cells)], segments, t.declared_secs,
                            t.measured_secs, t.not_selected))
    events = [(ffi.string(lib.ff_dvdvideo_event_name(e.kind)).decode(), text(e.args))
              for e in (p.events[k] for k in range(p.nb_events))]
    return Plan(titles, events, scan)


def image_options(udf_reader=0, prefer_iso_for_old_udf102=1):
    return ffi.new("DiscIOImageOptions *", [udf_reader, prefer_iso_for_old_udf102])


def titles(path, opts=None, attempts=5, cell_mode=0, title_order=0, min_length=0):
    """Opens the disc at path (a disc folder or image), scans its navigation
    (unless the cell mode is trim) and builds its title plan: a Plan, or
    the negative AVERROR code."""
    src = ffi.new("DVDVideoSource **")
    ret = lib.ff_dvdvideo_source_open(ffi.NULL, str(path).encode(), opts or image_options(), attempts, src)
    if ret < 0:
        return ret
    disc = ffi.new("DVDVideoDisc **")
    scan_c = ffi.new("DVDVideoScan **")
    plan = ffi.new("DVDVideoTitlePlan **")
    topt = ffi.new("DVDVideoTitleOptions *", [cell_mode, title_order, min_length])
    try:
        ret = lib.ff_dvdvideo_disc_open(ffi.NULL, src[0], disc)
        if ret >= 0 and cell_mode != lib.DVDVIDEO_CELLS_TRIM:
            ret = lib.ff_dvdvideo_scan(ffi.NULL, disc[0], ffi.NULL, ffi.NULL, scan_c)
        if ret >= 0:
            ret = lib.ff_dvdvideo_titles_plan(ffi.NULL, disc[0], scan_c[0], topt, plan)
        if ret < 0:
            return ret
        scan = scan_from_c(scan_c[0]) if scan_c[0] != ffi.NULL else None
        return plan_from_c(plan[0], scan)
    finally:
        lib.ff_dvdvideo_titles_free(plan)
        lib.ff_dvdvideo_scan_free(scan_c)
        lib.ff_dvdvideo_disc_close(disc)
        lib.ff_dvdvideo_source_close(src)


@ffi.def_extern()
def tb_py_scan_trace(opaque, line):
    ffi.from_handle(opaque)(ffi.string(line).decode("utf-8", "replace"))


def scan(path, opts=None, attempts=5, trace=None):
    """Opens the disc at path and scans its navigation; trace(line) receives
    the scan's trace lines. A Scan, or the negative AVERROR code."""
    src = ffi.new("DVDVideoSource **")
    ret = lib.ff_dvdvideo_source_open(ffi.NULL, str(path).encode(), opts or image_options(), attempts, src)
    if ret < 0:
        return ret
    disc = ffi.new("DVDVideoDisc **")
    out = ffi.new("DVDVideoScan **")
    handle = ffi.new_handle(trace) if trace else ffi.NULL
    try:
        ret = lib.ff_dvdvideo_disc_open(ffi.NULL, src[0], disc)
        if ret >= 0:
            ret = lib.ff_dvdvideo_scan(ffi.NULL, disc[0], lib.tb_py_scan_trace if trace else ffi.NULL, handle, out)
        return ret if ret < 0 else scan_from_c(out[0])
    finally:
        lib.ff_dvdvideo_scan_free(out)
        lib.ff_dvdvideo_disc_close(disc)
        lib.ff_dvdvideo_source_close(src)


class OpenDisc:
    """A built disc folder opened as the DVD-Video demuxer opens it
    (DVDVideoDisc); closed with close() or when collected."""

    def __init__(self, folder):
        self.src = ffi.new("DVDVideoSource **")
        self.disc = ffi.new("DVDVideoDisc **")
        assert lib.ff_dvdvideo_source_open(ffi.NULL, str(folder).encode(), image_options(), 1, self.src) >= 0
        assert lib.ff_dvdvideo_disc_open(ffi.NULL, self.src[0], self.disc) >= 0, "the built disc opens"

    @classmethod
    def build(cls, folder, titles_, title_sets):
        """Writes a disc folder (VMG with titles_, the title sets [(Vts,
        navs)] with their NAV packs) and opens it."""
        from synth.dvd import build_disc
        build_disc(folder, titles_, title_sets)
        return cls(folder)

    @property
    def d(self):
        return self.disc[0]

    def close(self):
        if self.disc is not None:
            lib.ff_dvdvideo_disc_close(self.disc)
            lib.ff_dvdvideo_source_close(self.src)
            self.disc = None

    def __del__(self):
        self.close()
