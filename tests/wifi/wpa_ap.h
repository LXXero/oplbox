/* SPDX-License-Identifier: GPL-3.0-only
 * The AP side of the WPA2-PSK EAPOL-Key handshakes, for the host tests only (never built into the firmware):
 * AES-128 (FIPS-197) and the RFC 3394 key wrap, EAPOL-Key frames as an AP sends them, and a small authenticator that
 * runs the 4-way and group key handshakes against a station. Moved out of tests/wifi_eapol.c (its made-up AP) so the
 * register model (tests/wifi/sdio_model.c, an "ap ... sec=wpa2 psk=..." line) and tests/wifi_wpa.c use the same AP.
 * Own code, from IEEE 802.11-2020 12.7.2, 12.7.6 and 12.7.7, on firmware/wifi/wpa_crypto.c's PRF, PTK and HMAC.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "wpa_crypto.h"

/* EAPOL-Key fields: byte offsets in the PDU (802.1X header first) */
#define F_INFO 5
#define F_KEYLEN 7
#define F_REPLAY 9
#define F_NONCE 17
#define F_RSC 65
#define F_MIC 81
#define F_KDLEN 97
#define F_KD 99
/* key information bits */
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

extern uint8_t wpa_ap_sbox[256];
void wpa_ap_aes_init(void);   /* the S-box; once before wpa_ap_aes128 / wpa_ap_wrap */
void wpa_ap_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* n 64-bit blocks of plain to n + 1 of cipher */
void wpa_ap_wrap(const uint8_t kek[16], size_t n, const uint8_t *plain, uint8_t *cipher);
/* the MIC as 12.7.2 has it, done on a copy with the field zeroed (not the library's incremental form) */
void wpa_ap_mic(const uint8_t *kck, const uint8_t *f, size_t len, uint8_t out[16]);
int wpa_ap_mic_ok(const uint8_t *kck, const uint8_t *f, size_t len);
/* An AP's EAPOL-Key frame. kd/kdl are the key data as sent (already wrapped); kck NULL leaves the MIC zero. */
size_t wpa_ap_frame(uint8_t *f, unsigned info, unsigned keylen, uint64_t rc, const uint8_t *nonce,
                    const uint8_t *rsc, const uint8_t *kd, size_t kdl, const uint8_t *kck);
/* key data padded as 12.7.2 has it (0xDD then zeros, to a multiple of 8 and at least 16) and wrapped */
size_t wpa_ap_key_data(const uint8_t *plain, size_t n, const uint8_t *kek, uint8_t *out);
/* GTK KDE: DD, length, 00-0F-AC, 1, key ID / Tx, reserved, GTK */
size_t wpa_ap_gtk_kde(uint8_t *p, const uint8_t *gtk, size_t len, uint8_t key_id);

/* ---- an authenticator: msg 1 and 3 and group msg 1 out, msg 2 and 4 and group msg 2 checked in */
enum { WPA_AUTH_IDLE, WPA_AUTH_MSG1, WPA_AUTH_MSG3, WPA_AUTH_DONE, WPA_AUTH_GROUP1 };
struct wpa_auth {
    uint8_t pmk[32], aa[6], spa[6], anonce[32], ptk[48], gtk[16];
    uint8_t rsne[64], sta_rsne[64];   /* its own (beacons, msg 3); the station's (association request, msg 2) */
    size_t rsne_len, sta_rsne_len;
    uint64_t rc;                      /* the last key replay counter sent */
    uint8_t gtk_idx, stage;           /* WPA_AUTH_*: where the handshake is */
    int have_ptk;
};
/* PMK, its address, the station's, both RSNEs, the GTK and its key ID; the ANonce from seed */
void wpa_auth_init(struct wpa_auth *a, const uint8_t pmk[32], const uint8_t aa[6], const uint8_t spa[6],
                   const uint8_t *rsne, size_t rsne_len, const uint8_t *sta_rsne, size_t sta_rsne_len,
                   const uint8_t gtk[16], uint8_t gtk_idx, uint8_t seed);
size_t wpa_auth_msg1(struct wpa_auth *a, uint8_t *f);     /* each a new replay counter; f holds 256 bytes */
size_t wpa_auth_msg3(struct wpa_auth *a, uint8_t *f);
size_t wpa_auth_group1(struct wpa_auth *a, uint8_t *f);   /* a new GTK (gtk_idx and gtk as set) after DONE */
/* A frame from the station: 2 (msg 2, its MIC good under the PTK its SNonce gives, its RSNE the association's),
 * 4 (msg 4), 6 (group msg 2); 0 when refused (wrong counter, bad MIC, not the message the handshake is at). */
int wpa_auth_take(struct wpa_auth *a, const uint8_t *f, size_t n);
