//! `libavformat/hddvd_tracks.c`: the tracks of HD DVD titles. With
//! `HDDVD_CORPUS` (a folder of HD DVD images), `HDDVD_TRACKS_REFERENCE` (one
//! `<image name>.tracks.txt` per image, every character but A-Z a-z 0-9 _ of
//! the name replaced by _; one line per title: index|first EVO file|tracks,
//! each track type:Matroska codec id:language) and `HDDVD_KEYDB` (key files,
//! comma-separated, for the encrypted images) every image that opens must
//! give the reference's titles and tracks.

mod common;

use common::hddvd::{
    ac3_frame, attr_record, clip, eac3_frame, evob, image, open, open_file, pack, playlist, ps1_packet, tmap_blocks,
    video_packet, vti_with, Evob, File,
};
use common::Img;
use std::fmt::Write as _;

/// Matroska codec id of an FFmpeg codec name (the codecs HD DVD tracks have).
fn mkv_codec(name: &str) -> &str {
    match name {
        "mpeg2video" => "V_MPEG2",
        "vc1" => "V_VC1",
        "h264" => "V_MPEG4/ISO/AVC",
        "ac3" => "A_AC3",
        "eac3" => "A_EAC3",
        "truehd" => "A_TRUEHD",
        "dts" => "A_DTS",
        "mp2" => "A_MPEG/L2",
        "pcm_dvd" => "A_PCM/INT/BIG",
        "dvd_subtitle" => "S_VOBSUB",
        other => other,
    }
}

/// Our titles and tracks as reference lines.
fn track_lines(dump: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for l in dump.lines() {
        if let Some(rest) = l.strip_prefix("title ") {
            // i|name|lang|kind|selected|duration|secs|size|file|...
            let f: Vec<&str> = rest.split('|').collect();
            out.push(format!("{}|{}|", f[0], f[8]));
        } else if let Some(rest) = l.strip_prefix("  track ") {
            // k|type|codec|index|number|lang[|WxH]
            let f: Vec<&str> = rest.split('|').collect();
            let kind = match f[1] {
                "video" => "Video",
                "audio" => "Audio",
                "subtitle" => "Subtitles",
                other => other,
            };
            let last = out.last_mut().unwrap();
            if !last.ends_with('|') {
                last.push(',');
            }
            let _ = write!(last, "{kind}:{}:{}", mkv_codec(f[2]), f[5]);
        }
    }
    out
}

#[test]
fn corpus_every_track_list_equals_the_reference() {
    let (Ok(dir), Ok(refdir)) = (std::env::var("HDDVD_CORPUS"), std::env::var("HDDVD_TRACKS_REFERENCE")) else {
        eprintln!("HDDVD_CORPUS / HDDVD_TRACKS_REFERENCE not set: corpus check skipped");
        return;
    };
    let kf: Vec<String> = std::env::var("HDDVD_KEYDB").map(|s| s.split(',').map(str::to_owned).collect()).unwrap_or_default();
    let mut images: Vec<_> = std::fs::read_dir(&dir)
        .unwrap()
        .map(|e| e.unwrap().path())
        .filter(|p| p.extension().is_some_and(|x| x.eq_ignore_ascii_case("iso")))
        .collect();
    images.sort();
    let (mut bad, mut checked) = (0, 0);
    for img in images {
        let stem: String = img.file_stem().unwrap().to_str().unwrap().chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
            .collect();
        let d = match open_file(&img, &kf) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("{stem}: does not open ({e}): not checked");
                continue;
            }
        };
        let got = track_lines(&d.tracks().unwrap());
        let want: Vec<String> = std::fs::read_to_string(format!("{refdir}/{stem}.tracks.txt"))
            .unwrap()
            .lines()
            .map(str::to_owned)
            .collect();
        checked += 1;
        if got == want {
            eprintln!("{stem}: {} titles, tracks equal", got.len());
            continue;
        }
        bad += 1;
        eprintln!("{stem}: DIFFERS ({} titles, reference {})", got.len(), want.len());
        for i in 0..got.len().max(want.len()) {
            let (g, w) = (got.get(i).map_or("-", String::as_str), want.get(i).map_or("-", String::as_str));
            if g != w {
                eprintln!("  ours {g}\n  ref  {w}");
            }
        }
    }
    assert!(checked > 0, "no image opened");
    assert_eq!(bad, 0, "track lists differ");
}


// ---- rules on built images ----

const T: &str = "00:00:00:00";
const PALETTE: [u32; 32] = {
    let mut p = [0u32; 32];
    let mut i = 0u32;
    while i < 32 {
        p[i as usize] = 0x0010_8080 + i;
        i += 1;
    }
    p
};

/// A disc: EVOBs with their attribute records, a playlist, each EVOB's
/// stream content (packs; empty: one padding pack).
fn disc(evobs: &[Evob], attr_of: &[u32], attrs: &[Vec<u8>], xpl: Vec<u8>, content: Vec<Vec<u8>>) -> Img {
    let mut files: Vec<File> = vec![("ADV_OBJ", "VPLST000.XPL", xpl), ("HVDVD_TS", "HVA00001.VTI", vti_with(evobs, attr_of, attrs))];
    for (e, c) in evobs.iter().zip(content) {
        let c = if c.is_empty() { pack(&[]) } else { c };
        let base = e.name.strip_suffix(".EVO").unwrap();
        files.push(("HVDVD_TS", Box::leak(format!("{base}.MAP").into_boxed_str()), tmap_blocks(c.len() / 2048)));
        files.push(("HVDVD_TS", e.name, c));
    }
    image(&files)
}

/// The titles and tracks of a disc: the title lines' first EVO file, the track lines.
fn tracks_of(im: &Img) -> Vec<String> {
    let d = open(im, &[]).unwrap().tracks().unwrap();
    d.lines()
        .filter_map(|l| {
            if let Some(r) = l.strip_prefix("title ") {
                Some(format!("title {}", r.split('|').nth(8).unwrap()))
            } else {
                l.strip_prefix("  track ").map(|r| format!("  {r}"))
            }
        })
        .collect()
}

fn one_title(name: &str, extra: &str) -> Vec<u8> {
    playlist("", &format!("<Title>{}</Title>", clip_with(name, extra)))
}

/// A `PrimaryAudioVideoClip` with child elements.
fn clip_with(map: &str, children: &str) -> String {
    let c = clip(map, T, T, false);
    format!("{}>{children}</PrimaryAudioVideoClip>", c.strip_suffix("/>").unwrap())
}

#[test]
fn tracks_follow_the_attribute_record() {
    let attr = attr_record(
        [0x60, 0, 0x50],
        &[0x00, 0x04, 0x08, 0x10, 0x14, 0x20],
        &[[0x00, 0x21, 0, 0, 0], [0x20, 0, 0, 0x23, 0], [0x80, 0, 0, 0, 0x24], [0x40, 0x25, 0, 0, 0], [0x00, 0, 0, 0, 0]],
        &PALETTE,
    );
    let im = disc(&[evob("A.EVO", 1, 10)], &[1], &[attr], one_title("A.MAP", ""), vec![vec![]]);
    // a first byte of 0x20 or more: no track; sub-pictures: the stream number of the first
    // display mode with bit 5, codings 0 / 1 / 4 only, the palette's upper half
    // when the first display mode's bit 5 is set
    assert_eq!(
        tracks_of(&im),
        [
            "title A.EVO",
            "  0|video|vc1|0|0|",
            "  1|audio|ac3|1|0|",
            "  2|audio|truehd|2|1|",
            "  3|audio|mp2|3|2|",
            "  4|audio|pcm_dvd|4|3|",
            "  5|audio|pcm_dvd|5|4|",
            "  6|subtitle|dvd_subtitle|7|1||720x480|00108090",
            "  7|subtitle|dvd_subtitle|8|3||720x480|00108080",
            "  8|subtitle|dvd_subtitle|9|4||720x480|00108080",
        ]
    );
}

#[test]
fn an_audio_stream_of_coding_3_leaves_the_title_out() {
    let ok = attr_record([0x60, 0, 0x50], &[0x00], &[], &PALETTE);
    let bad = attr_record([0x60, 0, 0x50], &[0x00, 0x0c], &[], &PALETTE);
    let body = format!("<Title>{}</Title><Title>{}</Title>", clip("A.MAP", T, T, false), clip("B.MAP", T, T, false));
    let im = disc(&[evob("A.EVO", 1, 10), evob("B.EVO", 2, 10)], &[1, 2], &[ok, bad], playlist("", &body), vec![vec![], vec![]]);
    assert_eq!(tracks_of(&im), ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|ac3|1|0|"]);
}

#[test]
fn video_coding_and_sub_picture_frame_size() {
    let cases: [([u8; 3], &str, &str); 6] = [
        ([0x00, 0, 0x00], "mpeg2video", "352x240"),
        ([0x24, 0, 0x10], "mpeg2video", "352x576"),
        ([0x40, 0, 0x30], "h264", "544x480"),
        ([0x74, 0, 0x80], "vc1", "1280x720"),
        ([0x60, 0, 0xc0], "vc1", "1920x1080"),
        ([0x60, 0, 0x60], "vc1", "0x0"),
    ];
    let names = ["A.EVO", "B.EVO", "C.EVO", "D.EVO", "E.EVO", "F.EVO", "G.EVO"];
    let mut attrs: Vec<Vec<u8>> = cases.iter().map(|(v, _, _)| attr_record(*v, &[], &[[0, 0x20, 0, 0, 0]], &PALETTE)).collect();
    attrs.push(attr_record([0x80, 0, 0], &[], &[], &PALETTE)); // no video: the title is left out
    let evobs: Vec<Evob> = names.iter().enumerate().map(|(i, n)| evob(n, u16::try_from(i + 1).unwrap(), 10)).collect();
    let body = names.iter().fold(String::new(), |mut b, n| {
        let _ = write!(b, "<Title>{}</Title>", clip(&n.replace("EVO", "MAP"), T, T, false));
        b
    });
    let im = disc(&evobs, &[1, 2, 3, 4, 5, 6, 7], &attrs, playlist("", &body), vec![vec![]; 7]);
    let got = tracks_of(&im);
    let mut want = Vec::new();
    for (i, (_, codec, size)) in cases.iter().enumerate().rev() {
        want.push(format!("title {}", names[i]));
        want.push(format!("  0|video|{codec}|0|0|"));
        want.push(format!("  1|subtitle|dvd_subtitle|1|0||{size}|00108090"));
    }
    assert_eq!(got, want);
}

#[test]
fn a_stream_with_the_codec_and_number_of_an_earlier_one_is_left_out() {
    let attr = attr_record([0x60, 0, 0x50], &[0x00], &[[0, 0x21, 0, 0, 0], [0x20, 0x21, 0, 0, 0], [0, 0x22, 0, 0, 0]], &PALETTE);
    let im = disc(&[evob("A.EVO", 1, 10)], &[1], &[attr], one_title("A.MAP", ""), vec![vec![]]);
    assert_eq!(
        tracks_of(&im),
        [
            "title A.EVO",
            "  0|video|vc1|0|0|",
            "  1|audio|ac3|1|0|",
            "  2|subtitle|dvd_subtitle|2|1||720x480|00108090",
            "  3|subtitle|dvd_subtitle|4|2||720x480|00108090",
        ]
    );
}

#[test]
fn languages_come_from_the_playlists_track_navigation_list() {
    let attr = attr_record([0x60, 0, 0x50], &[0, 0, 0, 0], &[[0, 0x21, 0, 0, 0]], &PALETTE);
    let streams = r#"<Audio streamNumber="1" track="1"/><Audio streamNumber="2" track="2"/><Audio streamNumber="3" track="3"/><Audio streamNumber="4" track="4"/><Subtitle streamNumber="2" track="1"/>"#;
    let nav = r#"<TrackNavigationList><AudioTrack track="1" langcode="fr:0"/><AudioTrack track="2" langcode="xx:1"/><AudioTrack track="3" langcode="engl:1"/><AudioTrack track="4" langcode="de"/><SubtitleTrack track="1" langcode="DE:0"/></TrackNavigationList>"#;
    // the first Title has two TrackNavigationLists: not used; the second one gives
    // the languages; a langcode without ":" or with more than 3 characters before
    // it gives none
    let body = format!(
        "<Title>{c}{nav}{nav}</Title><Title>{c}{nav}</Title>",
        c = clip_with("A.MAP", streams)
    );
    let im = disc(&[evob("A.EVO", 1, 10)], &[1], &[attr], playlist("", &body), vec![vec![]]);
    assert_eq!(
        tracks_of(&im),
        [
            "title A.EVO",
            "  0|video|vc1|0|0|",
            "  1|audio|ac3|1|0|fre",
            "  2|audio|ac3|2|1|xx",
            "  3|audio|ac3|3|2|",
            "  4|audio|ac3|4|3|",
            "  5|subtitle|dvd_subtitle|5|1|ger|720x480|00108090",
        ]
    );
}

#[test]
fn the_tracks_come_from_the_clip_with_the_most_bytes() {
    let a = attr_record([0x00, 0, 0x50], &[0x00], &[], &PALETTE);
    let b = attr_record([0x60, 0, 0x50], &[0x04, 0x00], &[], &PALETTE);
    let body = format!("<Title>{}{}</Title>", clip("A.MAP", T, T, false), clip("B.MAP", T, T, true));
    let im = disc(
        &[evob("A.EVO", 1, 10), evob("B.EVO", 2, 10)],
        &[1, 2],
        &[a, b],
        playlist("", &body),
        vec![vec![], [pack(&[]), pack(&[])].concat()],
    );
    assert_eq!(
        tracks_of(&im),
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|truehd|1|0|", "  2|audio|ac3|2|1|"]
    );
}

// ---- the Dolby Digital Plus probe ----

/// One title over one EVOB with a Dolby Digital Plus stream 0 and `packs`.
fn ddplus(packs: &[Vec<u8>]) -> Vec<String> {
    let attr = attr_record([0x60, 0, 0x50], &[0x1c], &[], &PALETTE);
    let im = disc(&[evob("A.EVO", 1, 10)], &[1], &[attr], one_title("A.MAP", ""), vec![packs.concat()]);
    tracks_of(&im)
}

fn frames(f: &[Vec<u8>]) -> Vec<u8> {
    f.concat()
}

const EAC3: [&str; 3] = ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|eac3|1|0|"];

#[test]
fn four_e_ac3_frames_make_an_e_ac3_track_four_ac3_frames_an_ac3_track() {
    let e = || eac3_frame(256, 0, None);
    assert_eq!(ddplus(&[pack(&[ps1_packet(0xc0, Some(90000), &frames(&[e(), e(), e(), e()]))])]), EAC3);
    // frames run on into the next pack (its packet without a PTS)
    let a = ac3_frame;
    assert_eq!(
        ddplus(&[
            pack(&[ps1_packet(0xc0, Some(90000), &frames(&[a(), a(), a()]))]),
            pack(&[ps1_packet(0xc0, None, &a())]),
        ]),
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|ac3|1|0|"]
    );
    // data before the first frame start is skipped
    let mut data = vec![0u8; 100];
    data.extend(frames(&[e(), e(), e(), e()]));
    assert_eq!(ddplus(&[pack(&[ps1_packet(0xc0, Some(90000), &data)])]), EAC3);
}

#[test]
fn a_mix_of_ac3_and_e_ac3_frames_or_fewer_than_four_frames_leave_the_stream_out() {
    let e = || eac3_frame(256, 0, None);
    let video_only = ["title A.EVO", "  0|video|vc1|0|0|"];
    assert_eq!(ddplus(&[pack(&[ps1_packet(0xc0, Some(90000), &frames(&[e(), e(), ac3_frame(), e()]))])]), video_only);
    assert_eq!(ddplus(&[pack(&[ps1_packet(0xc0, Some(90000), &frames(&[e(), e(), e()]))])]), video_only);
}

#[test]
fn the_stream_needs_a_pts_packet_first_in_one_of_its_first_2000_packs() {
    let e = || eac3_frame(256, 0, None);
    let audio = pack(&[ps1_packet(0xc0, Some(90000), &frames(&[e(), e(), e(), e()]))]);
    let mut packs = vec![pack(&[video_packet(1000)]); 1999];
    packs.push(audio.clone());
    assert_eq!(ddplus(&packs), EAC3);
    // the same packet behind a video packet does not count; nor does a PTS packet in pack 2001
    let mut packs = vec![pack(&[video_packet(1000), ps1_packet(0xc0, Some(90000), &e())])];
    packs.extend(vec![pack(&[video_packet(1000)]); 1999]);
    packs.push(audio);
    assert_eq!(ddplus(&packs), Vec::<String>::new());
}

#[test]
fn the_first_frame_must_be_a_valid_header() {
    let e = || eac3_frame(256, 0, None);
    // a dependent stream frame whose channel map gives 1 channel for a 2/0 mode
    let bad = eac3_frame(256, 1, Some(0x8000));
    assert_eq!(ddplus(&[pack(&[ps1_packet(0xc0, Some(90000), &frames(&[bad, e(), e(), e()]))])]), Vec::<String>::new());
    let good = eac3_frame(256, 1, Some(0xa000)); // L, R
    assert_eq!(ddplus(&[pack(&[ps1_packet(0xc0, Some(90000), &frames(&[good, e(), e(), e()]))])]), EAC3);
}

#[test]
fn a_pack_with_more_than_two_packets_fails_the_probe() {
    let e = || eac3_frame(256, 0, None);
    assert_eq!(
        ddplus(&[
            pack(&[ps1_packet(0xc0, Some(90000), &frames(&[e(), e()]))]),
            pack(&[video_packet(500), ps1_packet(0xc0, None, &e()), ps1_packet(0xc0, None, &e())]),
        ]),
        Vec::<String>::new()
    );
}

// ---- the DTS probe ----

/// One title over one EVOB with a DTS stream 0 and `packs`.
fn dts(packs: &[Vec<u8>]) -> Vec<String> {
    let attr = attr_record([0x60, 0, 0x50], &[0x18], &[], &PALETTE);
    let im = disc(&[evob("A.EVO", 1, 10)], &[1], &[attr], one_title("A.MAP", ""), vec![packs.concat()]);
    tracks_of(&im)
}

/// A DTS core frame of `size` bytes (48 kHz, 3/2, 512 samples).
fn dts_core(size: usize) -> Vec<u8> {
    let mut f = vec![0u8; size];
    f[..4].copy_from_slice(&[0x7f, 0xfe, 0x80, 0x01]);
    let fsize = u32::try_from(size - 1).unwrap();
    let nblks = 15u32; // 16 blocks of 32 samples
    // bits from byte 4: FTYPE 1, SHORT 5 (31), CPF 0, NBLKS 7, FSIZE 14, AMODE 6, SFREQ 4, RATE 5, ...
    let v: u64 = (1u64 << 63) | (31u64 << 58) | (u64::from(nblks) << 50) | (u64::from(fsize) << 36) | (9u64 << 30) | (13u64 << 26) | (15u64 << 21);
    f[4..12].copy_from_slice(&v.to_be_bytes());
    f
}

/// A DTS-HD extension substream frame of `size` bytes: no static fields, one
/// asset of `asset` bytes (header 16 bytes).
fn dts_extss(size: usize, asset: usize) -> Vec<u8> {
    let mut f = vec![0u8; size];
    f[..4].copy_from_slice(&[0x64, 0x58, 0x20, 0x25]);
    // user 8, index 2, header size type 0, header size-1 8, frame size-1 16, static 0, asset size-1 16, descriptor size 9, index 3
    let mut bits: Vec<(u64, u32)> = vec![(0, 8), (0, 2), (0, 1), (15, 8), (u64::try_from(size - 1).unwrap(), 16), (0, 1), (u64::try_from(asset - 1).unwrap(), 16), (0, 9), (0, 3)];
    let mut acc: u128 = 0;
    let mut n = 0u32;
    for (v, w) in bits.drain(..) {
        acc = (acc << w) | u128::from(v);
        n += w;
    }
    acc <<= 128 - n;
    f[4..20].copy_from_slice(&acc.to_be_bytes());
    f
}

#[test]
fn a_dts_stream_is_dts_or_dts_hd_with_its_core_as_a_second_track() {
    let core = || dts_core(900);
    assert_eq!(
        dts(&[pack(&[ps1_packet(0x88, Some(90000), &[core(), core()].concat())])]),
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|dts|1|0|"]
    );
    // an extension substream after the first core frame: DTS-HD, then its core
    let unit = || [dts_core(600), dts_extss(200, 184)].concat();
    assert_eq!(
        dts(&[pack(&[ps1_packet(0x88, Some(90000), &[unit(), unit()].concat())])]),
        ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|dts|1|0||with core", "  2|audio|dts|1|0||core"]
    );
}

#[test]
fn a_dts_stream_without_a_frame_or_with_a_bad_first_header_leaves_the_title_out() {
    // no frame at all
    assert_eq!(dts(&[pack(&[ps1_packet(0x88, Some(90000), &[0u8; 100])])]), Vec::<String>::new());
    // the asset is larger than the substream frame
    let unit = || [dts_core(600), dts_extss(200, 300)].concat();
    assert_eq!(dts(&[pack(&[ps1_packet(0x88, Some(90000), &[unit(), unit()].concat())])]), Vec::<String>::new());
}

#[test]
fn dts_data_before_a_frame_start_is_dropped_up_to_the_first_access_unit() {
    // the packet starts with the end of a frame; its first access unit pointer
    // says where the next frame starts: those bytes are dropped, the frame is found
    let core = || dts_core(900);
    let mut p = ps1_packet(0x88, Some(90000), &[vec![0x55u8; 50], core(), core()].concat());
    let hdr = 9 + 5; // PES header with a PTS
    p[hdr + 2..hdr + 4].copy_from_slice(&51u16.to_be_bytes()); // first access unit at byte 50 of the payload
    assert_eq!(dts(&[pack(&[p])]), ["title A.EVO", "  0|video|vc1|0|0|", "  1|audio|dts|1|0|"]);
}
