//! `CCExtractor`, compiled from `libs/ccextractor` by `build.rs` (C only) into
//! the `ccextractor` program, which is copied next to ours. It runs as a
//! helper process: CEA-608 caption data in its raw caption format on stdin,
//! SRT text on stdout ([`Captions`]).

use std::io::{self, Read, Write};
use std::path::PathBuf;
use std::process::{Child, ChildStdin, Command, Stdio};
use std::thread::JoinHandle;

/// Version of the compiled `CCExtractor` copy.
pub const VERSION: &str = "0.94";

/// File name of the helper program.
pub const PROGRAM: &str = "ccextractor";

/// The helper's options: raw caption input ("bin") on stdin, SRT in UTF-8 on
/// stdout, no input buffering, output flushed as it is written.
pub const ARGS: [&str; 7] = ["-in=bin", "-out=srt", "-stdin", "-stdout", "-utf8", "-nobi", "-ff"];

/// The header of the raw caption format (RCWT): magic, creator, version,
/// format revision 1 (CEA-608 blocks).
const RCWT_HEADER: [u8; 11] = [0xCC, 0xCC, 0xED, 0xCC, 0x00, 0x50, 0x00, 0x01, 0x00, 0x00, 0x00];

/// The helper program next to the running executable (where the build puts
/// it), or None when it is not there.
#[must_use]
pub fn program() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let mut dir = exe.parent()?.to_path_buf();
    // test executables live in target/<profile>/deps
    if dir.ends_with("deps") {
        dir.pop();
    }
    let p = dir.join(PROGRAM);
    p.is_file().then_some(p)
}

/// One caption as the helper wrote it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Cue {
    /// Start and end in milliseconds (the clock of the blocks given in).
    pub start_ms: u64,
    pub end_ms: u64,
    /// The cue's text lines, each followed by `\n` (CR and a BOM removed).
    pub text: String,
}

/// What a finished conversion gave.
#[derive(Debug)]
pub struct Output {
    pub cues: Vec<Cue>,
    /// The helper's messages (stderr), for the log.
    pub messages: String,
}

/// A running conversion: caption blocks in, cues out at [`Captions::finish`].
pub struct Captions {
    child: Child,
    stdin: Option<ChildStdin>,
    out: Option<JoinHandle<io::Result<Vec<u8>>>>,
    err: Option<JoinHandle<io::Result<Vec<u8>>>>,
    started: bool,
}

fn drain(mut r: impl Read + Send + 'static) -> JoinHandle<io::Result<Vec<u8>>> {
    std::thread::spawn(move || {
        let mut v = Vec::new();
        r.read_to_end(&mut v)?;
        Ok(v)
    })
}

impl Captions {
    /// Starts the helper at `program`.
    pub fn start(program: &std::path::Path) -> io::Result<Self> {
        let mut child = Command::new(program)
            .args(ARGS)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;
        let stdin = child.stdin.take();
        let out = child.stdout.take().map(drain);
        let err = child.stderr.take().map(drain);
        Ok(Captions { child, stdin, out, err, started: false })
    }

    /// One block at `ms`: `triplets` = 3 bytes per entry (field marker 4 /
    /// 5 and the byte pair). A block without entries is not written.
    pub fn push(&mut self, ms: u64, triplets: &[u8]) -> io::Result<()> {
        let count = triplets.len() / 3;
        if count == 0 {
            return Ok(());
        }
        let count = u16::try_from(count).map_err(|_| io::Error::other("too many caption entries in one block"))?;
        let w = self.stdin.as_mut().ok_or_else(|| io::Error::other("the conversion is finished"))?;
        if !self.started {
            w.write_all(&RCWT_HEADER)?;
            self.started = true;
        }
        w.write_all(&ms.to_le_bytes())?;
        w.write_all(&count.to_le_bytes())?;
        w.write_all(&triplets[..usize::from(count) * 3])
    }

    /// The end of the caption data: waits for the helper and parses its
    /// SRT. Fails when the helper fails or its output does not parse.
    pub fn finish(mut self) -> io::Result<Output> {
        drop(self.stdin.take());
        let status = self.child.wait()?;
        let join = |h: Option<JoinHandle<io::Result<Vec<u8>>>>| -> io::Result<Vec<u8>> {
            h.map_or(Ok(Vec::new()), |h| h.join().map_err(|_| io::Error::other("a pipe reader panicked"))?)
        };
        let out = join(self.out.take())?;
        let messages = String::from_utf8_lossy(&join(self.err.take())?).into_owned();
        if !status.success() {
            return Err(io::Error::other(format!("{PROGRAM} failed ({status}): {}", messages.trim())));
        }
        let cues = if self.started { parse_srt(&out)? } else { Vec::new() };
        Ok(Output { cues, messages })
    }
}

/// `HH:MM:SS,mmm` in milliseconds.
fn srt_time(s: &str) -> u64 {
    let n = |r: std::ops::Range<usize>| s[r].parse::<u64>().unwrap_or(0);
    ((n(0..2) * 3600 + n(3..5) * 60 + n(6..8)) * 1000) + n(9..12)
}

/// The helper's SRT: cues numbered from 1 without a gap, each a timing line
/// of exactly `HH:MM:SS,mmm --> HH:MM:SS,mmm` and text lines up to an
/// empty line. Anything else is an error.
pub fn parse_srt(srt: &[u8]) -> io::Result<Vec<Cue>> {
    let text = String::from_utf8_lossy(srt);
    let text = text.strip_prefix('\u{feff}').unwrap_or(&text);
    let mut lines = text.split('\n').map(|l| l.strip_suffix('\r').unwrap_or(l)).peekable();
    let bad = |what: String| io::Error::new(io::ErrorKind::InvalidData, what);
    let mut cues = Vec::new();
    loop {
        while lines.peek().is_some_and(|l| l.is_empty()) {
            lines.next();
        }
        let Some(number) = lines.next() else { break };
        if number.parse::<usize>().ok() != Some(cues.len() + 1) {
            return Err(bad(format!("caption {}: number line {number:?}", cues.len() + 1)));
        }
        let timing = lines.next().unwrap_or("");
        let b = timing.as_bytes();
        if b.len() != 29
            || !timing.is_ascii()
            || &timing[12..17] != " --> "
            || [2, 5, 19, 22].iter().any(|&i| b[i] != b':')
            || [8, 25].iter().any(|&i| b[i] != b',')
        {
            return Err(bad(format!("caption {}: timing line {timing:?}", cues.len() + 1)));
        }
        let mut body = String::new();
        while let Some(l) = lines.next_if(|l| !l.is_empty()) {
            body.push_str(l);
            body.push('\n');
        }
        cues.push(Cue { start_ms: srt_time(&timing[..12]), end_ms: srt_time(&timing[17..]), text: body });
    }
    Ok(cues)
}
