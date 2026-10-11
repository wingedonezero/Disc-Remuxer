"""Blu-ray: the stream files against an AACS disc's content hash tables
(libavformat/bluray_hash.c): an encrypted disc built here (tests/synth/
bluray.py), read with its key, whose /AACS holds a content certificate and a
content hash table: the last 8 bytes of the SHA-1 of every 196,608-byte hash
unit of the stream file as it is on the disc (encrypted). A rip checks every
whole hash unit it reads; a damaged one is a warning with its file and
offset, and the title ends with one line."""

import hashlib
import json
import struct
import subprocess

from helpers import DATA, PROGRAM
from synth import bluray as bd
from synth.bluray import Item, Stream
from synth.hddvd import aes_e, hexs
from test_bluray_aacs import UK, VUK, sha1

M2V = (DATA / "testsrc_1200.m2v").read_bytes()
AC3 = (DATA / "sine_7680.ac3").read_bytes()
S, E = 27000000, 27000000 + 90000 * 4
HASH_UNIT = 0x30000
STREAMS = ((0x1011, 0x02, b"\x31"), (0x1100, 0x81, b"\x31eng"))


def encrypted_stream():
    packets = []
    for i, c in enumerate(M2V[k:k + 60000] for k in range(0, len(M2V), 60000)):
        packets += bd.ts_packets(0x1011, bd.pes(0xe0, c, pts=2 * S + 3003 * i))
    for i, c in enumerate(AC3[k:k + 768] for k in range(0, len(AC3), 768)):
        packets += bd.ts_packets(0x1100, bd.pes(0xfd, c, ext=0x71, pts=2 * S + 2880 * i))
    # null packets after them: a stream file of several hash units
    packets += bd.ts_packets(0x1fff, bytes(184 * 4600))
    plain = bd.source_units(packets)
    return b"".join(bd.encrypt_unit(plain[i:i + 6144], UK[0]) for i in range(0, len(plain), 6144))


def hash_files(stream, layers=1):
    """Content000.cer and ContentHash000.tbl for clip 1: one entry, the
    values of its whole hash units."""
    values = b"".join(hashlib.sha1(stream[i:i + HASH_UNIT]).digest()[-8:]
                      for i in range(0, len(stream) - HASH_UNIT + 1, HASH_UNIT))
    cer = bytearray(0x44)
    cer[6] = layers
    cer[8:12] = struct.pack(">I", len(values) // 8)
    cer[12:14] = struct.pack(">H", 1)
    return {"AACS/Content000.cer": bytes(cer),
            "AACS/ContentHash000.tbl": struct.pack(">III", 0, 1, 0) + values}, len(values) // 8


def disc(tmp_path, damage=None, with_table=True):
    stream = encrypted_stream()
    assert len(stream) >= 3 * HASH_UNIT, "the test needs a few hash units"
    uk = bd.unit_key_ro([aes_e(VUK, UK[0])])
    files = bd.bdmv({1: bd.clpi(packets=len(stream) // 192, stcs=((0, 0x1001, S, E),), streams=STREAMS)},
                    {1: bd.mpls([Item("00001", S, E, videos=[Stream(0x1011, 0x02, b"\x31")],
                                      audio=[Stream(0x1100, 0x81, b"\x31eng")])])})
    files["AACS/Unit_Key_RO.inf"] = uk
    files["AACS/MKB_RO.inf"] = bd.mkb()
    hashes, n = hash_files(stream)
    if with_table:
        files.update(hashes)
    if damage is not None:
        stream = stream[:damage] + bytes([stream[damage] ^ 0xff]) + stream[damage + 1:]
    files["BDMV/STREAM/00001.m2ts"] = stream
    path = bd.write(tmp_path / "disc", files)
    key = tmp_path / "keydb.cfg"
    key.write_text(f"0x{hexs(sha1(uk))} = Test disc | V | 0x{hexs(VUK)}\n")
    return path, key, n


def rip(tmp_path, path, key):
    p = subprocess.run([*PROGRAM, "--set", f"aacs.key_files={key}", "--json", "demux", str(path), "0",
                        str(tmp_path / "out")], capture_output=True, text=True)
    msgs = [json.loads(l) for l in p.stdout.splitlines() if l.startswith('{"type":"message"')]
    return p.returncode, [(m["level"], m["text"]) for m in msgs]


def test_an_undamaged_disc_checks_every_whole_hash_unit_read(tmp_path):
    path, key, n = disc(tmp_path)
    code, msgs = rip(tmp_path, path, key)
    assert code == 0, msgs
    texts = [t for _, t in msgs]
    assert any("content hash table loaded (1 layer, SHA-1)" in t for t in texts), texts
    line = [t for t in texts if "content hash check:" in t]
    assert line and f"{n} blocks" in line[0] and "all correct" in line[0], line


def test_a_damaged_byte_is_found_in_its_hash_unit(tmp_path):
    path, key, n = disc(tmp_path, damage=HASH_UNIT + 5000)
    code, msgs = rip(tmp_path, path, key)
    warn = [t for lv, t in msgs if lv == "warning"]
    assert any(f"00001.m2ts: the 196608 bytes at offset {HASH_UNIT} do not match the disc's content hash: damaged"
               in t for t in warn), warn
    assert any(f"content hash check: 1 of {n} blocks checked are damaged, in 1 stream file" in t for t in warn), warn
    done = [t for _, t in msgs if t.startswith("Title #0: done")]
    assert done and "warning" in done[0], done


def test_without_a_table_nothing_is_checked_and_the_log_says_so(tmp_path):
    path, key, _ = disc(tmp_path, with_table=False)
    code, msgs = rip(tmp_path, path, key)
    texts = [t for _, t in msgs]
    assert code == 0, msgs
    assert any("no content hash table" in t for t in texts), texts
    assert not [t for t in texts if "content hash check:" in t]
