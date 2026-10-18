"""Damaged Blu-ray stream data in the standard log (src/mux/ts_disc.c
through the clip demuxer's gaps mode; title_reader.c; bluray_part.c
block_place): each stretch of a stream file left out is one warning with
the file and its bytes - a run of TS packets that cannot be read (one
reason), a run of all-zero source packets, a PID's data from a PES packet
that cannot be read to its next PES start - and the title's damage summary
counts them. A disc built byte by byte (tests/synth/bluray.py)."""

import json
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
S, E = 27000000, 27000000 + 45000 * 4
V = 0x1011


def disc(tmp_path, damage):
    """The video clip with damage(packets) applied to its TS packets."""
    packets = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        packets += bd.ts_packets(V, bd.pes(0xe0, c, pts=2 * S + 3003 * i))
    data = damage(bd.continuity(packets))
    clips = {1: bd.clpi(packets=len(data) // 192, stcs=((0, 0x1001, S, E),), streams=((V, 0x02, b"\x31"),))}
    item = Item("00001", S, E, videos=[Stream(V, 0x02, b"\x31")])
    files = bd.bdmv(clips, {1: bd.mpls([item], marks=((1, 0, S), (1, 0, S + 90000)))})
    files["BDMV/STREAM/00001.m2ts"] = data
    return bd.write(tmp_path / "disc", files)


def rip(tmp_path, disc_dir):
    p = subprocess.run([*PROGRAM, "--json", "demux", str(disc_dir), "0", str(tmp_path / "out")],
                       capture_output=True, text=True)
    recs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith("{")]
    texts = [r.get("text", "") for r in recs if r.get("type") == "message"]
    result = [r for r in recs if r.get("type") == "title_result"]
    return texts, result[0] if result else None


def units(src_packets):
    """Source packets (192 bytes each, already with their header) padded to whole units."""
    out = b"".join(src_packets)
    n = len(src_packets)
    while n % 32:
        out += bd.be32(n * 0x10) + bd.ts_packet(0x1fff, bytes(184))
        n += 1
    return out


def test_zero_and_unreadable_packets_are_each_one_line_with_their_bytes(tmp_path):
    def damage(packets):
        src = [bd.be32(i * 0x10) + p for i, p in enumerate(packets)]
        # source packets 100-139 zeroed, 140-141 without a sync byte, 142 with the transport error bit
        for k in range(100, 140):
            src[k] = bytes(192)
        for k in (140, 141):
            src[k] = src[k][:4] + b"\x00" + src[k][5:]
        src[142] = src[142][:5] + bytes([src[142][5] | 0x80]) + src[142][6:]
        return units(src)
    texts, result = rip(tmp_path, disc(tmp_path, damage))
    assert result and result["status"] != "failed", texts
    log = "\n".join(texts)
    # none of the three can be put on a stream: the zeros are more than the counter's cycle (16), the next two
    # are followed by another bad packet
    unknown = "which stream they belonged to is not known (the continuity counters around them do not tell)"
    assert ("Segment /BDMV/STREAM/00001.m2ts: 7.5 KB of zeros at bytes 19200-26879 (1.25 units, blank or unreadable "
            "sectors): skipped; " + unknown) in log, log
    assert ("Segment /BDMV/STREAM/00001.m2ts: 384 bytes at bytes 26880-27263 are damaged (2 transport packets: no "
            "sync byte; the stream file itself holds these bytes: a damaged or badly copied file): left out; "
            + unknown) in log, log
    assert ("Segment /BDMV/STREAM/00001.m2ts: 192 bytes at bytes 27264-27455 are damaged (1 transport packet: "
            "transport_error_indicator set; the stream file itself holds these bytes: a damaged or badly copied "
            "file): left out; " + unknown) in log, log
    assert "Damaged: 3 stretches of the stream files left out, 8.1 KB in all (each said above):" in log, log
    assert "  - 00001.m2ts bytes 26880-27263 (384 bytes)" in texts, texts
    assert result["damaged_ranges"] == 3 and result["damaged_bytes"] == 7680 + 3 * 192, result


def test_a_pes_packet_that_cannot_be_read_drops_its_pid_to_the_next_pes_start(tmp_path):
    def damage(packets):
        # a PES start without its start code after the first PES packet (327 TS packets of 60000 bytes)
        k = next(i for i in range(1, len(packets)) if packets[i][1] & 0x40)
        junk = bd.ts_packet(V, b"\x00\x00\x02" + bytes(181), pusi=True)
        return bd.source_units(packets[:k] + [junk] + packets[k:])
    texts, result = rip(tmp_path, disc(tmp_path, damage))
    log = "\n".join(texts)
    assert result and result["status"] != "failed", log
    lines = [t for t in texts if t.startswith("Segment") and "a PES packet without its start code" in t]
    assert len(lines) == 1, log
    assert lines[0].startswith("Segment /BDMV/STREAM/00001.m2ts: PID 0x1011 (track 0): a PES packet without its "
                               "start code at byte "), lines
    assert ": 184 bytes left out until the next PES start at byte " in lines[0], lines
    assert result["damaged_ranges"] == 1 and result["damaged_bytes"] == 184, result


def test_a_transport_packet_missing_from_the_stream_file_is_said_and_counted(tmp_path):
    def damage(packets):
        # packet 50 (inside the first PES packet) and the duplicate of packet 80 left in
        kept = packets[:50] + packets[51:81] + [packets[80]] + packets[81:]
        return units([bd.be32(i * 0x10) + p for i, p in enumerate(kept)])
    texts, result = rip(tmp_path, disc(tmp_path, damage))
    assert result and result["status"] != "failed", texts
    log = "\n".join(texts)
    assert ("Segment /BDMV/STREAM/00001.m2ts: PID 0x1011 (track 0): the continuity counter jumps from 1 to 3 at "
            "byte 9600: 1 transport packet(s) missing before it (counted modulo 16); the data around them is "
            "kept") in log, log
    assert ("Segment /BDMV/STREAM/00001.m2ts: PID 0x1011 (track 0): a duplicate transport packet at byte 15360 "
            "(the packet before it again, byte for byte): left out") in log, log
    assert "Damaged: 1 stretch of the stream files left out" in log, log
    assert result["damaged_ranges"] == 1 and result["damaged_bytes"] == 0, result


def test_damage_that_takes_no_picture_makes_no_hole_for_the_mkvmerge_mode(tmp_path):
    # 5 source packets in the middle of a picture's data replaced by bytes that are no stream data: the
    # picture is shorter, none is missing, so the mkvmerge mode uses no timestamp files (a stand-in
    # mkvmerge: it only makes the output file)
    import os
    def damage(packets):
        src = [bd.be32(i * 0x10) + p for i, p in enumerate(packets)]
        for k in range(60, 65):
            src[k] = bytes((k * 37 + i) % 251 + 1 for i in range(192))
        return units(src)
    fake = tmp_path / "fake-mkvmerge"
    fake.write_text('#!/bin/sh\nout=$(python3 -c "import json,sys; a=json.load(open(sys.argv[1][1:])); '
                    'print(a[a.index(\'--output\')+1])" "$1")\n: > "$out"\n')
    os.chmod(fake, 0o755)
    p = subprocess.run([*PROGRAM, "--set", "output.format=mkvmerge", "--set", f"mkv.mkvmerge_path={fake}", "demux",
                        str(disc(tmp_path, damage)), "0", str(tmp_path / "out")], capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    assert ("mkvmerge: damaged data was left out, but no picture is missing and no audio gap is left open: no "
            "hole, no timestamp files, the delays as --sync") in p.stdout, p.stdout
    # the counter of the video's next packet jumps over exactly the 5: they were the video's, in the PES packet
    # of the first chunk (its time: the file's first picture, so 0 into the file)
    assert ("Segment /BDMV/STREAM/00001.m2ts: 960 bytes at bytes 11520-12479 are damaged (5 transport packets: no "
            "sync byte; the stream file itself holds these bytes: a damaged or badly copied file): left out; they "
            "were PID 0x1011's (track 0; its continuity counter goes from 11 to 1 over them): the picture they belong "
            "to is incomplete (it may show a glitch until the next key frame)") in p.stdout, p.stdout
    assert "  - 00001.m2ts bytes 11520-12479 (960 bytes, track 0), 0:00:00.000 into the file" in p.stdout, p.stdout


def test_a_damaged_stretch_gets_the_time_of_its_stream_s_pes_packet(tmp_path):
    # 3 bad packets inside the second chunk's PES packet (PTS one picture after the first, 3003 ticks):
    # 33.367 ms into the file
    def damage(packets):
        src = [bd.be32(i * 0x10) + p for i, p in enumerate(packets)]
        k = next(i for i in range(1, len(packets)) if packets[i][1] & 0x40) + 20
        for j in range(k, k + 3):
            src[j] = bytes((j * 31 + i) % 251 + 1 for i in range(192))
        return units(src)
    texts, result = rip(tmp_path, disc(tmp_path, damage))
    assert result and result["status"] != "failed", texts
    lines = [t for t in texts if t.startswith("  - 00001.m2ts")]
    assert len(lines) == 1 and lines[0].endswith("(576 bytes, track 0), 0:00:00.033 into the file"), texts
