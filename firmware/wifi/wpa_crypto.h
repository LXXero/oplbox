/* SPDX-License-Identifier: GPL-3.0-only
 * WPA2-PSK key primitives (docs/WIFI.md stage 8, the host part): the PMK from the passphrase (PBKDF2-HMAC-SHA1,
 * 4096 iterations, in steps so the main loop can spread it), the IEEE 802.11 PRF that gives the PTK (and the
 * GTK on an authenticator), and AES key unwrap (RFC 3394) for the GTK in EAPOL-Key key data. No EAPOL here.
 * Own code, written from IEEE 802.11-2020 (12.7.1.2 PRF, 12.7.1.3 pairwise key hierarchy, J.4 passphrase to
 * PSK) and RFC 8018 (PBKDF2) / RFC 3394 (key wrap). SHA-1, HMAC and AES come from BearSSL (bearssl/, MIT).
 * Nothing here touches a register, and nothing has run on an FM-1.
 */
#ifndef WPA_CRYPTO_H
#define WPA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>
#include "bearssl/bearssl_hmac.h"

#define WPA_PMK_LEN 32
#define WPA_PMK_ITERATIONS 4096         /* IEEE 802.11 J.4.1 */
/* Iterations a main-loop pass runs by default: 8192 in all (two 20-byte PBKDF2 blocks for the 32-byte PMK),
 * each two SHA-1 compressions. 64 per pass is a guess until stage 8 times one on the FM-1 [unchecked]. */
#define WPA_PMK_STEP 64

/* A PMK being derived. Holds the HMAC key state made from the passphrase, not the passphrase itself. */
struct wpa_pmk_job {
    br_hmac_key_context key;
    uint8_t salt[32 + 4];       /* SSID, then the block index INT(i) */
    uint8_t salt_len;           /* SSID length */
    uint8_t block;              /* 1 or 2; 3 once done */
    uint16_t iter;              /* iterations done in this block */
    uint8_t u[20], t[20];       /* U_j and T_i of RFC 8018 5.2 */
    uint8_t pmk[WPA_PMK_LEN];
};

/* Start a PMK. The passphrase is 8-63 printable ASCII characters (32-126) and the SSID 1-32 bytes
 * (IEEE 802.11 J.4.1); anything else returns -1 and leaves the job finished with no key. 0 when started. */
int wpa_pmk_begin(struct wpa_pmk_job *j, const char *pass, size_t pass_len, const uint8_t *ssid, size_t ssid_len);
/* Run at most n iterations. Returns 1 once the PMK is ready (copy it with wpa_pmk_take), else 0. */
int wpa_pmk_step(struct wpa_pmk_job *j, unsigned n);
/* Copy the finished PMK out and wipe the job. -1, job untouched, while it is still running; -1 too when the
 * start was refused or the key was already taken. */
int wpa_pmk_take(struct wpa_pmk_job *j, uint8_t pmk[WPA_PMK_LEN]);
/* Wipe the job (an abandoned derivation). */
void wpa_pmk_clear(struct wpa_pmk_job *j);

/* HMAC-SHA1 of data with key (20 bytes out). The EAPOL-Key MIC (key descriptor version 2) is its first 16. */
void wpa_hmac_sha1(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[20]);

/* PRF-(8 * out_len) of IEEE 802.11 12.7.1.2: HMAC-SHA1(K, A || 0 || B || i) for i = 0, 1, ..., concatenated
 * and cut to out_len bytes. label is A without its NUL. out_len at most 255 * 20. */
void wpa_prf_sha1(const uint8_t *key, size_t key_len, const char *label, const uint8_t *data, size_t len,
                  uint8_t *out, size_t out_len);

/* The CCMP PTK (12.7.1.3): PRF-384(PMK, "Pairwise key expansion", Min(AA, SPA) || Max(AA, SPA) ||
 * Min(ANonce, SNonce) || Max(ANonce, SNonce)). KCK, KEK and TK are its bytes 0-15, 16-31 and 32-47. */
#define WPA_PTK_LEN 48
void wpa_ptk(const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6], const uint8_t spa[6], const uint8_t anonce[32],
             const uint8_t snonce[32], uint8_t ptk[WPA_PTK_LEN]);

/* AES key unwrap (RFC 3394 2.2.2): cipher holds n + 1 64-bit blocks, plain receives n (n >= 2). kek is 16, 24
 * or 32 bytes (WPA2's KEK is 16). Returns 0 when the integrity check value is A6A6A6A6A6A6A6A6, else -1 with
 * plain zeroed. plain may not overlap cipher. */
int wpa_aes_unwrap(const uint8_t *kek, size_t kek_len, size_t n, const uint8_t *cipher, uint8_t *plain);

#endif
