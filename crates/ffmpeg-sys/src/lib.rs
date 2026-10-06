//! Our FFmpeg (`libs/ffmpeg`), built by `build.rs` with the DVD-Video demuxer
//! on our libdvdread / libdvdnav / libdvdcss and linked statically.
//!
//! Declarations are added as the workspace uses them. `glue/glue.c` holds the
//! C side of calls that are awkward from Rust (log callback, option sets).

use std::os::raw::{c_char, c_int, c_uint, c_void};

// Keeps the DVD libraries, expat and libaacs linked after FFmpeg.
use libaacs_sys as _;
use libdvdnav_sys as _;
use libexpat_sys as _;

/// FFmpeg log levels (`libavutil/log.h`).
pub mod log_level {
    use std::os::raw::c_int;
    pub const QUIET: c_int = -8;
    pub const PANIC: c_int = 0;
    pub const FATAL: c_int = 8;
    pub const ERROR: c_int = 16;
    pub const WARNING: c_int = 24;
    pub const INFO: c_int = 32;
    pub const VERBOSE: c_int = 40;
    pub const DEBUG: c_int = 48;
    pub const TRACE: c_int = 56;
}

/// Receives one frame from [`dr_parser_run`]: its size and the timestamps the
/// parser gave it (`AV_NOPTS_VALUE` = none).
pub type ParserFrameCb = unsafe extern "C" fn(opaque: *mut c_void, size: c_int, pts: i64, dts: i64);

/// FFmpeg's "no timestamp" value (`AV_NOPTS_VALUE`).
pub const AV_NOPTS_VALUE: i64 = i64::MIN;

/// Receives one complete FFmpeg log line (without its newline).
pub type LogSink = unsafe extern "C" fn(level: c_int, line: *const c_char);

extern "C" {
    /// FFmpeg's version string (e.g. "9.0.2").
    pub fn av_version_info() -> *const c_char;
    pub fn avutil_version() -> c_uint;
    pub fn avcodec_version() -> c_uint;
    pub fn avformat_version() -> c_uint;
    /// Describes an AVERROR code in `errbuf`; returns < 0 when it is unknown.
    pub fn av_strerror(errnum: c_int, errbuf: *mut c_char, errbuf_size: usize) -> c_int;
    /// Frees memory FFmpeg allocated (`av_malloc` and friends).
    pub fn av_free(ptr: *mut c_void);

    /// glue.c: routes FFmpeg's log (and through it libdvdread / libdvdnav's)
    /// to `sink`, for lines up to `max_level`.
    pub fn dr_log_install(sink: LogSink, max_level: c_int);
    /// glue.c: runs FFmpeg's parser for `codec_name` (codec descriptor name)
    /// over `nb` packets as libavformat does (timestamp on each packet's first
    /// call only, flush at the end); `cb` gets every frame. 0 or AVERROR.
    pub fn dr_parser_run(
        codec_name: *const c_char,
        data: *const *const u8,
        sizes: *const c_int,
        pts: *const i64,
        nb: c_int,
        cb: ParserFrameCb,
        opaque: *mut c_void,
    ) -> c_int;
    /// glue.c: opens the DVD-Video demuxer with `options` (its `AVOptions` as
    /// `key=value` pairs joined by `:`, e.g. `title=1:read_attempts=5`; may
    /// be NULL), reads stream information and logs FFmpeg's stream dump. 0 or
    /// a negative AVERROR.
    pub fn dr_probe_dvdvideo(path: *const c_char, options: *const c_char) -> c_int;
    /// Opens a title with demuxer `format`, writes every stream to its own file
    /// in `outdir` (`<prefix><index>_<lang>.<ext>`) and the chapters as
    /// Matroska XML (`<prefix>chapters.xml`); `cb` gets each stream's
    /// statistics. `nb_titles` gets the demuxer's title count (or -1).
    pub fn dr_demux(
        format: *const c_char,
        path: *const c_char,
        options: *const c_char,
        key_files: *const c_char,
        outdir: *const c_char,
        prefix: *const c_char,
        nb_titles: *mut c_int,
        cb: DemuxStreamCb,
        opaque: *mut c_void,
    ) -> c_int;
}

/// `DrStreamStats` of the C glue: one stream's statistics after demuxing
/// (timestamps in 90 kHz for the disc demuxers; `AV_NOPTS_VALUE` = none).
#[repr(C)]
pub struct DemuxStreamStats {
    pub index: c_int,
    pub kind: *const c_char,
    pub codec: *const c_char,
    pub lang: *const c_char,
    pub file: *const c_char,
    pub packets: i64,
    pub bytes: i64,
    pub no_ts: i64,
    pub first_ts: i64,
    pub end_ts: i64,
    pub overlaps: i64,
    pub max_overlap: i64,
    pub gaps: i64,
    pub max_gap: i64,
}

/// Receives one stream's statistics from [`dr_demux`].
pub type DemuxStreamCb = unsafe extern "C" fn(opaque: *mut c_void, stats: *const DemuxStreamStats);

/// Splits a packed library version (`AV_VERSION_INT`) into major.minor.micro.
#[must_use]
pub fn version_triplet(v: c_uint) -> (u32, u32, u32) {
    (v >> 16, (v >> 8) & 0xff, v & 0xff)
}

/// Text of an AVERROR code.
#[must_use]
pub fn error_text(code: c_int) -> String {
    let mut buf = [0 as c_char; 256];
    // SAFETY: buf is writable for its length; av_strerror NUL-terminates.
    unsafe {
        av_strerror(code, buf.as_mut_ptr(), buf.len());
        std::ffi::CStr::from_ptr(buf.as_ptr()).to_string_lossy().into_owned()
    }
}

/// Disc I/O of our FFmpeg (`libavformat/discio.h`): sources and block reads.
pub mod discio {
    use std::os::raw::{c_char, c_int, c_void};

    pub const BLOCK_SIZE: usize = 2048;

    /// `DiscIOSourceOps`.
    #[repr(C)]
    pub struct SourceOps {
        pub read_at: unsafe extern "C" fn(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int,
        pub close: Option<unsafe extern "C" fn(opaque: *mut c_void)>,
    }

    /// `DiscIOSource` (only handled through pointers here).
    #[repr(C)]
    pub struct Source {
        _private: [u8; 0],
    }

    /// `DiscIOExtent`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct Extent {
        pub sector: i64,
        pub count: i64,
    }

    /// `DiscIOFile`.
    #[repr(C)]
    pub struct File {
        pub size: i64,
        pub nb_extents: c_int,
        pub extents: *mut Extent,
        pub data: *mut u8,
    }

    /// `DiscIODirCallback`.
    pub type DirCallback = unsafe extern "C" fn(opaque: *mut c_void, name: *const c_char, is_dir: c_int) -> c_int;

    /// `DiscIOFSOps`.
    #[repr(C)]
    pub struct FsOps {
        pub name: *const c_char,
        pub open_file: unsafe extern "C" fn(fs: *mut Fs, path: *const c_char, out: *mut *mut File) -> c_int,
        pub list_dir: unsafe extern "C" fn(fs: *mut Fs, path: *const c_char, cb: DirCallback, opaque: *mut c_void) -> c_int,
        pub close: Option<unsafe extern "C" fn(fs: *mut Fs)>,
        pub find_dir: unsafe extern "C" fn(fs: *mut Fs, path: *const c_char) -> c_int,
        /// [`FS_ISO9660`], [`FS_JOLIET`] or [`FS_UDF`].
        pub kind: c_int,
    }

    /// `enum DiscIOFSKind`.
    pub const FS_ISO9660: c_int = 1;
    pub const FS_JOLIET: c_int = 2;
    pub const FS_UDF: c_int = 3;

    /// `enum DiscIOUDFReader`.
    pub const UDF_NETBSD: c_int = 0;
    pub const UDF_LINUX: c_int = 1;

    /// `DiscIOImageOptions`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct ImageOptions {
        pub udf_reader: c_int,
        pub prefer_iso_for_old_udf102: c_int,
    }

    impl Default for ImageOptions {
        /// `DISCIO_IMAGE_OPTIONS_DEFAULT`.
        fn default() -> Self {
            ImageOptions { udf_reader: UDF_NETBSD, prefer_iso_for_old_udf102: 1 }
        }
    }

    /// `enum DiscIODiscFormat`.
    pub const DISC_NONE: c_int = 0;
    pub const DISC_DVD: c_int = 1;
    pub const DISC_BLURAY: c_int = 2;
    pub const DISC_HDDVD: c_int = 3;

    /// `DISCIO_LABEL_SIZE`.
    pub const LABEL_SIZE: usize = 260;

    /// `DISCIO_SECTOR_NOT_RECORDED`.
    pub const SECTOR_NOT_RECORDED: i64 = -1;

    /// `DiscIOFS`.
    #[repr(C)]
    pub struct Fs {
        pub ops: *const FsOps,
        pub priv_: *mut c_void,
        pub src: *mut Source,
        pub label: [c_char; LABEL_SIZE],
        pub udf_revision: u16,
        pub udf_recording_time: [u8; 12],
    }

    extern "C" {
        pub fn ff_discio_fs_close(fs: *mut *mut Fs);
        pub fn ff_discio_file_free(file: *mut *mut File);
        pub fn ff_discio_file_read(fs: *mut Fs, file: *const File, pos: i64, buf: *mut u8, len: c_int) -> c_int;
        pub fn ff_discio_iso9660_mount(src: *mut Source, joliet: c_int, out: *mut *mut Fs) -> c_int;
        pub fn ff_discio_iso9660_creation_date(fs: *const Fs, date: *mut c_char) -> c_int;
        pub fn ff_discio_udf_netbsd_mount(src: *mut Source, out: *mut *mut Fs) -> c_int;
        pub fn ff_discio_udf_linux_mount(src: *mut Source, out: *mut *mut Fs) -> c_int;
        pub fn ff_discio_mount_image(src: *mut Source, opts: *const ImageOptions, out: *mut *mut Fs) -> c_int;
        pub fn ff_discio_dvd_video_check(fs: *mut Fs) -> c_int;
        pub fn ff_discio_label_copy(dst: *mut c_char, src: *const c_char);
        pub fn ff_discio_disc_format(fs: *mut Fs) -> c_int;
    }

    extern "C" {
        pub fn ff_discio_source_new(
            logctx: *mut c_void,
            name: *const c_char,
            size: i64,
            ops: *const SourceOps,
            opaque: *mut c_void,
        ) -> *mut Source;
        pub fn ff_discio_source_open_file(logctx: *mut c_void, path: *const c_char, out: *mut *mut Source) -> c_int;
        pub fn ff_discio_source_free(src: *mut *mut Source);
        pub fn ff_discio_read_blocks(src: *mut Source, pos: i64, buf: *mut u8, len: c_int, attempts: c_int, quiet: c_int) -> c_int;
        pub fn ff_discio_read_bytes(src: *mut Source, pos: i64, buf: *mut u8, len: c_int, attempts: c_int, quiet: c_int) -> c_int;
    }
}

/// The DVD-Video demuxer's disc source and navigation scan
/// (`libavformat/dvdvideo_internal.h`).
pub mod dvdvideo {
    use std::ffi::{CStr, CString};
    use std::os::raw::{c_char, c_int, c_void};
    use std::path::Path;

    use crate::discio::ImageOptions;

    /// `DVDVideoSource` (only handled through pointers).
    #[repr(C)]
    pub struct Source {
        _private: [u8; 0],
    }

    /// `DVDVideoDisc` (only handled through pointers).
    #[repr(C)]
    pub struct Disc {
        _private: [u8; 0],
    }

    /// `DVDVideoRand`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy, Default)]
    pub struct Rand {
        pub state: [u32; 31],
        pub front: c_int,
    }

    /// `DVDVideoScanResult`.
    #[repr(C)]
    pub struct ScanResultC {
        pub name: [c_char; 16],
        pub title: c_int,
        pub pgcn: c_int,
        pub cells: *mut u8,
        pub nb_cells: c_int,
    }

    /// `DVDVideoScan`.
    #[repr(C)]
    pub struct ScanC {
        pub results: *mut ScanResultC,
        pub nb_results: c_int,
        pub failed: c_int,
        pub failure: [c_char; 512],
        pub entered: [u8; 100],
    }

    /// `DVDVideoScanTrace`.
    pub type TraceCb = unsafe extern "C" fn(opaque: *mut c_void, line: *const c_char);

    extern "C" {
        pub fn ff_dvdvideo_source_open(
            log: *mut c_void,
            path: *const c_char,
            opts: *const ImageOptions,
            attempts: c_int,
            out: *mut *mut Source,
        ) -> c_int;
        pub fn ff_dvdvideo_source_close(src: *mut *mut Source);
        pub fn ff_dvdvideo_rand_init(r: *mut Rand, seed: u32);
        pub fn ff_dvdvideo_rand_next(r: *mut c_void) -> c_int;
        pub fn ff_dvdvideo_disc_open(log: *mut c_void, src: *mut Source, out: *mut *mut Disc) -> c_int;
        pub fn ff_dvdvideo_disc_close(disc: *mut *mut Disc);
        pub fn ff_dvdvideo_scan(
            log: *mut c_void,
            disc: *mut Disc,
            trace: Option<TraceCb>,
            trace_opaque: *mut c_void,
            out: *mut *mut ScanC,
        ) -> c_int;
        pub fn ff_dvdvideo_scan_free(scan: *mut *mut ScanC);
    }

    /// One result of the navigation scan: the cells a title's program chain
    /// played.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct ScanResult {
        /// The start point it was found from.
        pub name: String,
        /// Title of the title search table (low byte).
        pub title: u8,
        /// Program chain of the title set (low byte).
        pub pgcn: u8,
        /// Cell numbers in the order played, repeats removed.
        pub cells: Vec<u8>,
    }

    /// What a navigation scan found.
    #[derive(Debug, Clone, Default, PartialEq, Eq)]
    pub struct Scan {
        pub results: Vec<ScanResult>,
        /// Why the scan failed (it then has no results).
        pub failure: Option<String>,
        /// Title sets (0 = the video manager) whose cells the navigation played.
        pub entered: Vec<u8>,
    }

    /// `DVDVideoChapter`.
    #[repr(C)]
    pub struct ChapterC {
        pub pgcn: c_int,
        pub pgn: c_int,
        pub cell: c_int,
        pub segment: u32,
        pub offset: u64,
        pub time: u64,
    }

    /// `DVDVideoExtent`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct Extent {
        pub logical: u32,
        pub sector: u32,
        pub count: u32,
    }

    /// `DVDVideoSegment`.
    #[repr(C)]
    pub struct SegmentC {
        pub extents: *mut Extent,
        pub nb_extents: c_int,
        pub last_cell: c_int,
        pub runs: *mut [c_int; 2],
        pub nb_runs: c_int,
        pub size: u64,
        pub label: [c_char; 256],
    }

    /// `DVDVideoTitle`.
    #[repr(C)]
    pub struct TitleC {
        pub index: c_int,
        pub name: [c_char; 16],
        pub title_type: c_int,
        pub vtsn: c_int,
        pub vts_ttn: c_int,
        pub angle: c_int,
        pub angles: c_int,
        pub pgcn: c_int,
        pub chapters: *mut ChapterC,
        pub nb_chapters: c_int,
        pub scan: c_int,
        pub audio: [u8; 8],
        pub nb_audio: c_int,
        pub audio_mask: c_int,
        pub subp: [u8; 64],
        pub nb_subp: c_int,
        pub cells: *mut c_int,
        pub nb_cells: c_int,
        pub segments: *mut SegmentC,
        pub nb_segments: c_int,
        pub segments_built: c_int,
        pub declared_secs: u32,
        pub measured_secs: u32,
        pub nav_invalid_count: u32,
        pub not_selected: c_int,
    }

    /// `DVDVideoTitleEvent`.
    #[repr(C)]
    pub struct EventC {
        pub kind: c_int,
        pub args: [c_char; 128],
    }

    /// `DVDVideoTitlePlan`.
    #[repr(C)]
    pub struct PlanC {
        pub titles: *mut TitleC,
        pub nb_titles: c_int,
        pub events: *mut EventC,
        pub nb_events: c_int,
    }

    /// `enum DVDVideoCellMode`.
    pub const CELLS_AUTO: c_int = 0;
    pub const CELLS_WALK: c_int = 1;
    pub const CELLS_TRIM: c_int = 2;
    pub const CELLS_WALK_TRIM: c_int = 3;
    /// `enum DVDVideoTitleOrder`.
    pub const ORDER_AUTO: c_int = 0;
    pub const ORDER_SCAN_FIRST: c_int = 1;
    pub const ORDER_TABLE: c_int = 2;
    /// `enum DVDVideoNotSelected`.
    pub const TITLE_SHORT: c_int = 1;
    pub const TITLE_FAKE: c_int = 2;

    /// `DVDVideoTitleOptions`.
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct TitleOptions {
        pub cell_mode: c_int,
        pub title_order: c_int,
        pub min_length: c_int,
    }

    extern "C" {
        pub fn ff_dvdvideo_titles_plan(
            log: *mut c_void,
            disc: *mut Disc,
            scan: *const ScanC,
            opt: *const TitleOptions,
            out: *mut *mut PlanC,
        ) -> c_int;
        pub fn ff_dvdvideo_titles_free(plan: *mut *mut PlanC);
        pub fn ff_dvdvideo_event_name(kind: c_int) -> *const c_char;
    }

    /// One chapter of a title.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Chapter {
        pub pgcn: i32,
        pub pgn: i32,
        /// Its first cell (index in the program chain).
        pub cell: i32,
        pub segment: u32,
        pub offset: u64,
        /// From the title start, in 1/1,080,000,000 s.
        pub time: u64,
    }

    /// One segment of a title.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Segment {
        pub extents: Vec<Extent>,
        pub size: u64,
        pub label: String,
    }

    /// One title of the title plan.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Title {
        pub name: String,
        pub vtsn: i32,
        pub pgcn: i32,
        pub angle: i32,
        pub angles: i32,
        pub chapters: Vec<Chapter>,
        /// Its navigation-scan result's start name.
        pub scan: Option<String>,
        pub cells: Vec<i32>,
        pub segments: Vec<Segment>,
        pub declared_secs: u32,
        pub measured_secs: u32,
        /// 0 selectable, [`TITLE_SHORT`], [`TITLE_FAKE`].
        pub not_selected: i32,
    }

    /// The title plan of a disc: its titles and the findings made building it.
    #[derive(Debug, Clone, Default, PartialEq, Eq)]
    pub struct Plan {
        pub titles: Vec<Title>,
        /// (finding, its arguments, tab-separated).
        pub events: Vec<(String, String)>,
        pub scan: Option<Scan>,
    }

    fn c_text(t: &[c_char]) -> String {
        // SAFETY: a NUL-terminated array.
        unsafe { CStr::from_ptr(t.as_ptr()) }.to_string_lossy().into_owned()
    }

    unsafe fn slice<'a, T>(p: *const T, n: c_int) -> &'a [T] {
        let n = usize::try_from(n).unwrap_or(0);
        // SAFETY: the caller passes n valid elements (none when n is 0).
        if n == 0 { &[] } else { unsafe { std::slice::from_raw_parts(p, n) } }
    }

    /// A title plan made by `ff_dvdvideo_titles_plan` as Rust values (`scan`:
    /// the scan it was built with, for the result names).
    ///
    /// # Safety
    /// `p` must be a plan made by `ff_dvdvideo_titles_plan`, not yet freed.
    #[must_use]
    pub unsafe fn plan_from_c(p: &PlanC, scan: Option<&Scan>) -> Plan {
        // SAFETY: the plan's arrays hold their counts.
        let titles = unsafe { slice(p.titles, p.nb_titles) }
            .iter()
            .map(|t| Title {
                name: c_text(&t.name),
                vtsn: t.vtsn,
                pgcn: t.pgcn,
                angle: t.angle,
                angles: t.angles,
                // SAFETY: as above.
                chapters: unsafe { slice(t.chapters, t.nb_chapters) }
                    .iter()
                    .map(|c| Chapter { pgcn: c.pgcn, pgn: c.pgn, cell: c.cell, segment: c.segment, offset: c.offset, time: c.time })
                    .collect(),
                scan: usize::try_from(t.scan).ok().and_then(|i| scan.and_then(|s| s.results.get(i))).map(|r| r.name.clone()),
                // SAFETY: as above.
                cells: unsafe { slice(t.cells, t.nb_cells) }.to_vec(),
                // SAFETY: as above.
                segments: unsafe { slice(t.segments, t.nb_segments) }
                    .iter()
                    .map(|g| Segment {
                        // SAFETY: as above.
                        extents: unsafe { slice(g.extents, g.nb_extents) }.to_vec(),
                        size: g.size,
                        label: c_text(&g.label),
                    })
                    .collect(),
                declared_secs: t.declared_secs,
                measured_secs: t.measured_secs,
                not_selected: t.not_selected,
            })
            .collect();
        // SAFETY: as above; the event names are static strings.
        let events = unsafe { slice(p.events, p.nb_events) }
            .iter()
            .map(|e| (unsafe { CStr::from_ptr(ff_dvdvideo_event_name(e.kind)) }.to_string_lossy().into_owned(), c_text(&e.args)))
            .collect();
        Plan { titles, events, scan: scan.cloned() }
    }

    /// Opens the disc at `path` (a disc folder or image), scans its navigation
    /// (unless the cell mode is trim) and builds its title plan. The error is
    /// an AVERROR code.
    pub fn titles(path: &Path, opts: &ImageOptions, attempts: c_int, topt: &TitleOptions) -> Result<Plan, c_int> {
        let c = CString::new(path.as_os_str().as_encoded_bytes()).map_err(|_| -22)?;
        let mut src: *mut Source = std::ptr::null_mut();
        // SAFETY: valid path and options; the source is closed below.
        let ret = unsafe { ff_dvdvideo_source_open(std::ptr::null_mut(), c.as_ptr(), opts, attempts, &raw mut src) };
        if ret < 0 {
            return Err(ret);
        }
        let mut disc: *mut Disc = std::ptr::null_mut();
        let mut scan_c: *mut ScanC = std::ptr::null_mut();
        let mut plan: *mut PlanC = std::ptr::null_mut();
        // SAFETY: an open source; every object is freed below (NULL accepted).
        let result = unsafe {
            let mut ret = ff_dvdvideo_disc_open(std::ptr::null_mut(), src, &raw mut disc);
            if ret >= 0 && topt.cell_mode != CELLS_TRIM {
                ret = ff_dvdvideo_scan(std::ptr::null_mut(), disc, None, std::ptr::null_mut(), &raw mut scan_c);
            }
            if ret >= 0 {
                ret = ff_dvdvideo_titles_plan(std::ptr::null_mut(), disc, scan_c, topt, &raw mut plan);
            }
            if ret < 0 {
                Err(ret)
            } else {
                let scan = (!scan_c.is_null()).then(|| convert(&*scan_c));
                Ok(plan_from_c(&*plan, scan.as_ref()))
            }
        };
        // SAFETY: as above.
        unsafe {
            ff_dvdvideo_titles_free(&raw mut plan);
            ff_dvdvideo_scan_free(&raw mut scan_c);
            ff_dvdvideo_disc_close(&raw mut disc);
            ff_dvdvideo_source_close(&raw mut src);
        }
        result
    }

    unsafe extern "C" fn trace_line(opaque: *mut c_void, line: *const c_char) {
        // SAFETY: opaque is the `&mut dyn FnMut(&str)` given to `scan`, line a
        // NUL-terminated string.
        let (f, line) = unsafe { (&mut *opaque.cast::<&mut dyn FnMut(&str)>(), CStr::from_ptr(line)) };
        f(&line.to_string_lossy());
    }

    /// Opens the disc at `path` (a disc folder or image) and scans its
    /// navigation; `trace` receives the scan's trace lines. The error is an
    /// AVERROR code.
    pub fn scan(
        path: &Path,
        opts: &ImageOptions,
        attempts: c_int,
        trace: Option<&mut dyn FnMut(&str)>,
    ) -> Result<Scan, c_int> {
        let c = CString::new(path.as_os_str().as_encoded_bytes()).map_err(|_| -22)?;
        let mut src: *mut Source = std::ptr::null_mut();
        // SAFETY: valid path and options; the source is closed below.
        let ret = unsafe { ff_dvdvideo_source_open(std::ptr::null_mut(), c.as_ptr(), opts, attempts, &raw mut src) };
        if ret < 0 {
            return Err(ret);
        }
        let mut cb = trace;
        let (tcb, topaque): (Option<TraceCb>, *mut c_void) = match cb.as_mut() {
            Some(f) => (Some(trace_line), std::ptr::from_mut(f).cast()),
            None => (None, std::ptr::null_mut()),
        };
        let mut out: *mut ScanC = std::ptr::null_mut();
        let mut disc: *mut Disc = std::ptr::null_mut();
        // SAFETY: an open source; the disc is closed below.
        let mut ret = unsafe { ff_dvdvideo_disc_open(std::ptr::null_mut(), src, &raw mut disc) };
        if ret >= 0 {
            // SAFETY: an open disc; the trace pointer lives across the call.
            ret = unsafe { ff_dvdvideo_scan(std::ptr::null_mut(), disc, tcb, topaque, &raw mut out) };
        }
        let result = if ret < 0 {
            Err(ret)
        } else {
            // SAFETY: a scan made by ff_dvdvideo_scan.
            Ok(unsafe { convert(&*out) })
        };
        // SAFETY: made by ff_dvdvideo_scan / ff_dvdvideo_disc_open (NULL is accepted).
        unsafe {
            ff_dvdvideo_scan_free(&raw mut out);
            ff_dvdvideo_disc_close(&raw mut disc);
        }
        // SAFETY: opened above.
        unsafe { ff_dvdvideo_source_close(&raw mut src) };
        result
    }

    unsafe fn convert(s: &ScanC) -> Scan {
        let n = usize::try_from(s.nb_results).unwrap_or(0);
        let results = if n == 0 {
            Vec::new()
        } else {
            // SAFETY: nb_results results.
            unsafe { std::slice::from_raw_parts(s.results, n) }
                .iter()
                .map(|r| ScanResult {
                    // SAFETY: a NUL-terminated name.
                    name: unsafe { CStr::from_ptr(r.name.as_ptr()) }.to_string_lossy().into_owned(),
                    title: u8::try_from(r.title & 0xff).unwrap_or(0),
                    pgcn: u8::try_from(r.pgcn & 0xff).unwrap_or(0),
                    cells: if r.nb_cells > 0 {
                        // SAFETY: nb_cells cells.
                        unsafe { std::slice::from_raw_parts(r.cells, usize::try_from(r.nb_cells).unwrap_or(0)) }.to_vec()
                    } else {
                        Vec::new()
                    },
                })
                .collect()
        };
        Scan {
            results,
            // SAFETY: a NUL-terminated text.
            failure: (s.failed != 0).then(|| unsafe { CStr::from_ptr(s.failure.as_ptr()) }.to_string_lossy().into_owned()),
            entered: (0..100u8).filter(|&i| s.entered[usize::from(i)] != 0).collect(),
        }
    }
}

/// The HD DVD demuxer's parts (`libavformat/hddvd_*.c`).
pub mod hddvd {
    use super::discio::Fs;
    use std::os::raw::{c_char, c_int, c_void};

    /// `HDDVD_VTI_MAX_EVOBS`.
    pub const VTI_MAX_EVOBS: usize = 1998;

    /// `HDDVDEvobAttr`.
    #[repr(C)]
    pub struct EvobAttr {
        pub raw: [u8; 0x186],
        pub nb_audio: c_int,
        pub nb_subpic: c_int,
        pub words: [u32; 32],
    }

    /// `HDDVDEvob`.
    #[repr(C)]
    pub struct Evob {
        pub playlist: c_int,
        pub name: [c_char; 256],
        pub base: [c_char; 256],
        pub attr: c_int,
        pub start_ptm: u32,
        pub end_ptm: u32,
        pub sectors: u32,
        pub slot: c_int,
        pub raw: [u8; 0x140],
    }

    /// `HDDVDVTI`.
    #[repr(C)]
    pub struct Vti {
        pub folder: [c_char; 9],
        pub nb_attrs: c_int,
        pub attrs: *mut EvobAttr,
        pub nb_evobs: c_int,
        pub evobs: [*mut Evob; VTI_MAX_EVOBS],
    }

    /// `HDDVDReadFn`.
    pub type ReadFn = unsafe extern "C" fn(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int;

    extern "C" {
        pub fn ff_hddvd_vti_parse(log: *mut c_void, read: ReadFn, opaque: *mut c_void, out: *mut *mut Vti) -> c_int;
        pub fn ff_hddvd_vti_open(log: *mut c_void, fs: *mut Fs, out: *mut *mut Vti) -> c_int;
        pub fn ff_hddvd_vti_free(vti: *mut *mut Vti);
    }

    /// `HDDVDXpl` (only handled through pointers; its first member is `file`,
    /// the N of VPLSTNNN.XPL).
    #[repr(C)]
    pub struct Xpl {
        pub file: c_int,
        _rest: [u8; 0],
    }

    extern "C" {
        pub fn ff_hddvd_xpl_parse(
            log: *mut c_void,
            read: ReadFn,
            opaque: *mut c_void,
            offset: i64,
            length: i64,
            out: *mut *mut Xpl,
        ) -> c_int;
        pub fn ff_hddvd_xpl_load(log: *mut c_void, fs: *mut Fs, out: *mut *mut *mut Xpl, nb: *mut c_int) -> c_int;
        pub fn ff_hddvd_xpl_free(xpl: *mut *mut Xpl);
        pub fn ff_hddvd_xpl_free_all(xpls: *mut *mut *mut Xpl, nb: c_int);
        /// Free the result with `av_free`.
        pub fn ff_hddvd_xpl_dump(xpl: *const Xpl) -> *mut c_char;
    }

    /// `HDDVDTitlePlan` (only handled through pointers).
    #[repr(C)]
    pub struct TitlePlan {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn ff_hddvd_titles_plan(
            log: *mut c_void,
            fs: *mut Fs,
            vti: *const Vti,
            xpls: *const *mut Xpl,
            nb_xpls: c_int,
            min_length: c_int,
            out: *mut *mut TitlePlan,
        ) -> c_int;
        pub fn ff_hddvd_titles_free(plan: *mut *mut TitlePlan);
        /// Free the result with `av_free`.
        pub fn ff_hddvd_titles_dump(plan: *const TitlePlan) -> *mut c_char;
        pub fn ff_disc_lang_code(code: *const c_char) -> *const c_char;
        pub fn ff_hddvd_titles_clip(plan: *const TitlePlan, slot: c_int) -> *mut Clip;
        pub fn ff_hddvd_evob_marks(log: *mut c_void, vti: *mut Vti, xpls: *const *mut Xpl, nb_xpls: c_int);
    }

    /// `HDDVDClip` (only handled through pointers).
    #[repr(C)]
    pub struct Clip {
        _private: [u8; 0],
    }

    /// `HDDVDAACS` (only handled through pointers).
    #[repr(C)]
    pub struct Aacs {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn ff_hddvd_aacs_open(
            log: *mut c_void,
            fs: *mut Fs,
            key_files: *const *const c_char,
            nb_key_files: c_int,
            nb_playlists: c_int,
            out: *mut *mut Aacs,
        ) -> c_int;
        pub fn ff_hddvd_aacs_close(aacs: *mut *mut Aacs);
        pub fn ff_hddvd_clip_block(log: *mut c_void, aacs: *mut Aacs, fs: *mut Fs, clip: *mut Clip, block: u32, buf: *mut u8) -> c_int;
        pub fn ff_hddvd_tracks_build(
            log: *mut c_void,
            fs: *mut Fs,
            aacs: *mut Aacs,
            vti: *const Vti,
            xpls: *const *mut Xpl,
            nb_xpls: c_int,
            plan: *mut TitlePlan,
        ) -> c_int;
    }
}

/// libavutil's AES and SHA (used by the tests to build encrypted discs).
pub mod avcrypto {
    use std::os::raw::{c_int, c_void};

    extern "C" {
        pub fn av_aes_alloc() -> *mut c_void;
        pub fn av_aes_init(a: *mut c_void, key: *const u8, key_bits: c_int, decrypt: c_int) -> c_int;
        pub fn av_aes_crypt(a: *mut c_void, dst: *mut u8, src: *const u8, count: c_int, iv: *mut u8, decrypt: c_int);
        pub fn av_sha_alloc() -> *mut c_void;
        pub fn av_sha_init(ctx: *mut c_void, bits: c_int) -> c_int;
        pub fn av_sha_update(ctx: *mut c_void, data: *const u8, len: usize);
        pub fn av_sha_final(ctx: *mut c_void, digest: *mut u8);
    }
}

/// `libavformat/discrip.h`: the shared rip core.
pub mod discrip {
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int, c_void};

    /// Ticks per second of the core's time unit.
    pub const TICKS_PER_SECOND: i64 = 1_080_000_000;
    /// Ticks per 90 kHz PES clock tick.
    pub const TICKS_PER_PTS: i64 = 12_000;

    /// `DRUnit`.
    #[repr(C)]
    pub struct Unit {
        pub data: *const u8,
        pub size: c_int,
        pub time: i64,
        pub pos: i64,
    }

    /// `DRCutterStats`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy)]
    pub struct CutterStats {
        pub bytes: i64,
        pub records: i64,
        pub units: i64,
        pub timed: i64,
        pub records_unused: i64,
        pub skipped: i64,
        pub skipped_bytes: i64,
    }

    pub const F_KEY: u32 = 0x0001;
    pub const F_SYNC: u32 = 0x0002;
    pub const F_MARKER: u32 = 0x0004;

    /// `DRFrame`.
    #[repr(C)]
    pub struct Frame {
        pub buf: *mut c_void,
        pub data: *mut u8,
        pub size: c_int,
        pub time: i64,
        pub dur: i64,
        pub pos: i64,
        pub flags: u32,
    }

    pub type FrameCb = unsafe extern "C" fn(opaque: *mut c_void, frame: *mut Frame) -> c_int;

    /// `DRAudioHeader`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
    pub struct AudioHeader {
        pub rate: c_int,
        pub samples: c_int,
    }

    /// `DRAudioStats`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy)]
    pub struct AudioStats {
        pub units: i64,
        pub frames: i64,
        pub markers: i64,
        pub cut_bytes: i64,
        pub review: i64,
        pub continuity: i64,
        pub header: AudioHeader,
    }

    /// `DRAudio` (only handled through pointers).
    #[repr(C)]
    pub struct Audio {
        _private: [u8; 0],
    }

    pub const AUDIO_CORE_ONLY: c_int = 0x0001;
    pub const F_TAIL: u32 = 0x0020;

    pub const EV_START_GAP: c_int = 1;
    pub const EV_START_DROP: c_int = 2;
    pub const EV_START_SHIFT: c_int = 3;
    pub const EV_OVERLAP: c_int = 4;
    pub const EV_GAP_ABSORBED: c_int = 5;
    pub const EV_GAP: c_int = 6;
    pub const EV_DROP: c_int = 7;
    pub const EV_GAP_MARKER: c_int = 8;
    pub const EV_VIDEO_ENDED: c_int = 9;
    pub const EV_TIME_ORDER: c_int = 10;
    pub const EV_RETIME: c_int = 11;
    pub const F_CHAPTER: u32 = 0x0010;
    pub const F_BATCH: u32 = 0x0040;
    pub const F_DISCARD: u32 = 0x0008;
    pub const EV_VIDEO_TIMECODE: c_int = 12;
    pub const EV_VIDEO_TIMECODE_LIMIT: c_int = 13;
    pub const EV_VIDEO_INVALID: c_int = 14;
    pub const EV_VIDEO_REPAIR: c_int = 15;
    pub const EV_VIDEO_RATE_CHANGE: c_int = 16;

    /// `DRVideoStats`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy)]
    pub struct VideoStats {
        pub units: i64,
        pub pictures: i64,
        pub out: i64,
        pub placeholders: i64,
        pub invalid: i64,
        pub num: c_int,
        pub den: c_int,
        pub base: i64,
    }

    /// `DRVideo` (only handled through pointers).
    #[repr(C)]
    pub struct Video {
        _private: [u8; 0],
    }

    pub const KIND_VIDEO: c_int = 1;
    pub const KIND_AUDIO: c_int = 2;
    pub const KIND_SUBTITLE: c_int = 3;

    pub type JoinOutCb = unsafe extern "C" fn(opaque: *mut c_void, track: c_int, frame: *mut Frame) -> c_int;

    /// `DRJoinConfig`.
    #[repr(C)]
    pub struct JoinConfig {
        pub nb_tracks: c_int,
        pub kinds: *const c_int,
        pub marks: *const i64,
        pub nb_marks: c_int,
        pub out: JoinOutCb,
        pub out_opaque: *mut c_void,
        pub event: Option<EventCb>,
        pub event_opaque: *mut c_void,
    }

    /// `DRJoinStats`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy)]
    pub struct JoinStats {
        pub segments: c_int,
        pub frames: i64,
        pub retimed: i64,
        pub chapters: i64,
        pub offset: i64,
        pub start: i64,
    }

    /// `DRJoin` (only handled through pointers).
    #[repr(C)]
    pub struct Join {
        _private: [u8; 0],
    }

    /// `DREvent`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
    pub struct Event {
        pub kind: c_int,
        pub track: c_int,
        pub pos: i64,
        pub dur: i64,
        pub skew: i64,
        pub count: i64,
    }

    pub type EventCb = unsafe extern "C" fn(opaque: *mut c_void, ev: *const Event);

    /// `DRVideoRef`.
    #[repr(C)]
    pub struct VideoRef {
        pub opaque: *mut c_void,
        pub max_time: unsafe extern "C" fn(opaque: *mut c_void) -> i64,
        pub advance: unsafe extern "C" fn(opaque: *mut c_void, target: i64, ended: *mut c_int) -> c_int,
    }

    /// `DRJunctionConfig`.
    #[repr(C)]
    pub struct JunctionConfig {
        pub track: c_int,
        pub frame_dur: i64,
        pub tolerance: i64,
        pub video: VideoRef,
        pub out: FrameCb,
        pub out_opaque: *mut c_void,
        pub event: Option<EventCb>,
        pub event_opaque: *mut c_void,
    }

    /// `DRJunctionStats`.
    #[repr(C)]
    #[derive(Debug, Default, Clone, Copy)]
    pub struct JunctionStats {
        pub input: i64,
        pub out: i64,
        pub dropped: i64,
        pub dropped_dur: i64,
        pub markers: i64,
        pub skew: i64,
        pub base: i64,
        pub ended: c_int,
    }

    /// `DRJunction` (only handled through pointers).
    #[repr(C)]
    pub struct Junction {
        _private: [u8; 0],
    }

    /// `DRCutter` (only handled through pointers).
    #[repr(C)]
    pub struct Cutter {
        _private: [u8; 0],
    }

    pub type UnitCb = unsafe extern "C" fn(opaque: *mut c_void, unit: *const Unit) -> c_int;

    /// The first members of `AVCodecDescriptor`.
    #[repr(C)]
    pub struct CodecDescriptor {
        pub id: c_int,
        pub kind: c_int,
        pub name: *const c_char,
    }

    extern "C" {
        pub fn avcodec_descriptor_get_by_name(name: *const c_char) -> *const CodecDescriptor;
        pub fn ff_discrip_cutter_open(
            out: *mut *mut Cutter,
            log: *mut c_void,
            codec: c_int,
            cb: UnitCb,
            opaque: *mut c_void,
        ) -> c_int;
        pub fn ff_discrip_cutter_write(c: *mut Cutter, data: *const u8, size: c_int, time: i64) -> c_int;
        pub fn ff_discrip_cutter_flush(c: *mut Cutter) -> c_int;
        pub fn ff_discrip_cutter_stats(c: *const Cutter, stats: *mut CutterStats);
        pub fn ff_discrip_cutter_close(c: *mut *mut Cutter);
        pub fn ff_discrip_frame_unref(f: *mut Frame);
        pub fn ff_discrip_audio_open(
            out: *mut *mut Audio,
            log: *mut c_void,
            codec: c_int,
            flags: c_int,
            cb: FrameCb,
            opaque: *mut c_void,
        ) -> c_int;
        pub fn ff_discrip_audio_unit(audio: *mut c_void, unit: *const Unit) -> c_int;
        pub fn ff_discrip_audio_marker(a: *mut Audio, time: i64) -> c_int;
        pub fn ff_discrip_audio_flush(a: *mut Audio) -> c_int;
        pub fn ff_discrip_audio_stats(a: *const Audio, stats: *mut AudioStats);
        pub fn ff_discrip_audio_close(a: *mut *mut Audio);
        pub fn ff_discrip_junction_open(out: *mut *mut Junction, log: *mut c_void, cfg: *const JunctionConfig) -> c_int;
        pub fn ff_discrip_junction_push(j: *mut Junction, frame: *mut Frame) -> c_int;
        pub fn ff_discrip_junction_finish(j: *mut Junction) -> c_int;
        pub fn ff_discrip_junction_stats(j: *const Junction, st: *mut JunctionStats);
        pub fn ff_discrip_junction_close(j: *mut *mut Junction);
        pub fn ff_discrip_join_open(out: *mut *mut Join, log: *mut c_void, cfg: *const JoinConfig) -> c_int;
        pub fn ff_discrip_join_segment(j: *mut Join) -> c_int;
        pub fn ff_discrip_join_push(j: *mut Join, track: c_int, frame: *mut Frame) -> c_int;
        pub fn ff_discrip_join_finish(j: *mut Join) -> c_int;
        pub fn ff_discrip_join_stats(j: *const Join, st: *mut JoinStats);
        pub fn ff_discrip_join_close(j: *mut *mut Join);
        pub fn ff_discrip_video_open(
            out: *mut *mut Video,
            log: *mut c_void,
            codec: c_int,
            track: c_int,
            cb: FrameCb,
            opaque: *mut c_void,
            event: Option<EventCb>,
            event_opaque: *mut c_void,
        ) -> c_int;
        pub fn ff_discrip_video_unit(video: *mut c_void, unit: *const Unit) -> c_int;
        pub fn ff_discrip_video_flush(v: *mut Video) -> c_int;
        pub fn ff_discrip_video_stats(v: *const Video, st: *mut VideoStats);
        pub fn ff_discrip_video_close(v: *mut *mut Video);
    }

    /// FFmpeg's codec id for a codec name (e.g. "ac3"), or None.
    #[must_use]
    pub fn codec_id(name: &CStr) -> Option<c_int> {
        // SAFETY: name is a valid C string; the result is a static descriptor or NULL.
        let d = unsafe { avcodec_descriptor_get_by_name(name.as_ptr()) };
        // SAFETY: a non-NULL result points to a static descriptor.
        (!d.is_null()).then(|| unsafe { (*d).id })
    }
}
