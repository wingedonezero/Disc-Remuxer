"""AC-3 (ATSC A/52) syncframe checksums, so built frames pass the CRC
checks of the disc frame rules: crc1 (bytes 2..3) makes the CRC of bytes
2 .. 5/8 of the frame 0, crc2 (the last two bytes) that of the rest."""


def _crc(data):
    """CRC-16, polynomial 0x8005, initial 0, most significant bit first."""
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x8005) & 0xffff if crc & 0x8000 else (crc << 1) & 0xffff
    return crc


def with_crcs(frame):
    """The AC-3 syncframe with its crc1 and crc2 set."""
    f = bytearray(frame)
    size = len(f)
    n58 = size * 5 >> 3
    # crc2: the CRC of the bytes before it, appended
    f[size - 2:size] = _crc(bytes(f[n58:size - 2])).to_bytes(2, "big")
    # crc1: the CRC is linear in the two bytes at 2..3 (the rest fixed); find
    # the value that makes the CRC of bytes 2 .. n58 zero
    f[2:4] = b"\0\0"
    target = _crc(bytes(f[2:n58]))
    basis = []
    for i in range(16):
        g = bytearray(n58 - 2)
        g[0:2] = (1 << i).to_bytes(2, "big")
        basis.append((_crc(bytes(g)), 1 << i))
    value, rows = 0, list(basis)
    want = target
    # Gaussian elimination over GF(2)
    for bit in range(15, -1, -1):
        piv = next((r for r in rows if r[0] >> bit & 1), None)
        if piv is None:
            continue
        rows.remove(piv)
        rows = [((r[0] ^ piv[0], r[1] ^ piv[1]) if r[0] >> bit & 1 else r) for r in rows]
        if want >> bit & 1:
            want ^= piv[0]
            value ^= piv[1]
    assert want == 0
    f[2:4] = value.to_bytes(2, "big")
    assert _crc(bytes(f[2:n58])) == 0 and _crc(bytes(f[n58:size])) == 0
    return bytes(f)
