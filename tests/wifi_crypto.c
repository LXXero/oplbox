/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the WPA2 key primitives (firmware/wifi/wpa_crypto.c) and the BearSSL code under them.
 *   - PBKDF2 passphrase to PSK: the three IEEE 802.11 J.4.2 vectors, and RFC 6070's 4096-iteration case (its
 *     20-byte key is the first PBKDF2 block, so the PMK's first 20 bytes), run in steps of several sizes.
 *   - PRF-512: the three IEEE 802.11 J.3 (802.11i H.3) test cases.
 *   - AES key unwrap: RFC 3394 4.1-4.6, plus a corrupted input, a wrong KEK and bad sizes.
 *   - SHA-1 "abc" (FIPS 180-2 A.1), HMAC-SHA1 RFC 2202 test 2, and a PTK whose expected value was computed with
 *     Python's hashlib/hmac (not a published vector; it checks the Min/Max ordering and the 48-byte cut).
 * The 802.11 vectors are as wpa_supplicant 2.9's src/crypto/crypto_module_tests.c reproduces them; each was
 * also recomputed with Python's hashlib before it went in here. Exits 1 when a check fails.
 */
#include <stdio.h>
#include <string.h>
#include "wpa_crypto.h"
#include "bearssl/bearssl_hash.h"

static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    unsigned v;
    while (s[0] && s[1] && sscanf(s, "%2x", &v) == 1) {
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static int same_hex(const uint8_t *got, size_t len, const char *want)
{
    uint8_t w[64];
    return unhex(want, w) == len && memcmp(got, w, len) == 0;
}

static int zero(const uint8_t *p, size_t n)
{
    while (n--)
        if (*p++)
            return 0;
    return 1;
}

/* the PMK in steps of `step` iterations: returns the number of steps it took, 0 on a refused start */
static unsigned pmk(const char *pass, const char *ssid, unsigned step, uint8_t out[WPA_PMK_LEN])
{
    struct wpa_pmk_job j;
    unsigned passes = 0;
    if (wpa_pmk_begin(&j, pass, strlen(pass), (const uint8_t *)ssid, strlen(ssid)))
        return 0;
    do {
        passes++;
        if (passes == 1 && wpa_pmk_take(&j, out) != -1)
            return 0;                               /* take refused while it runs ... */
    } while (!wpa_pmk_step(&j, step));
    if (wpa_pmk_take(&j, out))                      /* ... and gives the key once done */
        return 0;
    return passes;
}

static void test_pmk(void)
{
    static const struct { const char *pass, *ssid, *psk; } v[] = {     /* IEEE 802.11 J.4.2 */
        {"password", "IEEE", "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"},
        {"ThisIsAPassword", "ThisIsASSID", "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"},
        {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",
         "becb93866bb8c3832cb777c2f559807c8c59afcb6eae734885001300a981cc62"},
    };
    uint8_t k[WPA_PMK_LEN];
    char what[96];
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        unsigned passes = pmk(v[i].pass, v[i].ssid, WPA_PMK_STEP, k);
        snprintf(what, sizeof what, "PSK J.4.2 case %u", i + 1);
        check(passes && same_hex(k, sizeof k, v[i].psk), what);
        snprintf(what, sizeof what, "PSK J.4.2 case %u: %u passes of %u, want %u", i + 1, passes, WPA_PMK_STEP,
                 2 * WPA_PMK_ITERATIONS / WPA_PMK_STEP);
        check(passes == 2 * WPA_PMK_ITERATIONS / WPA_PMK_STEP, what);
    }
    /* step sizes that end a pass on, just before and across the block boundary at 4096, and all at once */
    static const unsigned steps[] = {1, 7, 4095, 4096, 4097, 8192, 100000};
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        unsigned passes = pmk(v[1].pass, v[1].ssid, steps[i], k);
        snprintf(what, sizeof what, "PSK in steps of %u", steps[i]);
        check(passes == (2 * WPA_PMK_ITERATIONS + steps[i] - 1) / steps[i] && same_hex(k, sizeof k, v[1].psk), what);
    }
    /* RFC 6070: PBKDF2-HMAC-SHA1("password", "salt", 4096, 20) */
    check(pmk("password", "salt", 512, k) && same_hex(k, 20, "4b007901b765489abead49d926f721d065a429c1"),
          "PBKDF2 RFC 6070 4096 iterations");

    /* refused starts: passphrase 7 or 64 characters, a control or non-ASCII byte, SSID empty or 33 bytes */
    struct wpa_pmk_job j;
    static const char ssid33[] = "123456789012345678901234567890123";
    check(wpa_pmk_begin(&j, "1234567", 7, (const uint8_t *)"x", 1) == -1, "7-character passphrase refused");
    check(wpa_pmk_begin(&j, "1234567890123456789012345678901234567890123456789012345678901234", 64,
                        (const uint8_t *)"x", 1) == -1, "64-character passphrase refused");
    check(wpa_pmk_begin(&j, "pass\tword", 9, (const uint8_t *)"x", 1) == -1, "tab in passphrase refused");
    check(wpa_pmk_begin(&j, "pass\x7fword", 9, (const uint8_t *)"x", 1) == -1, "DEL in passphrase refused");
    check(wpa_pmk_begin(&j, "pass\xc3\xa9word", 10, (const uint8_t *)"x", 1) == -1, "UTF-8 passphrase refused");
    check(wpa_pmk_begin(&j, "password", 8, (const uint8_t *)"", 0) == -1, "empty SSID refused");
    check(wpa_pmk_begin(&j, "password", 8, (const uint8_t *)ssid33, 33) == -1, "33-byte SSID refused");
    memset(k, 0x55, sizeof k);
    check(wpa_pmk_step(&j, 1) == 1 && wpa_pmk_take(&j, k) == -1 && k[0] == 0x55, "a refused job gives no key");
    check(wpa_pmk_begin(&j, " !~~~~~~", 8, (const uint8_t *)ssid33, 32) == 0, "space and ~, 32-byte SSID accepted");
    wpa_pmk_clear(&j);
    check(zero((const uint8_t *)&j.key, sizeof j.key) && wpa_pmk_take(&j, k) == -1, "clear wipes the key state");
    pmk("password", "IEEE", 8192, k);
    check(same_hex(k, sizeof k, v[0].psk), "a job can be reused");
}

static void test_prf(void)
{
    static const uint8_t k0[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                                   0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    uint8_t k2[20], d2[50], out[64], cut[20];
    memset(k2, 0xaa, sizeof k2);
    memset(d2, 0xdd, sizeof d2);
    wpa_prf_sha1(k0, sizeof k0, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
    check(same_hex(out, 64, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                            "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"), "PRF J.3 case 1");
    wpa_prf_sha1((const uint8_t *)"Jefe", 4, "prefix", (const uint8_t *)"what do ya want for nothing?", 28, out, 64);
    check(same_hex(out, 64, "51f4de5b33f249adf81aeb713a3c20f4fe631446fabdfa58244759ae58ef9009"
                            "a99abf4eac2ca5fa87e692c440eb40023e7babb206d61de7b92f41529092b8fc"), "PRF J.3 case 2");
    wpa_prf_sha1(k2, sizeof k2, "prefix", d2, sizeof d2, out, 64);
    check(same_hex(out, 64, "e1ac546ec4cb636f9976487be5c86be17a0252ca5d8d8df12cfb0473525249ce"
                            "9dd8d177ead710bc9b590547239107aef7b4abd43d87f0a68f1cbd9e2b6f7607"), "PRF J.3 case 3");
    wpa_prf_sha1(k2, sizeof k2, "prefix", d2, sizeof d2, cut, 13);   /* a shorter output is a prefix */
    check(memcmp(cut, out, 13) == 0, "PRF cut to 13 bytes");
}

static void test_ptk(void)
{
    uint8_t pmk_[WPA_PMK_LEN], aa[6], spa[6], an[32], sn[32], ptk[WPA_PTK_LEN + 1], ptk2[WPA_PTK_LEN];
    unhex("0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", pmk_);
    unhex("a0a1a2a3a4a5", aa);
    unhex("b0b1b2b3b4b5", spa);
    unhex("ff1112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f", an);
    unhex("e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", sn);
    ptk[WPA_PTK_LEN] = 0x5a;
    wpa_ptk(pmk_, aa, spa, an, sn, ptk);
    check(same_hex(ptk, WPA_PTK_LEN, "703733456c3a38f250d16f19c0fdfa23dea88420992656ba57ec5159616a6aa9"
                                     "9aa0c21c7700c3bcf108ea571c8f88d8") && ptk[WPA_PTK_LEN] == 0x5a,
          "PTK (Python reference), 48 bytes and no more");
    wpa_ptk(pmk_, spa, aa, sn, an, ptk2);           /* the other side computes the same key */
    check(memcmp(ptk, ptk2, WPA_PTK_LEN) == 0, "PTK is the same from either side");
}

static void test_unwrap(void)
{
    static const struct { const char *kek, *plain, *cipher; } v[] = {                  /* RFC 3394 4.1-4.6 */
        {"000102030405060708090a0b0c0d0e0f", "00112233445566778899aabbccddeeff",
         "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"},
        {"000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff",
         "96778b25ae6ca435f92b5b97c050aed2468ab8a17ad84e5d"},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f", "00112233445566778899aabbccddeeff",
         "64e8c3f9ce0f5ba263e9777905818a2a93c8191e7d6e8ae7"},
        {"000102030405060708090a0b0c0d0e0f1011121314151617", "00112233445566778899aabbccddeeff0001020304050607",
         "031d33264e15d33268f24ec260743edce1c6c7ddee725a936ba814915c6762d2"},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
         "00112233445566778899aabbccddeeff0001020304050607",
         "a8f9bc1612c68b3ff6e6f4fbe30e71e4769c8b80a32cb8958cd5d17d6b254da1"},
        {"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
         "00112233445566778899aabbccddeeff000102030405060708090a0b0c0d0e0f",
         "28c9f404c4b810f4cbccb35cfb87f8263f5786e2d80ed326cbc7f0e71a99f43bfb988b9b7a02dd21"},
    };
    uint8_t kek[32], c[40], p[32];
    char what[96];
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        size_t kl = unhex(v[i].kek, kek), cl = unhex(v[i].cipher, c), n = cl / 8 - 1;
        snprintf(what, sizeof what, "AES key unwrap RFC 3394 4.%u", i + 1);
        check(wpa_aes_unwrap(kek, kl, n, c, p) == 0 && same_hex(p, n * 8, v[i].plain), what);
    }
    size_t kl = unhex(v[0].kek, kek), cl = unhex(v[0].cipher, c);
    for (size_t b = 0; b < cl; b++) {               /* any changed byte fails the integrity check */
        c[b] ^= 0x01;
        memset(p, 0x55, sizeof p);
        int r = wpa_aes_unwrap(kek, kl, 2, c, p);
        c[b] ^= 0x01;
        snprintf(what, sizeof what, "unwrap refuses a change in byte %zu, output zeroed", b);
        check(r == -1 && zero(p, 16), what);
    }
    /* 4.1's key wrapped under an integrity value one bit off in its last or first byte (A6..A7, A7..A6;
     * wrapped with Python's cryptography AES and RFC 3394 2.2.1, checked against 4.1 first): only that byte
     * differs once unwrapped, so every byte of the check counts */
    static const char *const off[] = {"715fbc69210b823f7dfefab3b887e4c1162b29c304609004",
                                      "079e449c7e8504b8d559eda0387724c78820c1e93f4f9716"};
    for (unsigned i = 0; i < 2; i++) {
        uint8_t c2[24];
        unhex(off[i], c2);
        memset(p, 0x55, sizeof p);
        snprintf(what, sizeof what, "unwrap refuses integrity value %s", i ? "A7A6..A6" : "A6..A6A7");
        check(wpa_aes_unwrap(kek, kl, 2, c2, p) == -1 && zero(p, 16), what);
    }
    kek[15] ^= 0x80;
    check(wpa_aes_unwrap(kek, kl, 2, c, p) == -1 && zero(p, 16), "unwrap refuses the wrong KEK");
    kek[15] ^= 0x80;
    check(wpa_aes_unwrap(kek, 15, 2, c, p) == -1, "unwrap refuses a 15-byte KEK");
    /* one block, 0011223344556677, wrapped by the same 2.2.1 steps with n = 1 (Python, as above): it would pass
     * the integrity check, so only the n >= 2 rule of RFC 3394 refuses it */
    uint8_t c1[16];
    unhex("b82669ca42cb86233b5e5cfeacee620b", c1);
    check(wpa_aes_unwrap(kek, kl, 1, c1, p) == -1, "unwrap refuses n = 1");
    check(wpa_aes_unwrap(kek, kl, 2, c, p) == 0, "unwrap still works after the refusals");
}

static void test_bearssl(void)
{
    br_sha1_context s;
    uint8_t h[20];
    br_sha1_init(&s);
    br_sha1_update(&s, "abc", 3);
    br_sha1_out(&s, h);
    check(same_hex(h, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"), "SHA-1 \"abc\" (FIPS 180-2)");
    wpa_hmac_sha1((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, h);
    check(same_hex(h, 20, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"), "HMAC-SHA1 RFC 2202 test 2");
}

int main(void)
{
    test_bearssl();
    test_prf();
    test_ptk();
    test_unwrap();
    test_pmk();
    if (fails) {
        printf("wifi_crypto: %d of %d checks failed\n", fails, checks);
        return 1;
    }
    printf("wifi_crypto: %d checks passed (PSK J.4.2, PRF J.3, RFC 3394 4.1-4.6, PTK, refusals)\n", checks);
    return 0;
}
