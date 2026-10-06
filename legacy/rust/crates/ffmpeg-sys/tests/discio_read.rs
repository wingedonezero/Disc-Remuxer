//! `libavformat/discio_read.c`: block reads with retries, the block-by-block
//! fallback after the first retry, 0xFF padding of a last partial block, and
//! reads at any offset.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::{c_int, c_void};

use ffmpeg_sys::discio::{self, Source, SourceOps};

/// How the test source answers each read call (0-based call index).
#[derive(Clone, Copy, PartialEq)]
enum Answer {
    Data,
    Fail,
    /// Return only this many bytes.
    Short(usize),
}

struct Mem {
    data: Vec<u8>,
    answers: Vec<(usize, Answer)>,
    calls: RefCell<Vec<(i64, usize)>>,
}

unsafe extern "C" fn mem_read(opaque: *mut c_void, pos: i64, buf: *mut u8, len: c_int) -> c_int {
    // SAFETY: opaque is the Mem the test keeps alive; buf has len bytes.
    let m = unsafe { &*opaque.cast::<Mem>() };
    let len = usize::try_from(len).unwrap();
    let n = m.calls.borrow().len();
    m.calls.borrow_mut().push((pos, len));
    let answer = m.answers.iter().find(|(i, _)| *i == n).map_or(Answer::Data, |a| a.1);
    if answer == Answer::Fail {
        return -5; // AVERROR(EIO)
    }
    let p = usize::try_from(pos).unwrap();
    if p >= m.data.len() {
        return 0;
    }
    let mut k = len.min(m.data.len() - p);
    if let Answer::Short(s) = answer {
        k = k.min(s);
    }
    // SAFETY: k <= len.
    unsafe { std::ptr::copy_nonoverlapping(m.data[p..].as_ptr(), buf, k) };
    c_int::try_from(k).unwrap()
}

const OPS: SourceOps = SourceOps { read_at: mem_read, close: None };

struct Reader {
    src: *mut Source,
    mem: Box<Mem>,
}

impl Reader {
    fn new(data: Vec<u8>, size: Option<u64>, answers: &[(usize, Answer)]) -> Self {
        let size = size.unwrap_or(data.len() as u64);
        let mut mem = Box::new(Mem { data, answers: answers.to_vec(), calls: RefCell::new(vec![]) });
        let name = CString::new("test source").unwrap();
        // SAFETY: OPS is static; mem outlives src (dropped after it).
        let src = unsafe {
            discio::ff_discio_source_new(
                std::ptr::null_mut(),
                name.as_ptr(),
                i64::try_from(size).unwrap(),
                &OPS,
                std::ptr::addr_of_mut!(*mem).cast(),
            )
        };
        assert!(!src.is_null());
        Reader { src, mem }
    }

    fn blocks(&self, pos: u64, buf: &mut [u8], attempts: i32) -> i32 {
        // SAFETY: buf is writable for its length.
        unsafe { discio::ff_discio_read_blocks(self.src, i64::try_from(pos).unwrap(), buf.as_mut_ptr(), c_int::try_from(buf.len()).unwrap(), attempts, 1) }
    }

    fn bytes(&self, pos: u64, buf: &mut [u8], attempts: i32) -> i32 {
        // SAFETY: buf is writable for its length.
        unsafe { discio::ff_discio_read_bytes(self.src, i64::try_from(pos).unwrap(), buf.as_mut_ptr(), c_int::try_from(buf.len()).unwrap(), attempts, 1) }
    }

    fn calls(&self) -> Vec<(i64, usize)> {
        self.mem.calls.borrow().clone()
    }
}

impl Drop for Reader {
    fn drop(&mut self) {
        // SAFETY: src came from ff_discio_source_new.
        unsafe { discio::ff_discio_source_free(&raw mut self.src) };
    }
}

fn pattern(n: usize) -> Vec<u8> {
    (0..n).map(|i| u8::try_from(i % 251).unwrap()).collect()
}

#[test]
fn whole_blocks_read_in_one_call() {
    let r = Reader::new(pattern(4 * 2048), None, &[]);
    let mut b = vec![0; 2 * 2048];
    assert_eq!(r.blocks(2048, &mut b, 5), 0);
    assert_eq!(b, pattern(4 * 2048)[2048..3 * 2048]);
    assert_eq!(r.calls().len(), 1);
}

#[test]
fn last_partial_block_is_padded_with_ff() {
    let r = Reader::new(pattern(2048 + 100), None, &[]);
    let mut b = vec![0; 2048];
    assert_eq!(r.blocks(2048, &mut b, 5), 0);
    assert_eq!(&b[..100], &pattern(2148)[2048..]);
    assert!(b[100..].iter().all(|&x| x == 0xFF));
}

#[test]
fn after_a_retry_the_rest_is_read_block_by_block() {
    let r = Reader::new(pattern(3 * 2048), None, &[(0, Answer::Fail)]);
    let mut b = vec![0; 3 * 2048];
    assert_eq!(r.blocks(0, &mut b, 5), 0);
    assert_eq!(b, pattern(3 * 2048));
    assert_eq!(r.calls(), [(0, 6144), (0, 2048), (2048, 2048), (4096, 2048)]);
}

#[test]
fn gives_up_after_the_attempt_count() {
    let r = Reader::new(pattern(2048), None, &[(0, Answer::Fail), (1, Answer::Fail), (2, Answer::Fail)]);
    let mut b = vec![0; 2048];
    assert!(r.blocks(0, &mut b, 3) < 0);
    assert_eq!(r.calls().len(), 3);
}

#[test]
fn zero_attempts_counts_as_one() {
    let r = Reader::new(pattern(2048), None, &[(0, Answer::Fail)]);
    let mut b = vec![0; 2048];
    assert!(r.blocks(0, &mut b, 0) < 0);
    assert_eq!(r.calls().len(), 1);
}

#[test]
fn no_data_is_a_failed_attempt() {
    // the source says 4 blocks but holds 1 (a file cut short)
    let r = Reader::new(pattern(2048), Some(4 * 2048), &[]);
    let mut b = vec![0; 2 * 2048];
    assert!(r.blocks(2048, &mut b, 5) < 0);
    assert_eq!(r.calls().len(), 5);
}

#[test]
fn a_part_of_a_block_is_retried() {
    let r = Reader::new(pattern(2 * 2048), None, &[(0, Answer::Short(1000))]);
    let mut b = vec![0; 2 * 2048];
    assert_eq!(r.blocks(0, &mut b, 5), 0);
    assert_eq!(b, pattern(2 * 2048));
    assert_eq!(r.calls(), [(0, 4096), (0, 2048), (2048, 2048)]);
}

#[test]
fn unaligned_block_reads_are_refused() {
    let r = Reader::new(pattern(4096), None, &[]);
    let mut b = vec![0; 2048];
    assert!(r.blocks(1, &mut b, 1) < 0);
    let mut b = vec![0; 100];
    assert!(r.blocks(0, &mut b, 1) < 0);
    assert!(r.calls().is_empty());
}

#[test]
fn bytes_at_any_offset() {
    let data = pattern(5 * 2048);
    let r = Reader::new(data.clone(), None, &[]);
    for (pos, len) in [(0, 10), (100, 3000), (2047, 2), (1000, 3 * 2048), (4096, 2048), (5 * 2048 - 7, 7)] {
        let mut b = vec![0; len];
        assert_eq!(r.bytes(pos as u64, &mut b, 5), 0, "pos {pos} len {len}");
        assert_eq!(b, data[pos..pos + len], "pos {pos} len {len}");
    }
}

#[test]
fn host_file_source() {
    let dir = std::env::temp_dir().join(format!("discio-host-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let path = dir.join("image.bin");
    std::fs::write(&path, pattern(3 * 2048 + 5)).unwrap();
    let cpath = CString::new(path.to_str().unwrap()).unwrap();
    let mut src: *mut Source = std::ptr::null_mut();
    // SAFETY: valid path and out pointer.
    assert_eq!(unsafe { discio::ff_discio_source_open_file(std::ptr::null_mut(), cpath.as_ptr(), &raw mut src) }, 0);
    let mut b = vec![0; 2 * 2048];
    // SAFETY: src is open, b writable.
    // blocks 2 and 3: block 3 holds the file's last 5 bytes, then 0xFF
    assert_eq!(unsafe { discio::ff_discio_read_blocks(src, 4096, b.as_mut_ptr(), 4096, 5, 1) }, 0);
    assert_eq!(&b[..2048], &pattern(3 * 2048)[4096..6144]);
    assert_eq!(&b[2048..2053], &pattern(3 * 2048 + 5)[3 * 2048..]);
    assert!(b[2053..].iter().all(|&x| x == 0xFF));
    // SAFETY: src came from ff_discio_source_open_file.
    unsafe { discio::ff_discio_source_free(&raw mut src) };
    assert!(src.is_null());
}

#[test]
fn only_regular_files_open_as_host_files() {
    let dir = std::env::temp_dir().join(format!("discio-host-dir-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let cpath = CString::new(dir.to_str().unwrap()).unwrap();
    let mut src: *mut Source = std::ptr::null_mut();
    // SAFETY: valid path and out pointer.
    let ret = unsafe { discio::ff_discio_source_open_file(std::ptr::null_mut(), cpath.as_ptr(), &raw mut src) };
    assert_eq!(ret, -22, "a directory is refused (EINVAL)");
    assert!(src.is_null());
    let _ = std::fs::remove_dir(&dir);
}

#[test]
fn file_reads_cross_extents_and_not_recorded_runs_read_as_zeros() {
    use discio::{Extent, File, Fs, LABEL_SIZE, SECTOR_NOT_RECORDED};
    let data = pattern(4 * 2048);
    let r = Reader::new(data.clone(), None, &[]);
    let mut fs = Fs {
        ops: std::ptr::null(),
        priv_: std::ptr::null_mut(),
        src: r.src,
        label: [0; LABEL_SIZE],
        udf_revision: 0,
        udf_recording_time: [0; 12],
    };
    // file = source block 3, then a block that is not recorded, then block 1
    let mut ext = [
        Extent { sector: 3, count: 1 },
        Extent { sector: SECTOR_NOT_RECORDED, count: 1 },
        Extent { sector: 1, count: 1 },
    ];
    let file = File { size: 3 * 2048 - 10, nb_extents: 3, extents: ext.as_mut_ptr(), data: std::ptr::null_mut() };
    let mut b = vec![0xAAu8; 3 * 2048 - 10];
    // SAFETY: fs / file describe live memory; b is writable.
    let ret = unsafe { discio::ff_discio_file_read(&raw mut fs, &raw const file, 0, b.as_mut_ptr(), c_int::try_from(b.len()).unwrap()) };
    assert_eq!(ret, 0);
    assert_eq!(&b[..2048], &data[3 * 2048..]);
    assert!(b[2048..4096].iter().all(|&x| x == 0));
    assert_eq!(&b[4096..], &data[2048..2 * 2048 - 10]);
    // past the end of the file
    let mut one = [0u8; 20];
    // SAFETY: as above.
    assert!(unsafe { discio::ff_discio_file_read(&raw mut fs, &raw const file, 3 * 2048 - 15, one.as_mut_ptr(), 20) } < 0);
}
