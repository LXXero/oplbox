/* SPDX-License-Identifier: GPL-3.0-only
 * The AP side of the WPA2-PSK handshakes, for the host tests (tests/wifi/wpa_ap.h). Own code.
 */
#include <string.h>
#include "wpa_ap.h"

/* ---- AES-128 encryption (FIPS-197) and RFC 3394 2.2.1 key wrap, for the AP side only */

uint8_t wpa_ap_sbox[256];

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

void wpa_ap_aes_init(void)                     /* FIPS-197 5.1.1: the inverse in GF(2^8), then the affine map */
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
        wpa_ap_sbox[i] = s ^ 0x63;
    }
}

void wpa_ap_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[176], s[16], t[16], rcon = 1;
    memcpy(rk, key, 16);
    for (int i = 16; i < 176; i += 4) {
        uint8_t w[4];
        memcpy(w, rk + i - 4, 4);
        if (i % 16 == 0) {
            uint8_t u = w[0];
            w[0] = wpa_ap_sbox[w[1]] ^ rcon;
            w[1] = wpa_ap_sbox[w[2]];
            w[2] = wpa_ap_sbox[w[3]];
            w[3] = wpa_ap_sbox[u];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; k++)
            rk[i + k] = rk[i - 16 + k] ^ w[k];
    }
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        for (int i = 0; i < 16; i++)            /* SubBytes and ShiftRows: row i%4 moves left by i%4 columns */
            t[i] = wpa_ap_sbox[s[((i / 4 + i % 4) % 4) * 4 + i % 4]];
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
void wpa_ap_wrap(const uint8_t kek[16], size_t n, const uint8_t *plain, uint8_t *cipher)
{
    uint8_t b[16];
    memset(b, 0xA6, 8);
    memcpy(cipher + 8, plain, n * 8);
    for (unsigned j = 0; j <= 5; j++)
        for (size_t i = 1; i <= n; i++) {
            uint32_t t = (uint32_t)(n * j + i);
            memcpy(b + 8, cipher + i * 8, 8);
            wpa_ap_aes128(kek, b, b);
            b[4] ^= (uint8_t)(t >> 24);
            b[5] ^= (uint8_t)(t >> 16);
            b[6] ^= (uint8_t)(t >> 8);
            b[7] ^= (uint8_t)t;
            memcpy(cipher + i * 8, b + 8, 8);
        }
    memcpy(cipher, b, 8);
}


/* ---- frames */

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* the MIC as 12.7.2 has it, done on a copy with the field zeroed (not the library's incremental form) */
void wpa_ap_mic(const uint8_t *kck, const uint8_t *f, size_t len, uint8_t out[16])
{
    uint8_t c[512], h[20];
    memcpy(c, f, len);
    memset(c + F_MIC, 0, 16);
    wpa_hmac_sha1(kck, 16, c, len, h);
    memcpy(out, h, 16);
}

int wpa_ap_mic_ok(const uint8_t *kck, const uint8_t *f, size_t len)
{
    uint8_t m[16];
    wpa_ap_mic(kck, f, len, m);
    return memcmp(m, f + F_MIC, 16) == 0;
}

/* An AP's EAPOL-Key frame. kd/kdl are the key data as sent (already wrapped); kck NULL leaves the MIC zero. */
size_t wpa_ap_frame(uint8_t *f, unsigned info, unsigned keylen, uint64_t rc, const uint8_t *nonce,
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
        wpa_ap_mic(kck, f, len, f + F_MIC);
    return len;
}

/* key data padded as 12.7.2 has it (0xDD then zeros, to a multiple of 8 and at least 16) and wrapped */
size_t wpa_ap_key_data(const uint8_t *plain, size_t n, const uint8_t *kek, uint8_t *out)
{
    uint8_t p[256];
    size_t m = n < 16 ? 16 : (n + 7) / 8 * 8;
    memcpy(p, plain, n);
    if (m > n) {
        p[n] = 0xDD;
        memset(p + n + 1, 0, m - n - 1);
    }
    wpa_ap_wrap(kek, m / 8, p, out);
    return m + 8;
}

/* GTK KDE: DD, length, 00-0F-AC, 1, key ID / Tx, reserved, GTK */
size_t wpa_ap_gtk_kde(uint8_t *p, const uint8_t *gtk, size_t len, uint8_t key_id)
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

/* ---- the authenticator */

void wpa_auth_init(struct wpa_auth *a, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                   const uint8_t *rsne, size_t rsne_len, const uint8_t *sta_rsne, size_t sta_rsne_len,
                   const uint8_t gtk[16], uint8_t gtk_idx, uint8_t seed)
{
    memset(a, 0, sizeof *a);
    if (!wpa_ap_sbox[0]) wpa_ap_aes_init();
    memcpy(a->pmk, pmk, 32);
    memcpy(a->aa, aa, 6);
    memcpy(a->spa, spa, 6);
    memcpy(a->rsne, rsne, rsne_len);
    a->rsne_len = rsne_len;
    memcpy(a->sta_rsne, sta_rsne, sta_rsne_len);
    a->sta_rsne_len = sta_rsne_len;
    memcpy(a->gtk, gtk, 16);
    a->gtk_idx = gtk_idx;
    for (int i = 0; i < 32; i++) a->anonce[i] = (uint8_t)(seed + 7 * i);
}

size_t wpa_auth_msg1(struct wpa_auth *a, uint8_t *f)
{
    static const uint8_t z[1];
    a->stage = WPA_AUTH_MSG1;
    return wpa_ap_frame(f, MSG1_INFO, 16, ++a->rc, a->anonce, 0, z, 0, 0);
}

size_t wpa_auth_msg3(struct wpa_auth *a, uint8_t *f)
{
    uint8_t p[128], kd[144];
    size_t n = a->rsne_len;
    memcpy(p, a->rsne, n);
    n += wpa_ap_gtk_kde(p + n, a->gtk, 16, a->gtk_idx);
    size_t kdl = wpa_ap_key_data(p, n, a->ptk + 16, kd);
    a->stage = WPA_AUTH_MSG3;
    return wpa_ap_frame(f, MSG3_INFO, 16, ++a->rc, a->anonce, 0, kd, kdl, a->ptk);
}

size_t wpa_auth_group1(struct wpa_auth *a, uint8_t *f)
{
    uint8_t p[64], kd[72];
    size_t n = wpa_ap_gtk_kde(p, a->gtk, 16, a->gtk_idx);
    size_t kdl = wpa_ap_key_data(p, n, a->ptk + 16, kd);
    a->stage = WPA_AUTH_GROUP1;
    return wpa_ap_frame(f, GROUP1_INFO, 0, ++a->rc, 0, 0, kd, kdl, a->ptk);
}

int wpa_auth_take(struct wpa_auth *a, const uint8_t *f, size_t n)
{
    unsigned info;
    uint64_t rc = 0;
    if (n < 99 || f[1] != 3 || f[4] != 2) return 0;
    info = (unsigned)f[F_INFO] << 8 | f[F_INFO + 1];
    for (int i = 0; i < 8; i++) rc = rc << 8 | f[F_REPLAY + i];
    if (rc != a->rc || !(info & KI_MIC) || (info & KI_ACK)) return 0;
    if ((info & KI_P) && !(info & KI_SECURE) && a->stage == WPA_AUTH_MSG1) {
        uint8_t ptk[48];
        size_t kdl = (size_t)f[F_KDLEN] << 8 | f[F_KDLEN + 1];
        wpa_ptk(a->pmk, a->aa, a->spa, a->anonce, f + F_NONCE, ptk);
        if (!wpa_ap_mic_ok(ptk, f, n) || kdl != a->sta_rsne_len || 99 + kdl > n || memcmp(f + F_KD, a->sta_rsne, kdl))
            return 0;
        memcpy(a->ptk, ptk, 48);
        a->have_ptk = 1;
        return 2;
    }
    if (!a->have_ptk || !wpa_ap_mic_ok(a->ptk, f, n)) return 0;
    if ((info & KI_P) && (info & KI_SECURE) && a->stage == WPA_AUTH_MSG3) { a->stage = WPA_AUTH_DONE; return 4; }
    if (!(info & KI_P) && (info & KI_SECURE) && a->stage == WPA_AUTH_GROUP1) { a->stage = WPA_AUTH_DONE; return 6; }
    return 0;
}
