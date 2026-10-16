/*
 * SHA-1 and SHA-256, written from FIPS 180-4 (see sha.h).
 */

#include <string.h>

#include "base/bytes.h"
#include "base/sha.h"

static const uint32_t k256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
static uint32_t ror(uint32_t x, int n) { return x >> n | x << (32 - n); }

static void block1(DrSha *s)
{
    uint32_t w[80], a, b, c, d, e;

    for (int i = 0; i < 16; i++)
        w[i] = dr_rb32(s->block + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void block256(DrSha *s)
{
    uint32_t w[64], v[8];

    for (int i = 0; i < 16; i++)
        w[i] = dr_rb32(s->block + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(v, s->h, sizeof(v));
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + k256[i] + w[i];
        uint32_t s0 = ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22);
        uint32_t mj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + mj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++)
        s->h[i] += v[i];
}

void dr_sha_init(DrSha *s, int bits)
{
    static const uint32_t h1[5]   = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    static const uint32_t h256[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

    memset(s, 0, sizeof(*s));
    s->bits = bits == 160 ? 160 : 256;
    memcpy(s->h, s->bits == 160 ? h1 : h256, s->bits == 160 ? sizeof(h1) : sizeof(h256));
}

void dr_sha_update(DrSha *s, const uint8_t *data, size_t len)
{
    size_t fill = s->len % 64;

    s->len += len;
    while (len) {
        size_t n = 64 - fill < len ? 64 - fill : len;
        memcpy(s->block + fill, data, n);
        data += n;
        len  -= n;
        fill += n;
        if (fill == 64) {
            if (s->bits == 160)
                block1(s);
            else
                block256(s);
            fill = 0;
        }
    }
}

void dr_sha_final(DrSha *s, uint8_t *out)
{
    uint64_t total = s->len * 8;
    uint8_t pad[72] = { 0x80 };
    size_t fill = s->len % 64;
    size_t n = (fill < 56 ? 56 : 120) - fill;

    dr_sha_update(s, pad, n);
    for (int i = 0; i < 8; i++)
        pad[i] = (uint8_t)(total >> (56 - 8 * i));
    dr_sha_update(s, pad, 8);
    for (int i = 0; i < (s->bits == 160 ? 5 : 8); i++) {
        out[4 * i + 0] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)(s->h[i]);
    }
}
