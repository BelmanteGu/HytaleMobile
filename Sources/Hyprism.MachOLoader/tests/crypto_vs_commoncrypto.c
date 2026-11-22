// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// Host test (macOS only): compares the CommonCrypto shims with Apple's CommonCrypto.
//   cc -O1 tests/crypto_vs_commoncrypto.c -Isrc -o /tmp/cctest && /tmp/cctest

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
static ssize_t getrandom(void *b, size_t n, unsigned f) { (void)f; arc4random_buf(b, n); return (ssize_t)n; }
#define getrandom getrandom
#include "../src/crypto.c"
#include <CommonCrypto/CommonCrypto.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    uint8_t data[5000], a[64], b[64];
    arc4random_buf(data, sizeof data);
    size_t sizes[] = { 0, 1, 55, 56, 63, 64, 65, 111, 112, 127, 128, 129, 1000, 4999 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t n = sizes[i];
        cc_md5(data, (uint32_t)n, a); CC_MD5(data, (CC_LONG)n, b); CHECK(!memcmp(a, b, 16), "md5 %zu", n);
        cc_sha1(data, (uint32_t)n, a); CC_SHA1(data, (CC_LONG)n, b); CHECK(!memcmp(a, b, 20), "sha1 %zu", n);
        cc_sha256(data, (uint32_t)n, a); CC_SHA256(data, (CC_LONG)n, b); CHECK(!memcmp(a, b, 32), "sha256 %zu", n);
        cc_sha384(data, (uint32_t)n, a); CC_SHA384(data, (CC_LONG)n, b); CHECK(!memcmp(a, b, 48), "sha384 %zu", n);
        cc_sha512(data, (uint32_t)n, a); CC_SHA512(data, (CC_LONG)n, b); CHECK(!memcmp(a, b, 64), "sha512 %zu", n);
        // streaming in odd chunks, plus a copied running state
        struct sha256_state s; cc_sha256_init(&s);
        for (size_t off = 0; off < n; off += 7) cc_sha256_update(&s, data + off, (uint32_t)(n - off < 7 ? n - off : 7));
        struct sha256_state copy = s; cc_sha256_final(a, &copy); CC_SHA256(data, (CC_LONG)n, b);
        CHECK(!memcmp(a, b, 32), "sha256 stream %zu", n);
        uint32_t algs[] = { kCCHmacAlgSHA1, kCCHmacAlgMD5, kCCHmacAlgSHA256, kCCHmacAlgSHA384, kCCHmacAlgSHA512, kCCHmacAlgSHA224 };
        for (int k = 0; k < 6; k++) {
            size_t klen = (n * 3) % 200;
            cc_hmac(algs[k], data + 7, klen, data, n, a); CCHmac(algs[k], data + 7, klen, data, n, b);
            CHECK(!memcmp(a, b, digest_len(algs[k])), "hmac alg %u n %zu klen %zu", algs[k], n, klen);
        }
    }
    for (uint32_t prf = 1; prf <= 5; prf++) {
        uint8_t x[100], y[100];
        cc_pbkdf(2, "password", 8, data, 16, prf, 1000, x, sizeof x);
        CCKeyDerivationPBKDF(kCCPBKDF2, "password", 8, data, 16, prf, 1000, y, sizeof y);
        CHECK(!memcmp(x, y, sizeof x), "pbkdf2 prf %u", prf);
        // .NET exports certificate keys with an empty password
        cc_pbkdf(2, "", 0, data, 16, prf, 2048, x, 32);
        CCKeyDerivationPBKDF(kCCPBKDF2, "", 0, data, 16, prf, 2048, y, 32);
        CHECK(!memcmp(x, y, 32), "pbkdf2 empty password prf %u", prf);
    }
    size_t klens[] = { 16, 24, 32 };
    uint32_t modes[] = { kCCModeECB, kCCModeCBC, kCCModeCFB8 };
    for (int m = 0; m < 3; m++)
        for (int k = 0; k < 3; k++)
            for (uint32_t op = 0; op < 2; op++) {
                uint8_t mine[4096], theirs[4096];
                size_t moved;
                struct cryptor *c; CCCryptorRef r;
                CHECK(cc_cryptor_create_with_mode(op, modes[m], 0, 0, data + 100, data + 200, klens[k], NULL, 0, 0, 0, &c) == 0, "create");
                CCCryptorCreateWithMode(op, modes[m], kCCAlgorithmAES, ccNoPadding, data + 100, data + 200, klens[k], NULL, 0, 0, 0, &r);
                cc_cryptor_update(c, data, 4096, mine, sizeof mine, &moved);
                CCCryptorUpdate(r, data, 4096, theirs, sizeof theirs, &moved);
                CHECK(!memcmp(mine, theirs, 4096), "aes mode %u key %zu op %u", modes[m], klens[k], op);
                cc_cryptor_release(c); CCCryptorRelease(r);
                // the same data in two updates, as .NET does when it holds back the padding block
                CHECK(cc_cryptor_create_with_mode(op, modes[m], 0, 0, data + 100, data + 200, klens[k], NULL, 0, 0, 0, &c) == 0, "create");
                CCCryptorCreateWithMode(op, modes[m], kCCAlgorithmAES, ccNoPadding, data + 100, data + 200, klens[k], NULL, 0, 0, 0, &r);
                size_t m1, m2;
                cc_cryptor_update(c, data, 4096 - 16, mine, sizeof mine, &m1);
                cc_cryptor_update(c, data + 4096 - 16, 16, mine + m1, sizeof mine - m1, &m2);
                CCCryptorUpdate(r, data, 4096, theirs, sizeof theirs, &moved);
                CHECK(m1 + m2 == 4096 && !memcmp(mine, theirs, 4096), "aes chained mode %u key %zu op %u", modes[m], klens[k], op);
                cc_cryptor_release(c); CCCryptorRelease(r);
            }
    printf(failures ? "%d failures\n" : "all CommonCrypto checks passed\n", failures);
    return failures != 0;
}
