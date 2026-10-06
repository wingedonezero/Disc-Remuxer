//! Calls into our FFmpeg: versions, log routing, the stock DVD-Video probe.

use disc_core::settings::Settings;
use disc_core::{emit, msg};
use ffmpeg_sys::log_level;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::path::{Path, PathBuf};

pub fn version() -> String {
    // SAFETY: av_version_info returns a static NUL-terminated string.
    unsafe { CStr::from_ptr(ffmpeg_sys::av_version_info()) }
        .to_string_lossy()
        .into_owned()
}

pub fn library_versions() -> [(&'static str, (u32, u32, u32)); 3] {
    use ffmpeg_sys::version_triplet as t;
    // SAFETY: plain version getters without arguments.
    unsafe {
        [
            ("libavutil", t(ffmpeg_sys::avutil_version())),
            ("libavcodec", t(ffmpeg_sys::avcodec_version())),
            ("libavformat", t(ffmpeg_sys::avformat_version())),
        ]
    }
}

/// FFmpeg's log (with libdvdread / libdvdnav's, which FFmpeg forwards) goes
/// to our log under the target `ffmpeg`.
pub fn route_log(level: log::LevelFilter) {
    let max = match level {
        log::LevelFilter::Off => log_level::QUIET,
        log::LevelFilter::Error => log_level::ERROR,
        log::LevelFilter::Warn => log_level::WARNING,
        log::LevelFilter::Info => log_level::INFO,
        log::LevelFilter::Debug => log_level::DEBUG,
        log::LevelFilter::Trace => log_level::TRACE,
    };
    // SAFETY: sink is a valid extern "C" fn for the program's lifetime.
    unsafe { ffmpeg_sys::dr_log_install(sink, max) };
}

unsafe extern "C" fn sink(level: c_int, line: *const c_char) {
    // SAFETY: glue.c passes a NUL-terminated line.
    let text = unsafe { CStr::from_ptr(line) }.to_string_lossy();
    let level = match level {
        l if l <= log_level::ERROR => log::Level::Error,
        l if l <= log_level::WARNING => log::Level::Warn,
        l if l <= log_level::INFO => log::Level::Info,
        l if l <= log_level::DEBUG => log::Level::Debug,
        _ => log::Level::Trace,
    };
    log::log!(target: "ffmpeg", level, "{text}");
}

/// The DVD-Video demuxer's options for opening title `title` with the read
/// settings in effect, as `key=value` pairs joined by `:`.
#[must_use]
pub fn dvdvideo_options(settings: &Settings, title: i32) -> String {
    format!(
        "title={title}:read_attempts={}:udf_reader={}:prefer_iso_old_udf102={}",
        settings.number("read.attempts"),
        settings.text("read.udf_reader").unwrap_or("netbsd"),
        i32::from(settings.switch("read.prefer_iso_for_old_udf102")),
    )
}

/// Opens one title with FFmpeg's DVD-Video demuxer and logs FFmpeg's stream
/// dump; the error is FFmpeg's reason.
pub fn probe_dvdvideo(source: &Path, title: i32, settings: &Settings) -> Result<(), String> {
    let path = CString::new(source.as_os_str().as_encoded_bytes())
        .map_err(|_| "the path contains a NUL byte".to_string())?;
    let options = CString::new(dvdvideo_options(settings, title)).expect("no NUL in the options");
    // SAFETY: path and options are valid NUL-terminated strings.
    let ret = unsafe { ffmpeg_sys::dr_probe_dvdvideo(path.as_ptr(), options.as_ptr()) };
    if ret < 0 {
        return Err(ffmpeg_sys::error_text(ret));
    }
    Ok(())
}

/// The read settings as disc-image options and read attempts.
fn read_options(settings: &Settings) -> (ffmpeg_sys::discio::ImageOptions, c_int) {
    let opts = ffmpeg_sys::discio::ImageOptions {
        udf_reader: if settings.text("read.udf_reader") == Some("linux") {
            ffmpeg_sys::discio::UDF_LINUX
        } else {
            ffmpeg_sys::discio::UDF_NETBSD
        },
        prefer_iso_for_old_udf102: c_int::from(settings.switch("read.prefer_iso_for_old_udf102")),
    };
    (opts, c_int::try_from(settings.number("read.attempts")).unwrap_or(5))
}

/// `debug dvd-scan`: the navigation scan of one DVD, its trace (when asked)
/// and its results on standard output.
pub fn debug_dvd_scan(source: &Path, trace: bool, settings: &Settings) -> anyhow::Result<()> {
    use std::io::Write as _;

    let (opts, attempts) = read_options(settings);
    let mut out = std::io::stdout().lock();
    let mut print = |line: &str| {
        let _ = writeln!(out, "{line}");
    };
    let scan = ffmpeg_sys::dvdvideo::scan(source, &opts, attempts, if trace { Some(&mut print) } else { None })
        .map_err(|e| anyhow::anyhow!("the navigation scan of {} could not run: {}", source.display(), ffmpeg_sys::error_text(e)))?;
    let mut out = std::io::stdout().lock();
    if let Some(reason) = &scan.failure {
        writeln!(out, "scan failed: {reason}")?;
    }
    for r in &scan.results {
        let cells: Vec<String> = r.cells.iter().map(ToString::to_string).collect();
        writeln!(out, "result {} title {} pgc {} cells {}", r.name, r.title, r.pgcn, cells.join(","))?;
    }
    for t in scan.entered.iter().filter(|&&t| t != 0) {
        writeln!(out, "entered {t}")?;
    }
    Ok(())
}

/// The dvd.* settings as title-plan options.
#[must_use]
pub fn title_options(settings: &Settings) -> ffmpeg_sys::dvdvideo::TitleOptions {
    use ffmpeg_sys::dvdvideo as d;
    ffmpeg_sys::dvdvideo::TitleOptions {
        cell_mode: match settings.text("dvd.cell_mode") {
            Some("walk") => d::CELLS_WALK,
            Some("trim") => d::CELLS_TRIM,
            Some("walk_trim") => d::CELLS_WALK_TRIM,
            _ => d::CELLS_AUTO,
        },
        title_order: match settings.text("dvd.title_order") {
            Some("scan_first") => d::ORDER_SCAN_FIRST,
            Some("table") => d::ORDER_TABLE,
            _ => d::ORDER_AUTO,
        },
        min_length: c_int::try_from(settings.number("dvd.min_title_length")).unwrap_or(120),
    }
}

/// "h:mm:ss" of `s` seconds.
fn hms(s: u32) -> String {
    format!("{}:{:02}:{:02}", s / 3600, s / 60 % 60, s % 60)
}

/// `debug dvd-titles`: the title plan of one DVD on standard output.
pub fn debug_dvd_titles(source: &Path, settings: &Settings) -> anyhow::Result<()> {
    use std::io::Write as _;

    let (opts, attempts) = read_options(settings);
    let plan = ffmpeg_sys::dvdvideo::titles(source, &opts, attempts, &title_options(settings))
        .map_err(|e| anyhow::anyhow!("the title plan of {} could not be built: {}", source.display(), ffmpeg_sys::error_text(e)))?;
    let mut out = std::io::stdout().lock();
    if let Some(reason) = plan.scan.as_ref().and_then(|s| s.failure.as_ref()) {
        writeln!(out, "scan failed: {reason}")?;
    }
    for (name, args) in &plan.events {
        if args.is_empty() {
            writeln!(out, "event {name}")?;
        } else {
            writeln!(out, "event {name}\t{args}")?;
        }
    }
    for t in &plan.titles {
        let size: u64 = t.segments.iter().map(|s| s.size).sum();
        let map: Vec<&str> = t.segments.iter().map(|s| s.label.as_str()).collect();
        let selected = match t.not_selected {
            ffmpeg_sys::dvdvideo::TITLE_SHORT => "short",
            ffmpeg_sys::dvdvideo::TITLE_FAKE => "fake",
            _ => "yes",
        };
        writeln!(
            out,
            "title {} vts {} pgc {} angle {}/{} cells {} chapters {} length {} measured {} size {} segments {} map {} scan {} selected {}",
            t.name,
            t.vtsn,
            t.pgcn,
            t.angle + 1,
            t.angles,
            t.cells.len(),
            t.chapters.len(),
            hms(t.declared_secs),
            t.measured_secs,
            size,
            t.segments.len(),
            if map.is_empty() { "-".to_string() } else { map.join(",") },
            t.scan.as_deref().unwrap_or("-"),
            selected
        )?;
    }
    Ok(())
}

/// The HD DVD demuxer's options for title `title` with the read settings.
fn hddvd_options(settings: &Settings, title: i32) -> String {
    format!(
        "title={title}:read_attempts={}:udf_reader={}",
        settings.number("read.attempts"),
        settings.text("read.udf_reader").unwrap_or("netbsd"),
    )
}

/// Milliseconds as h:mm:ss.mmm.
fn ms(v: i64) -> String {
    if v == ffmpeg_sys::AV_NOPTS_VALUE {
        return "-".into();
    }
    let sign = if v < 0 { "-" } else { "" };
    let t = v.unsigned_abs();
    format!("{sign}{}:{:02}:{:02}.{:03}", t / 3_600_000, t / 60_000 % 60, t / 1000 % 60, t % 1000)
}

fn text(p: *const c_char) -> String {
    if p.is_null() {
        return String::new();
    }
    // SAFETY: the glue passes NUL-terminated strings that live during the callback.
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

unsafe extern "C" fn demux_stream(_opaque: *mut c_void, st: *const ffmpeg_sys::DemuxStreamStats) {
    // SAFETY: the glue passes a valid record for the duration of the call.
    let s = unsafe { &*st };
    let (kind, codec, lang, file) = (text(s.kind), text(s.codec), text(s.lang), text(s.file));
    let lang = if lang.is_empty() { "-".to_string() } else { lang };
    if file.is_empty() {
        disc_core::emit!(disc_core::msg::DEMUX_EMPTY, index = s.index, kind = kind, codec = codec, lang = lang);
        return;
    }
    disc_core::emit!(disc_core::msg::DEMUX_STREAM, index = s.index, kind = kind, codec = codec, lang = lang,
        packets = s.packets, bytes = s.bytes, start = ms(s.first_ms), end = ms(s.end_ms), file = file);
}

/// Where a title's files go: its name from the template; with `sub_folder`
/// (every title of the disc) in a folder of that name.
struct TitleFiles<'a> {
    folder: &'a Path,
    sub_folder: bool,
    template: &'a str,
    index: u32,
    chosen: Option<PathBuf>,
}

unsafe extern "C" fn demux_name(opaque: *mut c_void, title_name: *const c_char, out: *mut c_char, size: c_int) -> c_int {
    // SAFETY: opaque is the TitleFiles of the call; out has size bytes.
    let tf = unsafe { &mut *opaque.cast::<TitleFiles<'_>>() };
    let name = (!title_name.is_null()).then(|| text(title_name));
    let base = disc_core::names::title_name(tf.template, &disc_core::names::TitleNaming {
        name: name.as_deref(),
        index: tf.index,
        ..Default::default()
    });
    let dir = if tf.sub_folder { tf.folder.join(&base) } else { tf.folder.to_path_buf() };
    if let Err(e) = std::fs::create_dir_all(&dir) {
        emit!(msg::DEMUX_FOLDER_FAILED, folder = dir.display(), reason = e);
        return -e.raw_os_error().unwrap_or(5);
    }
    let prefix = dir.join(&base);
    let bytes = prefix.to_string_lossy().into_owned().into_bytes();
    let Ok(cap) = usize::try_from(size) else { return -22 };
    if bytes.len() >= cap || bytes.contains(&0) {
        return -36; // ENAMETOOLONG
    }
    // SAFETY: out has room for size bytes; bytes.len() + 1 <= size.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), bytes.len());
        *out.add(bytes.len()) = 0;
    }
    tf.chosen = Some(prefix);
    0
}

/// `demux`: every title (or one) of an HD DVD image as elementary streams and
/// chapters in `folder`.
pub fn demux_hddvd(source: &Path, title: Option<i32>, folder: &Path, settings: &Settings) -> anyhow::Result<()> {
    let c = |s: &str| CString::new(s).map_err(|_| anyhow::anyhow!("a path contains a NUL byte"));
    let path = c(&source.to_string_lossy())?;
    let keys = c(settings.text("aacs.key_files").unwrap_or(""))?;
    let format = c("hddvd")?;
    let template = settings.text("output.file_name_template").unwrap_or("");
    let mut t = title.unwrap_or(0);
    loop {
        emit!(msg::DEMUX_TITLE, title = t, source = source.display(), folder = folder.display());
        let options = c(&hddvd_options(settings, t))?;
        let mut tf = TitleFiles {
            folder,
            sub_folder: title.is_none(),
            template,
            index: u32::try_from(t).unwrap_or(0),
            chosen: None,
        };
        let mut nb: c_int = -1;
        // SAFETY: every pointer is a valid NUL-terminated string; tf outlives the call; the callbacks
        // match the glue's types.
        let ret = unsafe {
            ffmpeg_sys::dr_demux(format.as_ptr(), path.as_ptr(), options.as_ptr(), keys.as_ptr(), demux_name,
                (&raw mut tf).cast(), &raw mut nb, demux_stream, std::ptr::null_mut())
        };
        if let Some(p) = &tf.chosen {
            emit!(msg::DEMUX_NAME, title = t, prefix = p.display());
        }
        if ret < 0 {
            let reason = ffmpeg_sys::error_text(ret);
            emit!(msg::DEMUX_FAILED, title = t, source = source.display(), reason = reason);
            anyhow::bail!("title {t}: {reason}");
        }
        if title.is_none() && t == 0 {
            emit!(msg::DEMUX_TITLES, source = source.display(), count = nb);
        }
        t += 1;
        if title.is_some() || t >= nb {
            return Ok(());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_settings_become_demuxer_options() {
        let mut s = Settings::defaults();
        assert_eq!(dvdvideo_options(&s, 3), "title=3:read_attempts=5:udf_reader=netbsd:prefer_iso_old_udf102=1");
        s.set_from_command_line("read.attempts=9").unwrap();
        s.set_from_command_line("read.udf_reader=linux").unwrap();
        s.set_from_command_line("read.prefer_iso_for_old_udf102=false").unwrap();
        assert_eq!(dvdvideo_options(&s, 1), "title=1:read_attempts=9:udf_reader=linux:prefer_iso_old_udf102=0");
    }
}
