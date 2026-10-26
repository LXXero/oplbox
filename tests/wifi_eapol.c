/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the station side of the WPA2-PSK EAPOL-Key handshakes (firmware/wifi/wpa_eapol.c).
 *   1. A recorded handshake: the 4-way handshake in Wireshark's sample capture wpa-Induction.pcap (SSID "Coherer",
 *      passphrase "Induction", AP 00:0c:41:82:b2:55, station 00:0d:93:82:36:3a; wiki.wireshark.org SampleCaptures).
 *      Its msg 1 and msg 3 go to the station with the capture's SNonce; the msg 2 and msg 4 that come back must be
 *      the capture's byte for byte, except the key length field (the recorded station put 16 there, 12.7.6.3 and
 *      12.7.6.5 say 0), with the MIC recomputed for that one field. The TK is the one that decrypts the capture's
 *      CCMP data frame 99 (checked once with Python's cryptography AESCCM, not here); the GTK and its key index are
 *      the GTK KDE in msg 3's key data (the network's group cipher is TKIP, so a 32-byte GTK).
 *   2. A made-up handshake against an AP side in this file (PTK from wpa_ptk, MICs from wpa_hmac_sha1, the key data
 *      wrapped with the AES-128 and RFC 3394 key wrap below): 4-way, group key handshake, a repeated msg 3 and
 *      group msg 1 (answered, keys not flagged for reloading), a PTK rekey. The group msg 2 is compared with one
 *      computed with Python's hmac/hashlib from the same inputs.
 *   3. The refusals: bad MIC, replayed counters, wrong descriptor type and key descriptor version, RSNE mismatch,
 *      truncated frames, key data lengths, a failed unwrap, no GTK, every single-byte change to msg 3 and group msg 1.
 * IEEE 802.11-2020 Annex J has no PTK or EAPOL-Key MIC vector for AKM 2 that could be sourced here: the vectors
 * wpa_supplicant cites from it are J.3/J.4 (in tests/wifi_crypto.c), J.10 (SAE), J.12 (PASN) and J.13 (KDK), the
 * last three SHA-256 based. The IEEE text itself was not read [unchecked]. Exits 1 when a check fails.
 */
#include <stdio.h>
#include <string.h>
#include "wpa_eapol.h"

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
    uint8_t w[512];
    return unhex(want, w) == len && memcmp(got, w, len) == 0;
}

/* ---- AES-128 encryption (FIPS-197) and RFC 3394 2.2.1 key wrap, for the AP side only */

static uint8_t sbox[256];

static uint8_t xt(uint8_t a)
{
    return (uint8_t)(a << 1 ^ (a & 0x80 ? 0x1b : 0));
}

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a))
        if (b & 1)
            r ^= a;
    return r;
}

static void sbox_init(void)                     /* FIPS-197 5.1.1: the inverse in GF(2^8), then the affine map */
{
    for (int i = 0; i < 256; i++) {
        uint8_t inv = 0, s, x;
        for (int j = 1; i && j < 256; j++)
            if (gmul((uint8_t)i, (uint8_t)j) == 1) {
                inv = (uint8_t)j;
                break;
            }
        s = x = inv;
        for (int k = 0; k < 4; k++) {
            x = (uint8_t)(x << 1 | x >> 7);
            s ^= x;
        }
        sbox[i] = s ^ 0x63;
    }
}

static void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16], t[16], rcon = 1;
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t w[4];
        memcpy(w, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t u = w[0];
            w[0] = sbox[w[1]] ^ rcon;
            w[1] = sbox[w[2]];
            w[2] = sbox[w[3]];
            w[3] = sbox[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++)
            rk[i + k] = rk[i - 16 + k] ^ w[k];
    }
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++)            /* SubBytes and ShiftRows: row i%4 moves left by i%4 columns */
            t[i] = sbox[s[((i / 4 + i % 4) % 4) * 4 + i % 4]];
        if (r < 10)
            for (int c = 0; c < 4; c++) {       /* MixColumns */
                uint8_t *p = t + 4 * c, a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3], all = a0 ^ a1 ^ a2 ^ a3;
                p[0] ^= all ^ xt(a0 ^ a1);
                p[1] ^= all ^ xt(a1 ^ a2);
                p[2] ^= all ^ xt(a2 ^ a3);
                p[3] ^= all ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ rk[16 * r + i];
    }
    memcpy(out, s, 16);
}

/* n 64-bit blocks of plain to n + 1 of cipher */
static void wrap(const uint8_t kek[16], size_t n, const uint8_t *plain, uint8_t *cipher)
{
    uint8_t b[16];
    memset(b, 0xA6, 8);
    memcpy(cipher + 8, plain, n * 8);
    for (unsigned j = 0; j <= 5; j++)
        for (size_t i = 1; i <= n; i++) {
            uint32_t t = (uint32_t)(n * j + i);
            memcpy(b + 8, cipher + i * 8, 8);
            aes128(kek, b, b);
            b[4] ^= (uint8_t)(t >> 24);
            b[5] ^= (uint8_t)(t >> 16);
            b[6] ^= (uint8_t)(t >> 8);
            b[7] ^= (uint8_t)t;
            memcpy(cipher + i * 8, b + 8, 8);
        }
    memcpy(cipher, b, 8);
}

static void test_aes(void)
{
    uint8_t k[16], p[32], c[40], q[32];
    sbox_init();
    check(sbox[0] == 0x63 && sbox[0x53] == 0xED && sbox[0xFF] == 0x16, "AES S-box (FIPS-197 figure 7)");
    unhex("000102030405060708090a0b0c0d0e0f", k);
    unhex("00112233445566778899aabbccddeeff", p);
    aes128(k, p, c);
    check(same_hex(c, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"), "AES-128 FIPS-197 C.1");
    wrap(k, 2, p, c);                           /* KEK 000102..0F, key 00112233..FF */
    check(same_hex(c, 24, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"), "key wrap RFC 3394 4.1");
    for (int i = 0; i < 32; i++)
        p[i] = (uint8_t)(i * 7 + 1);
    wrap(k, 4, p, c);
    check(wpa_aes_unwrap(k, 16, 4, c, q) == 0 && memcmp(p, q, 32) == 0, "key wrap of 4 blocks unwraps");
}

/* ---- frames */

#define F_INFO 5
#define F_KEYLEN 7
#define F_REPLAY 9
#define F_NONCE 17
#define F_RSC 65
#define F_MIC 81
#define F_KDLEN 97
#define F_KD 99

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* the MIC as 12.7.2 has it, done on a copy with the field zeroed (not the library's incremental form) */
static void test_mic(const uint8_t *kck, const uint8_t *f, size_t len, uint8_t out[16])
{
    uint8_t c[512], h[20];
    memcpy(c, f, len);
    memset(c + F_MIC, 0, 16);
    wpa_hmac_sha1(kck, 16, c, len, h);
    memcpy(out, h, 16);
}

static int test_mic_ok(const uint8_t *kck, const uint8_t *f, size_t len)
{
    uint8_t m[16];
    test_mic(kck, f, len, m);
    return memcmp(m, f + F_MIC, 16) == 0;
}

/* An AP's EAPOL-Key frame. kd/kdl are the key data as sent (already wrapped); kck NULL leaves the MIC zero. */
static size_t ap_frame(uint8_t *f, unsigned info, unsigned keylen, uint64_t rc, const uint8_t *nonce,
                       const uint8_t *rsc, const uint8_t *kd, size_t kdl, const uint8_t *kck)
{
    size_t len = 99 + kdl;
    memset(f, 0, len);
    f[0] = 2;
    f[1] = 3;
    put16(f + 2, (unsigned)(95 + kdl));
    f[4] = 2;
    put16(f + F_INFO, info);
    put16(f + F_KEYLEN, keylen);
    for (int i = 0; i < 8; i++)
        f[F_REPLAY + i] = (uint8_t)(rc >> (56 - 8 * i));
    if (nonce)
        memcpy(f + F_NONCE, nonce, 32);
    if (rsc)
        memcpy(f + F_RSC, rsc, 8);
    put16(f + F_KDLEN, (unsigned)kdl);
    memcpy(f + F_KD, kd, kdl);
    if (kck)
        test_mic(kck, f, len, f + F_MIC);
    return len;
}

/* key data padded as 12.7.2 has it (0xDD then zeros, to a multiple of 8 and at least 16) and wrapped */
static size_t ap_key_data(const uint8_t *plain, size_t n, const uint8_t *kek, uint8_t *out)
{
    uint8_t p[256];
    size_t m = n < 16 ? 16 : (n + 7) / 8 * 8;
    memcpy(p, plain, n);
    if (m > n) {
        p[n] = 0xDD;
        memset(p + n + 1, 0, m - n - 1);
    }
    wrap(kek, m / 8, p, out);
    return m + 8;
}

/* GTK KDE: DD, length, 00-0F-AC, 1, key ID / Tx, reserved, GTK */
static size_t gtk_kde(uint8_t *p, const uint8_t *gtk, size_t len, uint8_t key_id)
{
    p[0] = 0xDD;
    p[1] = (uint8_t)(6 + len);
    p[2] = 0x00;
    p[3] = 0x0F;
    p[4] = 0xAC;
    p[5] = 1;
    p[6] = key_id;
    p[7] = 0;
    memcpy(p + 8, gtk, len);
    return 8 + len;
}

/* the nonce source: hands out its nonce, then the nonce plus 1 in its last byte, ...; or fails */
struct nonces {
    uint8_t next[32];
    int fail;
};

static int nonce_cb(void *arg, uint8_t out[32])
{
    struct nonces *n = arg;
    if (n->fail)
        return -1;
    memcpy(out, n->next, 32);
    n->next[31]++;
    return 0;
}

#define KI_V2 0x0002u
#define KI_P 0x0008u
#define KI_INSTALL 0x0040u
#define KI_ACK 0x0080u
#define KI_MIC 0x0100u
#define KI_SECURE 0x0200u
#define KI_REQUEST 0x0800u
#define KI_ENC 0x1000u
#define MSG1_INFO (KI_V2 | KI_P | KI_ACK)
#define MSG3_INFO (KI_V2 | KI_P | KI_INSTALL | KI_ACK | KI_MIC | KI_SECURE | KI_ENC)
#define GROUP1_INFO (KI_V2 | KI_ACK | KI_MIC | KI_SECURE | KI_ENC)

/* ---- 1. the recorded handshake */

static const char *ind_msg1 =
    "0203007502008a001000000000000000003e8e967dacd960324cac5b6aa721235bf57b949771c867989f49d04ed47c693300"
    "00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000016dd"
    "14000fac04592da88096c461da246c69001e877f3d";
static const char *ind_msg2 =
    "0203007502010a00100000000000000000cdf405ceb9d889ef3dec42609828fae546b7add7baecbb1a394eac5214b1d38600"
    "00000000000000000000000000000000000000000000000000000000000000a462a7029ad5ba30b6af0df391988e45001630"
    "140100000fac020100000fac040100000fac020000";
static const char *ind_msg3 =
    "020300af0213ca001000000000000000013e8e967dacd960324cac5b6aa721235bf57b949771c867989f49d04ed47c6933f5"
    "7b949771c867989f49d04ed47c6934cf0200000000000000000000000000007d0af6df51e99cde7a187453f0f935370050cf"
    "a72cde35b2c1e2319255806ab364179fd9673041b9a5939fa1a2010d2ac794e25168055f794ddc1fdfae3521f4446bfd11da"
    "98345f543df6ce199df8fe48f8cdd17adca87bf45711183c496d41aa0c";
static const char *ind_msg4 =
    "0203005f02030a00100000000000000001000000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000010bba3bdfbcfde2bc537509d71f2ecd10000";

static void test_capture(void)
{
    /* the association request's RSNE (CCMP pairwise, TKIP group, PSK) and the beacon's (CCMP and TKIP pairwise) */
    static const char *assoc_rsne = "30140100000fac020100000fac040100000fac020000";
    static const char *beacon_rsne = "30180100000fac020200000fac04000fac020100000fac020000";
    static const char *ptk = "b1cd792716762903f723424cd7d1651182a644133bfa4e0b75d96d230835843315798d511beae002"
                             "8313c8ab32f12c7e";       /* Python hashlib from the same inputs; TK decrypts frame 99 */
    uint8_t pmk[32], spa[6], aa[6], own[64], ap[64], m[512], want[512], r[WPA_EAPOL_REPLY_MAX], kck[48];
    size_t own_len, ap_len, len, wlen, rlen;
    struct wpa_pmk_job job;
    struct wpa_eapol s;
    struct nonces nc = {{0}, 0};

    check(wpa_pmk_begin(&job, "Induction", 9, (const uint8_t *)"Coherer", 7) == 0 && wpa_pmk_step(&job, 8192) &&
          wpa_pmk_take(&job, pmk) == 0, "capture: PMK from the passphrase");
    check(same_hex(pmk, 32, "a288fcf0caaacda9a9f58633ff35e8992a01d9c10ba5e02efdf8cb5d730ce7bc"),
          "capture: PMK as Python's PBKDF2 has it");
    unhex("000d9382363a", spa);
    unhex("000c4182b255", aa);
    own_len = unhex(assoc_rsne, own);
    ap_len = unhex(beacon_rsne, ap);
    unhex(ind_msg2, want);
    memcpy(nc.next, want + F_NONCE, 32);        /* the recorded station's SNonce */
    unhex(ptk, kck);
    check(wpa_eapol_init(&s, pmk, spa, aa, own, own_len, ap, ap_len, nonce_cb, &nc) == 0, "capture: init");

    len = unhex(ind_msg1, m);
    check(wpa_eapol_rx(&s, m, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.state == WPA_EAPOL_WAIT_MSG3 &&
          !s.new_keys, "capture: msg 1 taken");
    check(same_hex(s.tptk, 48, ptk), "capture: PTK");
    wlen = unhex(ind_msg2, want);
    check(rlen == wlen && r[F_KEYLEN] == 0 && r[F_KEYLEN + 1] == 0, "capture: msg 2 length and key length 0");
    check(test_mic_ok(kck, r, rlen), "capture: msg 2 MIC verifies");
    r[F_KEYLEN + 1] = 16;                       /* the recorded station's key length, and the MIC over it */
    test_mic(kck, r, rlen, r + F_MIC);
    check(rlen == wlen && memcmp(r, want, wlen) == 0, "capture: msg 2 is the recorded one (key length 16 put back)");

    len = unhex(ind_msg3, m);
    check(wpa_eapol_rx(&s, m, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.state == WPA_EAPOL_DONE &&
          s.new_keys == (WPA_EAPOL_NEW_PTK | WPA_EAPOL_NEW_GTK), "capture: msg 3 taken, PTK and GTK new");
    wlen = unhex(ind_msg4, want);
    check(test_mic_ok(kck, r, rlen), "capture: msg 4 MIC verifies");
    r[F_KEYLEN + 1] = 16;
    test_mic(kck, r, rlen, r + F_MIC);
    check(rlen == wlen && memcmp(r, want, wlen) == 0, "capture: msg 4 is the recorded one (key length 16 put back)");
    check(same_hex(s.tk, 16, "15798d511beae0028313c8ab32f12c7e"), "capture: TK");
    check(s.gtk_len == 32 && s.gtk_idx == 2 && s.gtk_tx == 0 &&
          same_hex(s.gtk, 32, "ee22041a83853263474c38811352282071c122359b7c35a7e7d034f3cd6ac565"),
          "capture: GTK (TKIP, 32 bytes) with key index 2");
    check(same_hex(s.gtk_rsc, 8, "cf02000000000000"), "capture: GTK receive counter from the Key RSC field");
}

/* ---- 2. the made-up handshake */

static const char *rsne_hex = "30140100000fac040100000fac040100000fac020000";     /* CCMP / CCMP / PSK */

struct ap {
    uint8_t pmk[32], aa[6], spa[6], anonce[32], ptk[48], rsne[64], gtk[16];
    size_t rsne_len;
    uint64_t rc;
};

static void ap_init(struct ap *a)
{
    /* PMK: IEEE 802.11 J.4.2's first PSK (passphrase "password", SSID "IEEE"); the station's address the lower */
    unhex("f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", a->pmk);
    unhex("020000000002", a->aa);
    unhex("020000000001", a->spa);
    for (int i = 0; i < 32; i++)
        a->anonce[i] = (uint8_t)(0x80 + i);
    for (int i = 0; i < 16; i++)
        a->gtk[i] = (uint8_t)(0x30 + i);
    a->rsne_len = unhex(rsne_hex, a->rsne);
    a->rc = 1;
}

static void sta_init(struct wpa_eapol *s, const struct ap *a, struct nonces *nc)
{
    uint8_t own[64];
    size_t n = unhex(rsne_hex, own);
    memset(nc, 0, sizeof *nc);
    for (int i = 0; i < 32; i++)
        nc->next[i] = (uint8_t)(0x10 + i);
    wpa_eapol_init(s, a->pmk, a->spa, a->aa, own, n, a->rsne, a->rsne_len, nonce_cb, nc);
}

static size_t ap_msg1(struct ap *a, uint8_t *f)
{
    static const uint8_t z[1];
    return ap_frame(f, MSG1_INFO, 16, a->rc, a->anonce, 0, z, 0, 0);
}

/* msg 3 with the AP's RSNE (rsne, n) and a GTK KDE (key ID gid); the PTK from the station's msg 2 */
static size_t ap_msg3(struct ap *a, uint8_t *f, const uint8_t *rsne, size_t n, uint8_t gid)
{
    static const uint8_t rsc[8] = {5, 0, 0, 0, 0, 0, 0, 0};
    uint8_t p[128], kd[136];
    memcpy(p, rsne, n);
    n += gtk_kde(p + n, a->gtk, 16, gid);
    size_t kdl = ap_key_data(p, n, a->ptk + 16, kd);
    return ap_frame(f, MSG3_INFO, 16, a->rc, a->anonce, rsc, kd, kdl, a->ptk);
}

static size_t ap_group1(struct ap *a, uint8_t *f, uint8_t gid)
{
    static const uint8_t rsc[8] = {0x34, 0x12, 0, 0, 0, 0, 0, 0};
    uint8_t p[64], kd[72];
    size_t n = gtk_kde(p, a->gtk, 16, gid);
    size_t kdl = ap_key_data(p, n, a->ptk + 16, kd);
    return ap_frame(f, GROUP1_INFO, 0, a->rc, 0, rsc, kd, kdl, a->ptk);
}

/* runs msg 1 and msg 2: the AP takes the PTK from the SNonce in msg 2 */
static int run_msg12(struct ap *a, struct wpa_eapol *s)
{
    uint8_t f[512], r[WPA_EAPOL_REPLY_MAX];
    size_t rlen, len = ap_msg1(a, f);
    if (wpa_eapol_rx(s, f, len, r, sizeof r, &rlen) != WPA_EAPOL_OK || rlen != 99 + a->rsne_len)
        return 0;
    wpa_ptk(a->pmk, a->aa, a->spa, a->anonce, r + F_NONCE, a->ptk);
    return test_mic_ok(a->ptk, r, rlen);
}

static void test_made_up(void)
{
    struct ap a;
    struct wpa_eapol s;
    struct nonces nc;
    uint8_t f[512], r[WPA_EAPOL_REPLY_MAX], tk1[16];
    size_t len, rlen;
    unsigned info;

    ap_init(&a);
    sta_init(&s, &a, &nc);
    check(s.state == WPA_EAPOL_WAIT_MSG1, "made-up: init");
    len = ap_msg1(&a, f);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.state == WPA_EAPOL_WAIT_MSG3,
          "made-up: msg 1 taken");
    info = (unsigned)r[F_INFO] << 8 | r[F_INFO + 1];
    check(rlen == 99 + a.rsne_len && r[0] == 2 && r[1] == 3 && r[4] == 2 && info == (KI_V2 | KI_P | KI_MIC) &&
          r[F_REPLAY + 7] == 1 && r[F_NONCE] == 0x10 && r[F_KDLEN + 1] == a.rsne_len &&
          memcmp(r + F_KD, a.rsne, a.rsne_len) == 0, "made-up: msg 2 fields (key info, counter, SNonce, RSNE)");
    wpa_ptk(a.pmk, a.aa, a.spa, a.anonce, r + F_NONCE, a.ptk);
    check(test_mic_ok(a.ptk, r, rlen), "made-up: msg 2 MIC verifies at the AP");
    check(same_hex(a.ptk, 48, "b881c51e62b05850c697361ee6cd6ceaadcb13753f96b7a88b328e239e7df7c7"
                              "d83db6939b4ac59141f08fc5e04144bb"), "made-up: PTK as Python's hashlib has it");

    a.rc = 2;
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 1);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.state == WPA_EAPOL_DONE &&
          s.new_keys == (WPA_EAPOL_NEW_PTK | WPA_EAPOL_NEW_GTK), "made-up: msg 3 taken");
    info = (unsigned)r[F_INFO] << 8 | r[F_INFO + 1];
    check(rlen == 99 && info == (KI_V2 | KI_P | KI_MIC | KI_SECURE) && r[F_REPLAY + 7] == 2 &&
          r[F_KEYLEN + 1] == 0 && r[F_KDLEN + 1] == 0 && test_mic_ok(a.ptk, r, rlen), "made-up: msg 4");
    check(memcmp(s.tk, a.ptk + 32, 16) == 0, "made-up: TK is the AP's");
    check(s.gtk_len == 16 && s.gtk_idx == 1 && memcmp(s.gtk, a.gtk, 16) == 0 && s.gtk_rsc[0] == 5,
          "made-up: GTK, key index 1, receive counter");
    memcpy(tk1, s.tk, 16);

    a.rc = 3;                                   /* the same msg 3 again with a new counter: msg 4, keys not new */
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 1);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && rlen == 99 && s.new_keys == 0 &&
          s.state == WPA_EAPOL_DONE && r[F_REPLAY + 7] == 3, "made-up: repeated msg 3 answered, no key reloaded");

    a.rc = 4;                                   /* group key handshake: GTK 2 */
    for (int i = 0; i < 16; i++)
        a.gtk[i] = (uint8_t)(0x40 + i);
    len = ap_group1(&a, f, 2 | 4);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.new_keys == WPA_EAPOL_NEW_GTK &&
          s.state == WPA_EAPOL_DONE, "made-up: group msg 1 taken");
    check(rlen == 99 && same_hex(r, 99, "0203005f0203020000000000000000000400000000000000000000000000000000000000"
                                        "000000000000000000000000000000000000000000000000000000000000000000000000"
                                        "0000000000000000003120ff95c333221db4607658bb8062ab0000"),
          "made-up: group msg 2 as Python's hmac has it");
    check(s.gtk_len == 16 && s.gtk_idx == 2 && s.gtk_tx == 1 && memcmp(s.gtk, a.gtk, 16) == 0 &&
          s.gtk_rsc[0] == 0x34 && s.gtk_rsc[1] == 0x12, "made-up: GTK 2, Tx bit, receive counter");
    a.rc = 5;
    len = ap_group1(&a, f, 2 | 4);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && rlen == 99 && s.new_keys == 0,
          "made-up: repeated group msg 1 answered, GTK not reloaded");

    a.rc = 6;                                   /* PTK rekey: a new 4-way handshake, a new SNonce, a new TK */
    a.anonce[0] ^= 0xFF;
    check(run_msg12(&a, &s) && s.state == WPA_EAPOL_WAIT_MSG3 && s.snonce[31] == 0x10 + 31 + 1,
          "made-up: rekey msg 1 and 2, a new SNonce");
    a.rc = 7;
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 2 | 4);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.new_keys == WPA_EAPOL_NEW_PTK &&
          memcmp(s.tk, a.ptk + 32, 16) == 0 && memcmp(s.tk, tk1, 16) != 0, "made-up: rekey gives a new TK only");

    wpa_eapol_clear(&s);
    uint8_t zero[sizeof s] = {0};
    check(memcmp(&s, zero, sizeof s) == 0, "made-up: clear wipes the context");
}

/* ---- 3. refusals */

/* a station that has answered msg 1 (counter 1); the AP ready with its PTK, counter 2 */
static void at_msg3(struct ap *a, struct wpa_eapol *s, struct nonces *nc)
{
    ap_init(a);
    sta_init(s, a, nc);
    run_msg12(a, s);
    a->rc = 2;
}

/* ... and one that has finished the 4-way handshake (counter 2); AP at counter 3 */
static void at_done(struct ap *a, struct wpa_eapol *s, struct nonces *nc)
{
    uint8_t f[512], r[WPA_EAPOL_REPLY_MAX];
    size_t rlen, len;
    at_msg3(a, s, nc);
    len = ap_msg3(a, f, a->rsne, a->rsne_len, 1);
    wpa_eapol_rx(s, f, len, r, sizeof r, &rlen);
    a->rc = 3;
}

/* the frame refused with reason `want`, no reply, the state `state` afterwards */
static void refused(struct wpa_eapol *s, const uint8_t *f, size_t len, enum wpa_eapol_reason want, int state,
                    const char *what)
{
    uint8_t r[WPA_EAPOL_REPLY_MAX];
    size_t rlen = 1;
    enum wpa_eapol_reason got = wpa_eapol_rx(s, f, len, r, sizeof r, &rlen);
    char m[160];
    snprintf(m, sizeof m, "refused: %s (reason %d, want %d; state %d, want %d)", what, got, want, s->state, state);
    check(got == want && s->reason == want && rlen == 0 && s->new_keys == 0 && s->state == state, m);
}

static void test_refusals(void)
{
    struct ap a;
    struct wpa_eapol s;
    struct nonces nc;
    uint8_t f[512], g[512], r[WPA_EAPOL_REPLY_MAX], kd[300], p[256];
    size_t len, rlen, n, kdl;
    char what[96];
    static const uint8_t zero16[16], zero48[48];

    /* the 802.1X and descriptor headers */
    ap_init(&a);
    sta_init(&s, &a, &nc);
    len = ap_msg1(&a, f);
    for (size_t l = 0; l < len; l++) {          /* every truncation of msg 1 */
        snprintf(what, sizeof what, "msg 1 cut to %zu bytes", l);
        refused(&s, f, l, WPA_EAPOL_E_SHORT, WPA_EAPOL_WAIT_MSG1, what);
    }
    memcpy(g, f, len);
    g[1] = 0;
    refused(&s, g, len, WPA_EAPOL_E_NOT_KEY, WPA_EAPOL_WAIT_MSG1, "an EAP packet");
    memcpy(g, f, len);
    g[4] = 254;
    refused(&s, g, len, WPA_EAPOL_E_DESC_TYPE, WPA_EAPOL_WAIT_MSG1, "descriptor type 254 (WPA)");
    for (unsigned v = 0; v < 8; v++) {
        if (v == 2)
            continue;
        ap_frame(g, (MSG1_INFO & ~7u) | v, 16, 1, a.anonce, 0, kd, 0, 0);
        snprintf(what, sizeof what, "msg 1 with key descriptor version %u", v);
        refused(&s, g, len, WPA_EAPOL_E_VERSION, WPA_EAPOL_WAIT_MSG1, what);
    }
    ap_frame(g, MSG1_INFO | KI_REQUEST, 16, 1, a.anonce, 0, kd, 0, 0);
    refused(&s, g, len, WPA_EAPOL_E_KEY_INFO, WPA_EAPOL_WAIT_MSG1, "request bit");
    ap_frame(g, MSG1_INFO & ~KI_ACK, 16, 1, a.anonce, 0, kd, 0, 0);
    refused(&s, g, len, WPA_EAPOL_E_KEY_INFO, WPA_EAPOL_WAIT_MSG1, "no ack bit");
    ap_frame(g, MSG1_INFO | KI_INSTALL, 16, 1, a.anonce, 0, kd, 0, 0);
    refused(&s, g, len, WPA_EAPOL_E_KEY_INFO, WPA_EAPOL_WAIT_MSG1, "msg 1 with install");
    memcpy(g, f, len);
    g[3] += 1;                                  /* body length one past the frame */
    refused(&s, g, len, WPA_EAPOL_E_SHORT, WPA_EAPOL_WAIT_MSG1, "body length past the frame");
    memcpy(g, f, len);
    g[F_KDLEN + 1] = 1;                         /* key data length past the body */
    refused(&s, g, len, WPA_EAPOL_E_SHORT, WPA_EAPOL_WAIT_MSG1, "key data length past the body");
    check(wpa_eapol_rx(&s, f, len, r, 99 + a.rsne_len - 1, &rlen) == WPA_EAPOL_E_BUF && rlen == 0 &&
          s.state == WPA_EAPOL_WAIT_MSG1, "refused: reply buffer one byte short");
    memcpy(g, f, len);
    g[len] = 0xEE;                              /* bytes past the body length (Ethernet padding) are ignored */
    check(wpa_eapol_rx(&s, g, len + 1, r, sizeof r, &rlen) == WPA_EAPOL_OK && rlen == 99 + a.rsne_len,
          "trailing bytes after the body ignored");

    /* order and counters */
    at_msg3(&a, &s, &nc);
    a.rc = 1;
    len = ap_msg1(&a, f);
    refused(&s, f, len, WPA_EAPOL_E_REPLAY, WPA_EAPOL_WAIT_MSG3, "msg 1 again with the counter answered");
    a.rc = 1;
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 1);
    refused(&s, f, len, WPA_EAPOL_E_REPLAY, WPA_EAPOL_WAIT_MSG3, "msg 3 with msg 1's counter");
    a.rc = 2;
    len = ap_group1(&a, f, 2);
    refused(&s, f, len, WPA_EAPOL_E_STATE, WPA_EAPOL_WAIT_MSG3, "group msg 1 before the 4-way handshake ends");
    ap_init(&a);
    sta_init(&s, &a, &nc);
    memset(a.ptk, 0x55, sizeof a.ptk);
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 1);
    refused(&s, f, len, WPA_EAPOL_E_STATE, WPA_EAPOL_WAIT_MSG1, "msg 3 before msg 1");

    at_msg3(&a, &s, &nc);
    a.anonce[5] ^= 1;
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 1);
    refused(&s, f, len, WPA_EAPOL_E_NONCE, WPA_EAPOL_WAIT_MSG3, "msg 3 with another ANonce");
    a.anonce[5] ^= 1;
    len = ap_msg3(&a, f, a.rsne, a.rsne_len, 1);
    memcpy(g, f, len);
    g[F_KEYLEN + 1] = 32;
    test_mic(a.ptk, g, len, g + F_MIC);
    refused(&s, g, len, WPA_EAPOL_E_KEY_LEN, WPA_EAPOL_WAIT_MSG3, "msg 3 with key length 32");
    ap_frame(g, MSG3_INFO & ~KI_ENC, 16, 2, a.anonce, 0, f + F_KD, len - 99, a.ptk);
    refused(&s, g, len, WPA_EAPOL_E_KEY_INFO, WPA_EAPOL_WAIT_MSG3, "msg 3 without encrypted key data");
    ap_frame(g, (MSG3_INFO & ~7u) | 1, 16, 2, a.anonce, 0, f + F_KD, len - 99, a.ptk);
    refused(&s, g, len, WPA_EAPOL_E_VERSION, WPA_EAPOL_WAIT_MSG3, "msg 3 with key descriptor version 1");
    memcpy(g, f, len);
    g[F_MIC + 15] ^= 0x80;
    refused(&s, g, len, WPA_EAPOL_E_MIC, WPA_EAPOL_WAIT_MSG3, "msg 3 with its MIC one bit off");
    memcpy(g, f, len);
    g[len - 1] ^= 1;
    refused(&s, g, len, WPA_EAPOL_E_MIC, WPA_EAPOL_WAIT_MSG3, "msg 3 with its key data one bit off");
    for (size_t i = 0; i < len; i++)            /* every byte changed: refused, and none of it taken */
        for (int bit = 0; bit < 8; bit += 7) {
            uint8_t rr[WPA_EAPOL_REPLY_MAX];
            memcpy(g, f, len);
            g[i] ^= (uint8_t)(1 << bit);
            snprintf(what, sizeof what, "msg 3 byte %zu bit %d changed", i, bit);
            enum wpa_eapol_reason got = wpa_eapol_rx(&s, g, len, rr, sizeof rr, &rlen);
            check(got != WPA_EAPOL_OK && rlen == 0 && s.state == WPA_EAPOL_WAIT_MSG3 && !s.have_replay, what);
        }
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.state == WPA_EAPOL_DONE,
          "msg 3 still taken after all those refusals");
    refused(&s, f, len, WPA_EAPOL_E_REPLAY, WPA_EAPOL_DONE, "msg 3 again with the same counter");

    /* key data length: not a multiple of 8, too short, too long (each with a valid MIC) */
    for (size_t l = 0; l <= 40; l++) {
        at_msg3(&a, &s, &nc);
        if (l % 8 == 0 && l >= 24)
            continue;
        memset(kd, 0x5A, l);
        len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, l, a.ptk);
        snprintf(what, sizeof what, "msg 3 with %zu bytes of key data", l);
        refused(&s, f, len, WPA_EAPOL_E_KEY_DATA_LEN, WPA_EAPOL_WAIT_MSG3, what);
    }
    at_msg3(&a, &s, &nc);
    memset(kd, 0x5A, 264);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, 264, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_KEY_DATA_LEN, WPA_EAPOL_WAIT_MSG3, "msg 3 with 264 bytes of key data");

    /* after a valid MIC: the RSNE, the unwrap, the GTK KDE. These end the association and wipe the keys. */
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len);
    p[a.rsne_len - 2] = 0x0C;                   /* RSN capabilities changed */
    len = ap_msg3(&a, f, p, a.rsne_len, 1);
    refused(&s, f, len, WPA_EAPOL_E_RSNE, WPA_EAPOL_FAILED, "msg 3 RSNE not the beacon's (capabilities)");
    check(!memcmp(s.tptk, zero48, 48) && s.gtk_len == 0, "refused: keys wiped on failure");
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_E_STATE && rlen == 0 &&
          s.reason == WPA_EAPOL_E_RSNE, "refused: anything once failed, the reason kept");
    at_msg3(&a, &s, &nc);
    p[a.rsne_len - 2] = 0;
    p[7] = 2;                                   /* group cipher TKIP: a downgrade */
    len = ap_msg3(&a, f, p, a.rsne_len, 1);
    refused(&s, f, len, WPA_EAPOL_E_RSNE, WPA_EAPOL_FAILED, "msg 3 RSNE not the beacon's (group cipher)");
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len - 2);
    p[1] -= 2;
    len = ap_msg3(&a, f, p, a.rsne_len - 2, 1);
    refused(&s, f, len, WPA_EAPOL_E_RSNE, WPA_EAPOL_FAILED, "msg 3 RSNE shorter than the beacon's");
    at_msg3(&a, &s, &nc);
    n = gtk_kde(p, a.gtk, 16, 1);
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_RSNE, WPA_EAPOL_FAILED, "msg 3 without an RSNE");
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len);
    memcpy(p + a.rsne_len, a.rsne, a.rsne_len);
    n = 2 * a.rsne_len + gtk_kde(p + 2 * a.rsne_len, a.gtk, 16, 1);
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_RSNE, WPA_EAPOL_FAILED, "msg 3 with a second RSNE");
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len);
    kdl = ap_key_data(p, a.rsne_len, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_GTK, WPA_EAPOL_FAILED, "msg 3 without a GTK KDE");
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len);
    n = a.rsne_len + gtk_kde(p + a.rsne_len, a.gtk, 4, 1);
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_GTK, WPA_EAPOL_FAILED, "msg 3 with a 4-byte GTK");
    at_msg3(&a, &s, &nc);                       /* two GTK KDEs */
    memcpy(p, a.rsne, a.rsne_len);
    n = a.rsne_len + gtk_kde(p + a.rsne_len, a.gtk, 16, 1);
    n += gtk_kde(p + n, a.gtk, 16, 2);
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_GTK, WPA_EAPOL_FAILED, "msg 3 with two GTK KDEs");
    at_msg3(&a, &s, &nc);                       /* a PMKID KDE (type 4) where the GTK KDE should be */
    memcpy(p, a.rsne, a.rsne_len);
    n = a.rsne_len + gtk_kde(p + a.rsne_len, a.gtk, 16, 1);
    p[a.rsne_len + 5] = 4;
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_GTK, WPA_EAPOL_FAILED, "msg 3 with a KDE of type 4 and no GTK KDE");
    at_msg3(&a, &s, &nc);                       /* an IGTK KDE (type 9) and an unknown element before the GTK KDE */
    memcpy(p, a.rsne, a.rsne_len);
    n = a.rsne_len + gtk_kde(p + a.rsne_len, a.gtk + 1, 15, 3);
    p[a.rsne_len + 5] = 9;
    p[n++] = 221;
    p[n++] = 3;
    p[n++] = 0x00;
    p[n++] = 0x0F;
    p[n++] = 0xAC;
    n += gtk_kde(p + n, a.gtk, 16, 1);
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.gtk_idx == 1 && s.gtk_len == 16 &&
          memcmp(s.gtk, a.gtk, 16) == 0, "msg 3: other KDEs and elements skipped, the GTK KDE taken");
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len);
    n = a.rsne_len + gtk_kde(p + a.rsne_len, a.gtk, 16, 1);
    p[a.rsne_len + 1] += 8;                     /* the KDE's length past the key data */
    kdl = ap_key_data(p, n, a.ptk + 16, kd);
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_GTK, WPA_EAPOL_FAILED, "msg 3 with a KDE longer than the key data");
    at_msg3(&a, &s, &nc);
    memcpy(p, a.rsne, a.rsne_len);
    n = a.rsne_len + gtk_kde(p + a.rsne_len, a.gtk, 16, 1);
    {
        uint8_t kek[16];
        memcpy(kek, a.ptk + 16, 16);
        kek[0] ^= 1;
        kdl = ap_key_data(p, n, kek, kd);
    }
    len = ap_frame(f, MSG3_INFO, 16, 2, a.anonce, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_UNWRAP, WPA_EAPOL_FAILED, "msg 3 key data wrapped under another KEK");
    ap_init(&a);
    sta_init(&s, &a, &nc);
    nc.fail = 1;
    len = ap_msg1(&a, f);
    refused(&s, f, len, WPA_EAPOL_E_RNG, WPA_EAPOL_FAILED, "msg 1 when the nonce source fails");

    /* group key handshake */
    at_done(&a, &s, &nc);
    len = ap_group1(&a, f, 2);
    memcpy(g, f, len);
    g[F_MIC] ^= 1;
    refused(&s, g, len, WPA_EAPOL_E_MIC, WPA_EAPOL_DONE, "group msg 1 with a bad MIC");
    for (size_t i = 0; i < len; i++) {
        uint8_t rr[WPA_EAPOL_REPLY_MAX];
        memcpy(g, f, len);
        g[i] ^= 0x80;
        snprintf(what, sizeof what, "group msg 1 byte %zu changed", i);
        enum wpa_eapol_reason got = wpa_eapol_rx(&s, g, len, rr, sizeof rr, &rlen);
        check(got != WPA_EAPOL_OK && rlen == 0 && s.state == WPA_EAPOL_DONE && s.gtk_idx == 1 && s.replay[7] == 2,
              what);
    }
    a.rc = 2;
    len = ap_group1(&a, f, 2);
    refused(&s, f, len, WPA_EAPOL_E_REPLAY, WPA_EAPOL_DONE, "group msg 1 with msg 3's counter");
    a.rc = 1ull << 40;
    len = ap_group1(&a, f, 2);
    check(wpa_eapol_rx(&s, f, len, r, sizeof r, &rlen) == WPA_EAPOL_OK && s.gtk_idx == 2, "group msg 1 taken");
    a.rc = 3;
    len = ap_group1(&a, f, 1);
    refused(&s, f, len, WPA_EAPOL_E_REPLAY, WPA_EAPOL_DONE, "group msg 1 with a counter below the last");
    a.rc = 2;
    len = ap_msg1(&a, f);
    refused(&s, f, len, WPA_EAPOL_E_REPLAY, WPA_EAPOL_DONE, "msg 1 with a counter below the last valid MIC's");
    a.rc = (1ull << 40) + 1;
    memset(kd, 0x5A, 20);
    len = ap_frame(f, GROUP1_INFO, 0, a.rc, 0, 0, kd, 20, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_KEY_DATA_LEN, WPA_EAPOL_DONE, "group msg 1 with 20 bytes of key data");
    n = gtk_kde(p, a.gtk, 16, 1);
    {
        uint8_t kek[16];
        memcpy(kek, a.ptk + 16, 16);
        kek[15] ^= 0x80;
        kdl = ap_key_data(p, n, kek, kd);
    }
    len = ap_frame(f, GROUP1_INFO, 0, a.rc, 0, 0, kd, kdl, a.ptk);
    refused(&s, f, len, WPA_EAPOL_E_UNWRAP, WPA_EAPOL_FAILED, "group msg 1 wrapped under another KEK");
    check(!memcmp(s.tk, zero16, 16) && !memcmp(s.ptk, zero48, 48) && !memcmp(s.tptk, zero48, 48) &&
          !memcmp(s.gtk, zero48, 32) && s.gtk_len == 0, "refused: TK, PTK and GTK wiped on a failure once done");

    /* init */
    {
        uint8_t bad[64];
        size_t bl = unhex(rsne_hex, bad);
        ap_init(&a);
        bad[1]++;
        check(wpa_eapol_init(&s, a.pmk, a.spa, a.aa, bad, bl, a.rsne, a.rsne_len, nonce_cb, &nc) == -1 &&
              s.state == WPA_EAPOL_FAILED, "refused: init with an RSNE whose length byte is wrong");
        bad[1]--;
        bad[0] = 221;
        check(wpa_eapol_init(&s, a.pmk, a.spa, a.aa, a.rsne, a.rsne_len, bad, bl, nonce_cb, &nc) == -1,
              "refused: init with an AP element that is not an RSNE");
        check(wpa_eapol_init(&s, a.pmk, a.spa, a.aa, a.rsne, a.rsne_len, a.rsne, a.rsne_len, 0, 0) == -1,
              "refused: init without a nonce source");
    }
}

int main(void)
{
    test_aes();
    test_capture();
    test_made_up();
    test_refusals();
    printf("wifi_eapol: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
