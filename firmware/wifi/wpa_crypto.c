/* SPDX-License-Identifier: GPL-3.0-only
 * WPA2-PSK key primitives (see wpa_crypto.h). Own code from IEEE 802.11-2020 12.7.1.2-12.7.1.3 and J.4, RFC 8018
 * 5.2 (PBKDF2) and RFC 3394 2.2.2 (key unwrap, index-based form). SHA-1, HMAC and AES-128/192/256 decryption
 * are BearSSL's (bearssl/, MIT): br_sha1, br_hmac, br_aes_keysched + br_aes_small_decrypt.
 */
#include "wpa_crypto.h"
#include "bearssl/inner.h"

/* memset that the compiler keeps even when the buffer is not read again */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

static void hmac(const br_hmac_key_context *k, const void *d, size_t len, uint8_t out[20])
{
    br_hmac_context c;
    br_hmac_init(&c, k, 0);
    br_hmac_update(&c, d, len);
    br_hmac_out(&c, out);
    wipe(&c, sizeof c);
}

/* ---- PMK: PBKDF2-HMAC-SHA1(passphrase, SSID, 4096, 32), RFC 8018 5.2 with IEEE 802.11 J.4.1's parameters */

int wpa_pmk_begin(struct wpa_pmk_job *j, const char *pass, size_t pass_len, const uint8_t *ssid, size_t ssid_len)
{
    wipe(j, sizeof *j);
    j->block = 3;                                   /* finished, no key: what a refused start leaves */
    if (pass_len < 8 || pass_len > 63 || ssid_len < 1 || ssid_len > 32)
        return -1;
    for (size_t i = 0; i < pass_len; i++)
        if ((uint8_t)pass[i] < 32 || (uint8_t)pass[i] > 126)
            return -1;
    br_hmac_key_init(&j->key, &br_sha1_vtable, pass, pass_len);
    memcpy(j->salt, ssid, ssid_len);
    j->salt_len = (uint8_t)ssid_len;
    j->block = 1;
    return 0;
}

int wpa_pmk_step(struct wpa_pmk_job *j, unsigned n)
{
    for (; n && j->block <= 2; n--) {
        if (j->iter == 0) {                         /* U_1 = PRF(P, S || INT(i)) */
            uint8_t *s = j->salt + j->salt_len;
            s[0] = s[1] = s[2] = 0;
            s[3] = j->block;
            hmac(&j->key, j->salt, j->salt_len + 4u, j->u);
            memcpy(j->t, j->u, 20);
        } else {                                    /* U_c = PRF(P, U_{c-1}), T_i ^= U_c */
            hmac(&j->key, j->u, 20, j->u);
            for (int k = 0; k < 20; k++)
                j->t[k] ^= j->u[k];
        }
        if (++j->iter == WPA_PMK_ITERATIONS) {      /* T_1 is PMK bytes 0-19, the first 12 of T_2 bytes 20-31 */
            memcpy(j->pmk + (j->block - 1) * 20, j->t, j->block == 1 ? 20 : WPA_PMK_LEN - 20);
            j->block++;
            j->iter = 0;
        }
    }
    return j->block > 2;
}

int wpa_pmk_take(struct wpa_pmk_job *j, uint8_t pmk[WPA_PMK_LEN])
{
    if (j->block <= 2)                              /* still running: left as it is */
        return -1;
    if (!j->key.dig_vtable)                         /* refused, or already taken */
        return -1;
    memcpy(pmk, j->pmk, WPA_PMK_LEN);
    wpa_pmk_clear(j);
    return 0;
}

void wpa_pmk_clear(struct wpa_pmk_job *j)
{
    wipe(j, sizeof *j);
    j->block = 3;
}

/* ---- HMAC-SHA1 and the IEEE 802.11 PRF */

void wpa_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[20])
{
    br_hmac_key_context k;
    br_hmac_key_init(&k, &br_sha1_vtable, key, key_len);
    hmac(&k, data, len, out);
    wipe(&k, sizeof k);
}

void wpa_prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t len,
                  uint8_t *out, size_t out_len)
{
    br_hmac_key_context k;
    br_hmac_context c;
    uint8_t r[20], zero = 0;
    size_t label_len = 0;
    while (label[label_len])
        label_len++;
    br_hmac_key_init(&k, &br_sha1_vtable, key, key_len);
    for (uint8_t i = 0; out_len; i++) {             /* R = R || HMAC-SHA1(K, A || Y || B || X), Y = 0, X = i */
        size_t m = out_len < 20 ? out_len : 20;
        br_hmac_init(&c, &k, 0);
        br_hmac_update(&c, label, label_len);
        br_hmac_update(&c, &zero, 1);
        br_hmac_update(&c, data, len);
        br_hmac_update(&c, &i, 1);
        br_hmac_out(&c, r);
        memcpy(out, r, m);
        out += m;
        out_len -= m;
    }
    wipe(&k, sizeof k);
    wipe(&c, sizeof c);
    wipe(r, sizeof r);
}

/* a < b as big-endian numbers of n bytes (the Min/Max of 12.7.1.3) */
static int less(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] < b[i];
    return 0;
}

void wpa_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
             const uint8_t snonce[32], uint8_t ptk[WPA_PTK_LEN])
{
    uint8_t d[6 + 6 + 32 + 32];
    int a = less(aa, spa, 6), n = less(anonce, snonce, 32);
    memcpy(d, a ? aa : spa, 6);
    memcpy(d + 6, a ? spa : aa, 6);
    memcpy(d + 12, n ? anonce : snonce, 32);
    memcpy(d + 44, n ? snonce : anonce, 32);
    wpa_prf_sha1(pmk, WPA_PMK_LEN, "Pairwise key expansion", d, sizeof d, ptk, WPA_PTK_LEN);
}

/* ---- AES key unwrap, RFC 3394 2.2.2 */

int wpa_aes_unwrap(const uint8_t *kek, size_t kek_len, size_t n, const uint8_t *cipher, uint8_t *plain)
{
    uint32_t skey[60];                              /* (14 + 1) round keys of 4 words, the most AES-256 needs */
    uint8_t b[16];
    unsigned rounds;
    uint8_t bad = 0;

    if (n < 2 || (rounds = br_aes_keysched(skey, kek, kek_len)) == 0) {
        if (n)
            wipe(plain, n * 8);
        return -1;
    }
    memcpy(b, cipher, 8);                           /* A = C[0], R[i] = C[i] */
    memcpy(plain, cipher + 8, n * 8);
    for (int j = 5; j >= 0; j--)
        for (size_t i = n; i >= 1; i--) {           /* B = AES-1(K, (A ^ t) | R[i]), t = n * j + i */
            uint32_t t = (uint32_t)(n * (size_t)j + i);
            b[4] ^= (uint8_t)(t >> 24);
            b[5] ^= (uint8_t)(t >> 16);
            b[6] ^= (uint8_t)(t >> 8);
            b[7] ^= (uint8_t)t;
            memcpy(b + 8, plain + (i - 1) * 8, 8);
            br_aes_small_decrypt(rounds, skey, b);
            memcpy(plain + (i - 1) * 8, b + 8, 8);  /* A = MSB(64, B), R[i] = LSB(64, B) */
        }
    for (int k = 0; k < 8; k++)                     /* every byte compared, so the time does not say which */
        bad |= b[k] ^ 0xA6;
    wipe(skey, sizeof skey);
    wipe(b, sizeof b);
    if (bad) {
        wipe(plain, n * 8);
        return -1;
    }
    return 0;
}
