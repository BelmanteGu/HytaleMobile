// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// CommonCrypto subset: MD5, SHA-1, SHA-2, HMAC, PBKDF2, AES and random bytes.
// Digest states live inside the caller's Darwin context structures with no
// pointers, because callers copy those structures to compute running hashes.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#include "macho.h"

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put_be64(uint8_t *p, uint64_t v) { put_be32(p, v >> 32); put_be32(p + 4, (uint32_t)v); }
static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// ---------------------------------------------------------------------------
// Block-hash framework. Each state is: chaining values, a 64-bit byte count and
// one block buffer. Sizes fit Darwin's CC_*_CTX (MD5 92, SHA1 96, SHA256 104, SHA512 208).

struct md5_state { uint32_t h[4]; uint64_t count; uint8_t buf[64]; };
struct sha1_state { uint32_t h[5]; uint64_t count; uint8_t buf[64]; };
struct sha256_state { uint32_t h[8]; uint64_t count; uint8_t buf[64]; };
struct sha512_state { uint64_t h[8]; uint64_t count; uint8_t buf[128]; };

_Static_assert(sizeof(struct md5_state) <= 92, "CC_MD5_CTX");
_Static_assert(sizeof(struct sha1_state) <= 96, "CC_SHA1_CTX");
_Static_assert(sizeof(struct sha256_state) <= 104, "CC_SHA256_CTX");
_Static_assert(sizeof(struct sha512_state) <= 208, "CC_SHA512_CTX");

static void md5_block(uint32_t *h, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    };
    static const uint8_t r[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
        6, 10, 15, 21, 6, 10, 15, 21,
    };
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = le32(p + 4 * i);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + ROTL32(a + f + k[i] + w[g], r[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static void sha1_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 80; i++)
        w[i] = ROTL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = ROTL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROTL32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

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

static void sha256_block(uint32_t *h, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = be32(p + 4 * i);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(w[i - 15], 7) ^ ROTR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROTR32(w[i - 2], 17) ^ ROTR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25)) + ((e & f) ^ (~e & g)) + k256[i] + w[i];
        uint32_t t2 = (ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

static const uint64_t k512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static void sha512_block(uint64_t *h, const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = be64(p + 8 * i);
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROTR64(w[i - 15], 1) ^ ROTR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROTR64(w[i - 2], 19) ^ ROTR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = hh + (ROTR64(e, 14) ^ ROTR64(e, 18) ^ ROTR64(e, 41)) + ((e & f) ^ (~e & g)) + k512[i] + w[i];
        uint64_t t2 = (ROTR64(a, 28) ^ ROTR64(a, 34) ^ ROTR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

// Generic buffered update over a state laid out as {h, count, buf}.
#define DEFINE_UPDATE(name, type, block_size, block_fn)                                        \
    static void name(type *s, const void *data, size_t len)                                    \
    {                                                                                          \
        const uint8_t *p = data;                                                               \
        size_t used = (size_t)(s->count % (block_size));                                       \
        s->count += len;                                                                       \
        if (used) {                                                                            \
            size_t take = (block_size) - used < len ? (block_size) - used : len;               \
            memcpy(s->buf + used, p, take);                                                    \
            p += take;                                                                         \
            len -= take;                                                                       \
            if (used + take < (block_size))                                                    \
                return;                                                                        \
            block_fn(s->h, s->buf);                                                            \
        }                                                                                      \
        for (; len >= (block_size); p += (block_size), len -= (block_size))                    \
            block_fn(s->h, p);                                                                 \
        memcpy(s->buf, p, len);                                                                \
    }

DEFINE_UPDATE(md5_update, struct md5_state, 64, md5_block)
DEFINE_UPDATE(sha1_update, struct sha1_state, 64, sha1_block)
DEFINE_UPDATE(sha256_update, struct sha256_state, 64, sha256_block)
DEFINE_UPDATE(sha512_update, struct sha512_state, 128, sha512_block)

static void md5_init(struct md5_state *s) { *s = (struct md5_state){ { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, 0, { 0 } }; }
static void sha1_init(struct sha1_state *s)
{
    *s = (struct sha1_state){ { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 }, 0, { 0 } };
}
static void sha224_init(struct sha256_state *s)
{
    *s = (struct sha256_state){ { 0xc1059ed8, 0x367cd507, 0x3070dd17, 0xf70e5939, 0xffc00b31, 0x68581511,
                                  0x64f98fa7, 0xbefa4fa4 }, 0, { 0 } };
}
static void sha256_init(struct sha256_state *s)
{
    *s = (struct sha256_state){ { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c,
                                  0x1f83d9ab, 0x5be0cd19 }, 0, { 0 } };
}
static void sha384_init(struct sha512_state *s)
{
    *s = (struct sha512_state){ { 0xcbbb9d5dc1059ed8ull, 0x629a292a367cd507ull, 0x9159015a3070dd17ull,
                                  0x152fecd8f70e5939ull, 0x67332667ffc00b31ull, 0x8eb44a8768581511ull,
                                  0xdb0c2e0d64f98fa7ull, 0x47b5481dbefa4fa4ull }, 0, { 0 } };
}
static void sha512_init(struct sha512_state *s)
{
    *s = (struct sha512_state){ { 0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                                  0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                                  0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull }, 0, { 0 } };
}

static void md5_final(struct md5_state *s, uint8_t *out)
{
    uint64_t bits = s->count * 8;
    uint8_t pad[72] = { 0x80 };
    size_t used = (size_t)(s->count % 64);
    size_t padlen = used < 56 ? 56 - used : 120 - used;
    uint8_t len[8];
    for (int i = 0; i < 8; i++)
        len[i] = (uint8_t)(bits >> (8 * i));
    md5_update(s, pad, padlen);
    md5_update(s, len, 8);
    for (int i = 0; i < 4; i++)
        put_le32(out + 4 * i, s->h[i]);
}

static void sha1_final(struct sha1_state *s, uint8_t *out)
{
    uint64_t bits = s->count * 8;
    uint8_t pad[72] = { 0x80 }, len[8];
    size_t used = (size_t)(s->count % 64);
    put_be64(len, bits);
    sha1_update(s, pad, used < 56 ? 56 - used : 120 - used);
    sha1_update(s, len, 8);
    for (int i = 0; i < 5; i++)
        put_be32(out + 4 * i, s->h[i]);
}

static void sha256_final_n(struct sha256_state *s, uint8_t *out, int words)
{
    uint64_t bits = s->count * 8;
    uint8_t pad[72] = { 0x80 }, len[8];
    size_t used = (size_t)(s->count % 64);
    put_be64(len, bits);
    sha256_update(s, pad, used < 56 ? 56 - used : 120 - used);
    sha256_update(s, len, 8);
    for (int i = 0; i < words; i++)
        put_be32(out + 4 * i, s->h[i]);
}

static void sha512_final_n(struct sha512_state *s, uint8_t *out, int words)
{
    uint64_t bits = s->count * 8;
    uint8_t pad[136] = { 0x80 }, len[16] = { 0 };
    size_t used = (size_t)(s->count % 128);
    put_be64(len + 8, bits);
    sha512_update(s, pad, used < 112 ? 112 - used : 240 - used);
    sha512_update(s, len, 16);
    for (int i = 0; i < words; i++)
        put_be64(out + 8 * i, s->h[i]);
}

// ---------------------------------------------------------------------------
// Darwin digest API

static int cc_md5_init(struct md5_state *c) { md5_init(c); return 1; }
static int cc_md5_update(struct md5_state *c, const void *d, uint32_t n) { md5_update(c, d, n); return 1; }
static int cc_md5_final(uint8_t *md, struct md5_state *c) { md5_final(c, md); return 1; }
static uint8_t *cc_md5(const void *d, uint32_t n, uint8_t *md) { struct md5_state c; md5_init(&c); md5_update(&c, d, n); md5_final(&c, md); return md; }

static int cc_sha1_init(struct sha1_state *c) { sha1_init(c); return 1; }
static int cc_sha1_update(struct sha1_state *c, const void *d, uint32_t n) { sha1_update(c, d, n); return 1; }
static int cc_sha1_final(uint8_t *md, struct sha1_state *c) { sha1_final(c, md); return 1; }
static uint8_t *cc_sha1(const void *d, uint32_t n, uint8_t *md) { struct sha1_state c; sha1_init(&c); sha1_update(&c, d, n); sha1_final(&c, md); return md; }

static int cc_sha256_init(struct sha256_state *c) { sha256_init(c); return 1; }
static int cc_sha256_update(struct sha256_state *c, const void *d, uint32_t n) { sha256_update(c, d, n); return 1; }
static int cc_sha256_final(uint8_t *md, struct sha256_state *c) { sha256_final_n(c, md, 8); return 1; }
static uint8_t *cc_sha256(const void *d, uint32_t n, uint8_t *md) { struct sha256_state c; sha256_init(&c); sha256_update(&c, d, n); sha256_final_n(&c, md, 8); return md; }

static int cc_sha384_init(struct sha512_state *c) { sha384_init(c); return 1; }
static int cc_sha384_update(struct sha512_state *c, const void *d, uint32_t n) { sha512_update(c, d, n); return 1; }
static int cc_sha384_final(uint8_t *md, struct sha512_state *c) { sha512_final_n(c, md, 6); return 1; }
static uint8_t *cc_sha384(const void *d, uint32_t n, uint8_t *md) { struct sha512_state c; sha384_init(&c); sha512_update(&c, d, n); sha512_final_n(&c, md, 6); return md; }

static int cc_sha512_init(struct sha512_state *c) { sha512_init(c); return 1; }
static int cc_sha512_update(struct sha512_state *c, const void *d, uint32_t n) { sha512_update(c, d, n); return 1; }
static int cc_sha512_final(uint8_t *md, struct sha512_state *c) { sha512_final_n(c, md, 8); return 1; }
static uint8_t *cc_sha512(const void *d, uint32_t n, uint8_t *md) { struct sha512_state c; sha512_init(&c); sha512_update(&c, d, n); sha512_final_n(&c, md, 8); return md; }

// ---------------------------------------------------------------------------
// HMAC. CCHmacContext is 384 bytes: algorithm, inner state, outer key pad.

enum { HMAC_SHA1, HMAC_MD5, HMAC_SHA256, HMAC_SHA384, HMAC_SHA512, HMAC_SHA224 };

struct hmac_state {
    uint32_t alg;
    union {
        struct md5_state md5;
        struct sha1_state sha1;
        struct sha256_state sha256;
        struct sha512_state sha512;
    } inner;
    uint8_t opad[128];
};

_Static_assert(sizeof(struct hmac_state) <= 384, "CCHmacContext");

static size_t hmac_block(uint32_t alg) { return alg == HMAC_SHA384 || alg == HMAC_SHA512 ? 128 : 64; }

static size_t digest_len(uint32_t alg)
{
    switch (alg) {
    case HMAC_MD5: return 16;
    case HMAC_SHA1: return 20;
    case HMAC_SHA224: return 28;
    case HMAC_SHA256: return 32;
    case HMAC_SHA384: return 48;
    default: return 64;
    }
}

static void digest_init(struct hmac_state *h)
{
    switch (h->alg) {
    case HMAC_MD5: md5_init(&h->inner.md5); break;
    case HMAC_SHA1: sha1_init(&h->inner.sha1); break;
    case HMAC_SHA224: sha224_init(&h->inner.sha256); break;
    case HMAC_SHA256: sha256_init(&h->inner.sha256); break;
    case HMAC_SHA384: sha384_init(&h->inner.sha512); break;
    default: sha512_init(&h->inner.sha512); break;
    }
}

static void digest_update(struct hmac_state *h, const void *d, size_t n)
{
    switch (h->alg) {
    case HMAC_MD5: md5_update(&h->inner.md5, d, n); break;
    case HMAC_SHA1: sha1_update(&h->inner.sha1, d, n); break;
    case HMAC_SHA224: case HMAC_SHA256: sha256_update(&h->inner.sha256, d, n); break;
    default: sha512_update(&h->inner.sha512, d, n); break;
    }
}

static void digest_final(struct hmac_state *h, uint8_t *out)
{
    switch (h->alg) {
    case HMAC_MD5: md5_final(&h->inner.md5, out); break;
    case HMAC_SHA1: sha1_final(&h->inner.sha1, out); break;
    case HMAC_SHA224: sha256_final_n(&h->inner.sha256, out, 7); break;
    case HMAC_SHA256: sha256_final_n(&h->inner.sha256, out, 8); break;
    case HMAC_SHA384: sha512_final_n(&h->inner.sha512, out, 6); break;
    default: sha512_final_n(&h->inner.sha512, out, 8); break;
    }
}

static void cc_hmac_init(struct hmac_state *h, uint32_t alg, const void *key, size_t key_len)
{
    uint8_t k[128] = { 0 }, ipad[128];
    size_t block = hmac_block(alg);
    h->alg = alg;
    if (key_len > block) {
        digest_init(h);
        digest_update(h, key, key_len);
        digest_final(h, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (size_t i = 0; i < block; i++) {
        ipad[i] = k[i] ^ 0x36;
        h->opad[i] = k[i] ^ 0x5c;
    }
    digest_init(h);
    digest_update(h, ipad, block);
}

static void cc_hmac_update(struct hmac_state *h, const void *d, size_t n) { digest_update(h, d, n); }

static void cc_hmac_final(struct hmac_state *h, void *mac)
{
    uint8_t inner[64];
    digest_final(h, inner);
    digest_init(h);
    digest_update(h, h->opad, hmac_block(h->alg));
    digest_update(h, inner, digest_len(h->alg));
    digest_final(h, mac);
}

static void cc_hmac(uint32_t alg, const void *key, size_t key_len, const void *d, size_t n, void *mac)
{
    struct hmac_state h;
    cc_hmac_init(&h, alg, key, key_len);
    cc_hmac_update(&h, d, n);
    cc_hmac_final(&h, mac);
}

// CCKeyDerivationPBKDF(kCCPBKDF2, password, len, salt, len, prf, rounds, derived, len)
// prf: kCCPRFHmacAlgSHA1 1, SHA224 2, SHA256 3, SHA384 4, SHA512 5.
static int cc_pbkdf(uint32_t algorithm, const char *password, size_t password_len, const uint8_t *salt,
                    size_t salt_len, uint32_t prf, unsigned rounds, uint8_t *derived, size_t derived_len)
{
    static const uint32_t prf_to_hmac[] = { 0, HMAC_SHA1, HMAC_SHA224, HMAC_SHA256,
                                            HMAC_SHA384, HMAC_SHA512 };
    if (algorithm != 2 || prf < 1 || prf > 5 || rounds == 0)
        return -4300; // CC_PARAM_ERROR
    uint32_t alg = prf_to_hmac[prf];
    size_t hlen = digest_len(alg);
    for (uint32_t block = 1; derived_len; block++) {
        uint8_t counter[4], u[64], t[64];
        put_be32(counter, block);
        struct hmac_state h;
        cc_hmac_init(&h, alg, password, password_len);
        cc_hmac_update(&h, salt, salt_len);
        cc_hmac_update(&h, counter, 4);
        cc_hmac_final(&h, u);
        memcpy(t, u, hlen);
        for (unsigned r = 1; r < rounds; r++) {
            cc_hmac(alg, password, password_len, u, hlen, u);
            for (size_t i = 0; i < hlen; i++)
                t[i] ^= u[i];
        }
        size_t take = derived_len < hlen ? derived_len : hlen;
        memcpy(derived, t, take);
        derived += take;
        derived_len -= take;
    }
    return 0;
}

static int cc_random_generate_bytes(void *bytes, size_t count)
{
    uint8_t *p = bytes;
    while (count) {
        ssize_t r = getrandom(p, count, 0);
        if (r <= 0)
            return -4302; // kCCRNGFailure
        p += r;
        count -= (size_t)r;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// AES (FIPS-197), table-free byte implementation.

static const uint8_t sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t inv_sbox[256];

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }
static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    while (b) {
        if (b & 1)
            r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

struct aes_key {
    int rounds;
    uint8_t rk[240];
};

static void aes_expand(struct aes_key *k, const uint8_t *key, size_t len)
{
    int nk = (int)len / 4;
    k->rounds = nk + 6;
    int total = 4 * (k->rounds + 1);
    memcpy(k->rk, key, len);
    uint8_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint8_t t[4];
        memcpy(t, k->rk + 4 * (i - 1), 4);
        if (i % nk == 0) {
            uint8_t first = t[0];
            t[0] = sbox[t[1]] ^ rcon;
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[first];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            for (int j = 0; j < 4; j++)
                t[j] = sbox[t[j]];
        }
        for (int j = 0; j < 4; j++)
            k->rk[4 * i + j] = k->rk[4 * (i - nk) + j] ^ t[j];
    }
}

static void aes_encrypt_block(const struct aes_key *k, const uint8_t *in, uint8_t *out)
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ k->rk[i];
    for (int round = 1; round <= k->rounds; round++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)
            t[i] = sbox[s[(i + 4 * (i % 4)) % 16]]; // SubBytes + ShiftRows
        if (round != k->rounds) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                uint8_t all = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= all ^ xtime(a0 ^ a1);
                col[1] ^= all ^ xtime(a1 ^ a2);
                col[2] ^= all ^ xtime(a2 ^ a3);
                col[3] ^= all ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ k->rk[16 * round + i];
    }
    memcpy(out, s, 16);
}

static void aes_decrypt_block(const struct aes_key *k, const uint8_t *in, uint8_t *out)
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ k->rk[16 * k->rounds + i];
    for (int round = k->rounds - 1; round >= 0; round--) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++)
            t[(i + 4 * (i % 4)) % 16] = inv_sbox[s[i]]; // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; i++)
            t[i] ^= k->rk[16 * round + i];
        if (round != 0) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = t + 4 * c, a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                col[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                col[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                col[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

// ---------------------------------------------------------------------------
// DES and 3DES (EDE), bit-serial from FIPS 46-3. Only used for the small PBES2 payloads .NET decrypts.

static const uint8_t des_ip[64] = {58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7};
static const uint8_t des_fp[64] = {40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25};
static const uint8_t des_e[48] = {32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1};
static const uint8_t des_p[32] = {16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25};
static const uint8_t des_pc1[56] = {57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,
    63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4};
static const uint8_t des_pc2[48] = {14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32};
static const uint8_t des_shifts[16] = {1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t des_s[8][64] = {
    {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
    {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
    {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
    {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
    {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
    {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
    {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
    {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11},
};

static uint64_t des_perm(uint64_t in, const uint8_t *t, int n, int in_bits)
{
    uint64_t out = 0;
    for (int i = 0; i < n; i++)
        out = (out << 1) | ((in >> (in_bits - t[i])) & 1);
    return out;
}

static uint64_t des_load(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}

static void des_schedule(uint64_t ks[16], const uint8_t *key)
{
    uint64_t k = des_perm(des_load(key), des_pc1, 56, 64);
    uint32_t c = (uint32_t)(k >> 28) & 0xfffffff, d = (uint32_t)k & 0xfffffff;
    for (int i = 0; i < 16; i++) {
        for (int s = 0; s < des_shifts[i]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xfffffff;
            d = ((d << 1) | (d >> 27)) & 0xfffffff;
        }
        ks[i] = des_perm(((uint64_t)c << 28) | d, des_pc2, 48, 56);
    }
}

static uint64_t des_crypt(const uint64_t ks[16], uint64_t block, int decrypt)
{
    uint64_t ip = des_perm(block, des_ip, 64, 64);
    uint32_t l = (uint32_t)(ip >> 32), r = (uint32_t)ip;
    for (int i = 0; i < 16; i++) {
        uint64_t x = des_perm(r, des_e, 48, 32) ^ ks[decrypt ? 15 - i : i];
        uint32_t f = 0;
        for (int j = 0; j < 8; j++) {
            unsigned six = (unsigned)(x >> (42 - 6 * j)) & 63;
            f = (f << 4) | des_s[j][(((six >> 4) & 2) | (six & 1)) * 16 + ((six >> 1) & 15)];
        }
        uint32_t t = l ^ (uint32_t)des_perm(f, des_p, 32, 32);
        l = r;
        r = t;
    }
    return des_perm(((uint64_t)r << 32) | l, des_fp, 64, 64);
}

static void des3_block(const uint64_t ks[3][16], int encrypt, const uint8_t *in, uint8_t *out)
{
    uint64_t v = des_load(in);
    if (encrypt) {
        v = des_crypt(ks[0], v, 0); v = des_crypt(ks[1], v, 1); v = des_crypt(ks[2], v, 0);
    } else {
        v = des_crypt(ks[2], v, 1); v = des_crypt(ks[1], v, 0); v = des_crypt(ks[0], v, 1);
    }
    for (int i = 7; i >= 0; i--, v >>= 8)
        out[i] = (uint8_t)v;
}

// ---------------------------------------------------------------------------
// CCCryptor: AES with ECB, CBC and CFB8. Padding is handled by the caller (.NET).

enum { CC_OK = 0, CC_PARAM_ERROR = -4300, CC_UNIMPLEMENTED = -4305 };
enum { MODE_ECB = 1, MODE_CBC = 2, MODE_CFB8 = 10 };

struct cryptor {
    int encrypt;
    int mode;
    int des3;
    size_t bs;
    struct aes_key key;
    uint64_t dks[3][16];
    uint8_t iv[16];
    uint8_t initial_iv[16];
};

static void cryptor_block(struct cryptor *c, int encrypt, const uint8_t *in, uint8_t *out)
{
    if (c->des3)
        des3_block(c->dks, encrypt, in, out);
    else if (encrypt)
        aes_encrypt_block(&c->key, in, out);
    else
        aes_decrypt_block(&c->key, in, out);
}

static int cc_cryptor_create_with_mode(uint32_t op, uint32_t mode, uint32_t alg, uint32_t padding, const void *iv,
                                       const void *key, size_t key_len, const void *tweak, size_t tweak_len,
                                       int rounds, uint32_t options, struct cryptor **out)
{
    (void)tweak; (void)tweak_len; (void)rounds; (void)options;
    int des3 = alg == 2;
    if ((alg != 0 && !des3) || padding != 0 || (des3 ? key_len != 24 : key_len != 16 && key_len != 24 && key_len != 32))
        return CC_UNIMPLEMENTED; // only AES and 3DES without CommonCrypto padding
    if (mode != MODE_ECB && mode != MODE_CBC && (mode != MODE_CFB8 || des3))
        return CC_UNIMPLEMENTED;
    if (!inv_sbox[0])
        for (int i = 0; i < 256; i++)
            inv_sbox[sbox[i]] = (uint8_t)i;
    struct cryptor *c = calloc(1, sizeof *c);
    c->encrypt = op == 0;
    c->mode = (int)mode;
    c->des3 = des3;
    c->bs = des3 ? 8 : 16;
    if (des3)
        for (int i = 0; i < 3; i++)
            des_schedule(c->dks[i], (const uint8_t *)key + 8 * i);
    else
        aes_expand(&c->key, key, key_len);
    if (iv)
        memcpy(c->iv, iv, c->bs);
    memcpy(c->initial_iv, c->iv, 16);
    *out = c;
    return CC_OK;
}

// Darwin passes the last four arguments packed on the stack: tweakLength @0,
// numRounds @8, options @12, cryptorRef @16. Entered through the STACK_ARGS
// trampoline apple_CCCryptorCreateWithMode.
int cc_cryptor_create_with_mode_stack(uint32_t op, uint32_t mode, uint32_t alg, uint32_t padding, const void *iv,
                                      const void *key, size_t key_len, const void *tweak, const uint8_t *stack)
{
    size_t tweak_len;
    int32_t rounds;
    uint32_t options;
    struct cryptor **out;
    memcpy(&tweak_len, stack, sizeof tweak_len);
    memcpy(&rounds, stack + 8, sizeof rounds);
    memcpy(&options, stack + 12, sizeof options);
    memcpy(&out, stack + 16, sizeof out);
    return cc_cryptor_create_with_mode(op, mode, alg, padding, iv, key, key_len, tweak, tweak_len, rounds, options,
                                       out);
}

#if defined(__APPLE__)
// Host tests on macOS call through the native Darwin ABI.
#define CC_CRYPTOR_CREATE_WITH_MODE_ENTRY cc_cryptor_create_with_mode
#else
void apple_CCCryptorCreateWithMode(void);
#define CC_CRYPTOR_CREATE_WITH_MODE_ENTRY apple_CCCryptorCreateWithMode
#endif

static int cc_cryptor_update(struct cryptor *c, const void *in, size_t len, void *out, size_t avail, size_t *moved)
{
    const uint8_t *p = in;
    uint8_t *q = out;
    if (avail < len)
        return -4301; // kCCBufferTooSmall
    if (c->mode == MODE_CFB8) {
        for (size_t i = 0; i < len; i++) {
            uint8_t ks[16];
            aes_encrypt_block(&c->key, c->iv, ks);
            uint8_t inb = p[i], outb = inb ^ ks[0];
            q[i] = outb;
            memmove(c->iv, c->iv + 1, 15);
            c->iv[15] = c->encrypt ? outb : inb;
        }
    } else {
        if (len % c->bs)
            return -4303; // kCCAlignmentError
        for (size_t off = 0; off < len; off += c->bs) {
            uint8_t block[16];
            if (c->mode == MODE_CBC && c->encrypt) {
                for (size_t i = 0; i < c->bs; i++)
                    block[i] = p[off + i] ^ c->iv[i];
                cryptor_block(c, 1, block, q + off);
                memcpy(c->iv, q + off, c->bs);
            } else if (c->mode == MODE_CBC) {
                uint8_t saved[16];
                memcpy(saved, p + off, c->bs);
                cryptor_block(c, 0, saved, block);
                for (size_t i = 0; i < c->bs; i++)
                    q[off + i] = block[i] ^ c->iv[i];
                memcpy(c->iv, saved, c->bs);
            } else {
                cryptor_block(c, c->encrypt, p + off, q + off);
            }
        }
    }
    if (moved)
        *moved = len;
    return CC_OK;
}

static int cc_cryptor_reset(struct cryptor *c, const void *iv)
{
    memcpy(c->iv, iv ? iv : c->initial_iv, c->bs);
    return CC_OK;
}

static int cc_cryptor_release(struct cryptor *c)
{
    if (c) {
        memset(c, 0, sizeof *c);
        free(c);
    }
    return CC_OK;
}

// ---------------------------------------------------------------------------

const struct shim crypto_shims[] = {
    { "_CC_MD5_Init", (void *)cc_md5_init }, { "_CC_MD5_Update", (void *)cc_md5_update },
    { "_CC_MD5_Final", (void *)cc_md5_final }, { "_CC_MD5", (void *)cc_md5 },
    { "_CC_SHA1_Init", (void *)cc_sha1_init }, { "_CC_SHA1_Update", (void *)cc_sha1_update },
    { "_CC_SHA1_Final", (void *)cc_sha1_final }, { "_CC_SHA1", (void *)cc_sha1 },
    { "_CC_SHA256_Init", (void *)cc_sha256_init }, { "_CC_SHA256_Update", (void *)cc_sha256_update },
    { "_CC_SHA256_Final", (void *)cc_sha256_final }, { "_CC_SHA256", (void *)cc_sha256 },
    { "_CC_SHA384_Init", (void *)cc_sha384_init }, { "_CC_SHA384_Update", (void *)cc_sha384_update },
    { "_CC_SHA384_Final", (void *)cc_sha384_final }, { "_CC_SHA384", (void *)cc_sha384 },
    { "_CC_SHA512_Init", (void *)cc_sha512_init }, { "_CC_SHA512_Update", (void *)cc_sha512_update },
    { "_CC_SHA512_Final", (void *)cc_sha512_final }, { "_CC_SHA512", (void *)cc_sha512 },
    { "_CCHmacInit", (void *)cc_hmac_init }, { "_CCHmacUpdate", (void *)cc_hmac_update },
    { "_CCHmacFinal", (void *)cc_hmac_final }, { "_CCHmac", (void *)cc_hmac },
    { "_CCKeyDerivationPBKDF", (void *)cc_pbkdf },
    { "_CCRandomGenerateBytes", (void *)cc_random_generate_bytes },
    { "_CCCryptorCreateWithMode", (void *)CC_CRYPTOR_CREATE_WITH_MODE_ENTRY },
    { "_CCCryptorUpdate", (void *)cc_cryptor_update },
    { "_CCCryptorReset", (void *)cc_cryptor_reset },
    { "_CCCryptorRelease", (void *)cc_cryptor_release },
};
const size_t crypto_shims_count = SHIM_COUNT(crypto_shims);
