/* SPDX-License-Identifier: GPL-3.0-only
 * The station (supplicant) side of the WPA2-PSK EAPOL-Key handshakes (docs/WIFI.md stage 8, the host part): the
 * 4-way handshake and the group key handshake, AKM 2 (PSK) with CCMP as pairwise cipher, key descriptor version 2
 * (HMAC-SHA1-128 MIC, AES key wrap for the key data). Own code, written from IEEE 802.11-2020 12.7.2 (EAPOL-Key
 * frames), 12.7.6 (4-way handshake) and 12.7.7 (group key handshake), on wpa_crypto.c. No malloc.
 *
 * It takes one received EAPOL frame at a time (the 802.1X header onward: what follows LLC/SNAP with ethertype
 * 0x888E) and writes the frame to send back. The keys it agrees (TK for CCMP, GTK with its key index and receive
 * sequence counter) are left in the context for the caller to load into the MAC's key tables; nothing here touches
 * a register, and nothing has run on an FM-1.
 *
 * What the caller does with a result:
 *   - *reply_len > 0: send those bytes as an EAPOL frame to the AP (ethertype 0x888E). Message 4 goes out before
 *     the new pairwise key is loaded: it is sent unprotected (or under the old key on a rekey).
 *   - new_keys & WPA_EAPOL_NEW_PTK: load tk (CCMP, 16 bytes) as the pairwise key for the AP's address.
 *   - new_keys & WPA_EAPOL_NEW_GTK: load gtk (gtk_len bytes) as group key gtk_idx, receive counter gtk_rsc.
 *   - state WPA_EAPOL_FAILED: the AP sent a frame with a valid MIC that breaks the handshake (12.7.6.4: an RSNE
 *     that is not the beacon's, key data that does not unwrap, no GTK). Deauthenticate; wpa_eapol_init again
 *     before the next association.
 * Every other refusal (bad MIC, replayed counter, a malformed or unexpected frame) drops the frame and leaves the
 * state as it was, as 12.7.6 says ("discard"); `reason` says which check refused it.
 */
#ifndef WPA_EAPOL_H
#define WPA_EAPOL_H

#include <stddef.h>
#include <stdint.h>
#include "wpa_crypto.h"

#define WPA_EAPOL_RSNE_MAX 64           /* longest RSNE kept (element ID and length included) */
#define WPA_EAPOL_HDR 4                 /* 802.1X: protocol version, packet type, body length */
#define WPA_EAPOL_KEY_FIXED 95          /* EAPOL-Key body up to the key data, with a 16-byte MIC (12.7.2) */
#define WPA_EAPOL_KEY_DATA_MAX 256      /* longest key data taken (msg 3 with RSNE, GTK and IGTK is about 120) */
#define WPA_EAPOL_REPLY_MAX (WPA_EAPOL_HDR + WPA_EAPOL_KEY_FIXED + WPA_EAPOL_RSNE_MAX)
#define WPA_EAPOL_GTK_MAX 32            /* TKIP's; CCMP's is 16 */

enum wpa_eapol_state {
    WPA_EAPOL_WAIT_MSG1,                /* associated, nothing received */
    WPA_EAPOL_WAIT_MSG3,                /* message 2 sent */
    WPA_EAPOL_DONE,                     /* message 4 sent, keys agreed; group key handshakes and PTK rekeys go on */
    WPA_EAPOL_FAILED                    /* deauthenticate (see above) */
};

enum wpa_eapol_reason {
    WPA_EAPOL_OK,
    WPA_EAPOL_E_SHORT,                  /* frame shorter than its header, its body length or the key data length */
    WPA_EAPOL_E_NOT_KEY,                /* 802.1X packet type is not EAPOL-Key (3) */
    WPA_EAPOL_E_DESC_TYPE,              /* descriptor type is not 2 (IEEE 802.11) */
    WPA_EAPOL_E_VERSION,                /* key descriptor version is not 2 (HMAC-SHA1-128, AES key wrap) */
    WPA_EAPOL_E_KEY_INFO,               /* key information bits match none of msg 1, msg 3 or group msg 1 */
    WPA_EAPOL_E_KEY_LEN,                /* msg 3's key length is not CCMP's 16 */
    WPA_EAPOL_E_STATE,                  /* a message the handshake is not at (msg 3 before msg 1, group before done) */
    WPA_EAPOL_E_REPLAY,                 /* key replay counter not above every one taken before */
    WPA_EAPOL_E_NONCE,                  /* msg 3's ANonce is not msg 1's */
    WPA_EAPOL_E_KEY_DATA_LEN,           /* encrypted key data not a multiple of 8, under 24 bytes or too long */
    WPA_EAPOL_E_MIC,                    /* MIC does not verify */
    WPA_EAPOL_E_RSNE,                   /* msg 3's RSNE missing or not the beacon's: FAILED */
    WPA_EAPOL_E_UNWRAP,                 /* key data fails the AES key unwrap integrity check: FAILED */
    WPA_EAPOL_E_GTK,                    /* no GTK KDE, or a malformed one or malformed key data: FAILED */
    WPA_EAPOL_E_RNG,                    /* the nonce source failed: FAILED */
    WPA_EAPOL_E_BUF                     /* reply buffer smaller than the reply */
};

#define WPA_EAPOL_NEW_PTK 1
#define WPA_EAPOL_NEW_GTK 2

/* Fills out[32] with a fresh SNonce (12.7.5: random); returns 0, or nonzero when it has none. */
typedef int (*wpa_eapol_nonce_fn)(void *arg, uint8_t out[32]);

struct wpa_eapol {
    /* set by wpa_eapol_init */
    uint8_t pmk[WPA_PMK_LEN];
    uint8_t spa[6], aa[6];              /* own MAC (supplicant), the AP's (authenticator) */
    uint8_t own_rsne[WPA_EAPOL_RSNE_MAX], own_rsne_len;     /* sent in the association request; msg 2 repeats it */
    uint8_t ap_rsne[WPA_EAPOL_RSNE_MAX], ap_rsne_len;       /* from the beacon / probe response; msg 3 must match */
    wpa_eapol_nonce_fn nonce;
    void *nonce_arg;
    /* the handshake */
    uint8_t state, reason, new_keys;    /* enum wpa_eapol_state, enum wpa_eapol_reason, WPA_EAPOL_NEW_* of last rx */
    uint8_t have_replay, have_msg1;
    uint8_t replay[8];                  /* counter of the last frame whose MIC verified */
    uint8_t msg1_replay[8];             /* counter of the last msg 1 answered */
    uint8_t anonce[32], snonce[32];
    uint8_t tptk[WPA_PTK_LEN];          /* from the last msg 1; becomes ptk when msg 3 verifies under it (12.7.6.4) */
    uint8_t ptk[WPA_PTK_LEN];           /* KCK 0-15, KEK 16-31, TK 32-47 */
    /* the keys for the MAC */
    uint8_t tk[16];
    uint8_t gtk[WPA_EAPOL_GTK_MAX], gtk_len, gtk_idx, gtk_tx;
    uint8_t gtk_rsc[8];                 /* Key RSC field as sent: the PN, least significant byte first */
};

/* Start a handshake for one association. own_rsne/ap_rsne are whole elements (ID 48, length, body), at most
 * WPA_EAPOL_RSNE_MAX bytes. Returns 0, or -1 (state FAILED) when an RSNE is not a well-formed RSNE. */
int wpa_eapol_init(struct wpa_eapol *s, const uint8_t pmk[WPA_PMK_LEN], const uint8_t spa[6], const uint8_t aa[6],
                   const uint8_t *own_rsne, size_t own_rsne_len, const uint8_t *ap_rsne, size_t ap_rsne_len,
                   wpa_eapol_nonce_fn nonce, void *nonce_arg);

/* Take one received EAPOL frame (len bytes; bytes past the 802.1X body length are ignored). Writes the reply into
 * reply (cap bytes; WPA_EAPOL_REPLY_MAX is always enough) and its length into *reply_len, 0 when there is nothing
 * to send. Returns s->reason: WPA_EAPOL_OK when the frame was taken. */
enum wpa_eapol_reason wpa_eapol_rx(struct wpa_eapol *s, const uint8_t *frame, size_t len, uint8_t *reply,
                                   size_t cap, size_t *reply_len);

/* Wipe every key and nonce (disassociation). */
void wpa_eapol_clear(struct wpa_eapol *s);

#endif
