/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_wpa: joining a WPA2-PSK network as a station (stage 8, docs/WIFI_STAGE8.md): fm1wifi_frame.c's station for
 * the open-system authentication and the association (with our RSN element), then wpa_eapol.c's 4-way and group key
 * handshakes over EAPOL data frames, and the agreed keys into the MAC's key tables (fm1wifi.h fm1wifi_key_*) so CCMP
 * runs in hardware. AKM 2 (PSK), CCMP as pairwise and group cipher, no management frame protection.
 *
 * Two layers, as stages 4-6 have them:
 *  - fm1wifi_wpa_step: pure code, one frame and the time at a time; it says what to send and which keys to load
 *    (w->install), touches no register. The host tests run it against a made-up AP (tests/wifi_wpa.c).
 *  - fm1wifi_wpa_join / _listen / _leave: the driver on fm1wifi.c's FIFOs, clock and key tables.
 * What it saw is kept for the WiFi page and its report: every EAPOL frame either way (its first
 * FM1WIFI_WPA_REC_LEN bytes), the key table entries as read back (without the keys), the protected frames the MAC
 * decrypted or refused. The passphrase never reaches this code, only the PMK; the PMK and the keys are never kept in
 * what the page shows. Own code, from IEEE 802.11-2020 (9.4.2.24 RSNE, 12.7.6 and 12.7.7). Not run on an FM-1 yet.
 */
#pragma once
#include <stdint.h>
#include "fm1wifi.h"
#include "wpa_eapol.h"

enum { FM1WIFI_WPA_IDLE, FM1WIFI_WPA_JOIN, FM1WIFI_WPA_4WAY, FM1WIFI_WPA_UP, FM1WIFI_WPA_DOWN };
const char *fm1wifi_wpa_state_name(unsigned s);     /* "IDLE", "JOIN", "4-WAY", "UP", "DOWN" */
/* Why it went DOWN (or did not come UP) */
enum { FM1WIFI_WPA_WHY_NONE, FM1WIFI_WPA_WHY_RSN, FM1WIFI_WPA_WHY_JOIN, FM1WIFI_WPA_WHY_NO_MSG1,
       FM1WIFI_WPA_WHY_NO_MSG3, FM1WIFI_WPA_WHY_EAPOL, FM1WIFI_WPA_WHY_KEYS, FM1WIFI_WPA_WHY_LINK,
       FM1WIFI_WPA_WHY_LEFT };
const char *fm1wifi_wpa_why_name(unsigned why);     /* "NOT WPA2-PSK", "NO MSG 1", "KEY TABLES", ...; 14 at most */
/* What the AP's RSN element lacks (FM1WIFI_WPA_WHY_RSN) */
enum { FM1WIFI_RSN_OK, FM1WIFI_RSN_NONE, FM1WIFI_RSN_BAD, FM1WIFI_RSN_GROUP, FM1WIFI_RSN_PAIRWISE, FM1WIFI_RSN_AKM,
       FM1WIFI_RSN_MFP };
const char *fm1wifi_rsn_name(unsigned why);         /* "OK", "NO RSNE", "BAD RSNE", "NO CCMP GROUP", ... */
const char *fm1wifi_eapol_reason_name(unsigned r);  /* enum wpa_eapol_reason: "OK", "SHORT", ..., "MIC", "RSNE", ... */
/* Our RSN element for b's network (22 bytes: version 1, b's group cipher, CCMP, PSK, capabilities 0) into own:
 * FM1WIFI_RSN_OK, or what b's RSNE lacks: none kept, malformed, a group cipher other than CCMP, no CCMP pairwise
 * cipher, no PSK AKM, management frame protection required. */
#define FM1WIFI_WPA_RSNE_LEN 22u
int fm1wifi_wpa_rsn(const fm1wifi_bss *b, uint8_t own[FM1WIFI_WPA_RSNE_LEN]);

#define FM1WIFI_WPA_MSG1_MS 2000u    /* after the association: msg 1 by then (APs send it at once, again after 1 s) */
#define FM1WIFI_WPA_4WAY_MS 5000u    /* ... and the 4-way handshake done */
#define FM1WIFI_WPA_REC_N 8u
#define FM1WIFI_WPA_REC_LEN 192u     /* msg 3 with a 22-byte RSNE and a GTK KDE is 155 bytes */
#define FM1WIFI_WPA_OUT (32u + WPA_EAPOL_REPLY_MAX + 64u)   /* out's size: the longest frame the station sends */
enum { FM1WIFI_WPA_MSG_OTHER, FM1WIFI_WPA_MSG_1, FM1WIFI_WPA_MSG_2, FM1WIFI_WPA_MSG_3, FM1WIFI_WPA_MSG_4,
       FM1WIFI_WPA_MSG_G1, FM1WIFI_WPA_MSG_G2 };
const char *fm1wifi_wpa_msg_name(unsigned m);       /* "MSG 1" .. "MSG 4", "GROUP 1", "GROUP 2", "OTHER" */
/* One EAPOL frame as it went: the EAPOL PDU (802.1X header onward) */
typedef struct {
    uint8_t dir;              /* 0 from the AP, 1 to it */
    uint8_t msg;              /* FM1WIFI_WPA_MSG_*, from the key information bits */
    uint8_t reason;           /* from the AP: what wpa_eapol_rx said (enum wpa_eapol_reason, 0 taken) */
    uint8_t prot;             /* 1: protected (decrypted by the MAC / encrypted by it) */
    uint16_t t;               /* ms after the association (saturates) */
    uint16_t len;             /* the PDU's length; the first FM1WIFI_WPA_REC_LEN of it are in b */
    uint8_t b[FM1WIFI_WPA_REC_LEN];
} fm1wifi_wpa_rec;

typedef struct {
    fm1wifi_sta sta;          /* the 802.11 join (its why when the join or the link failed) */
    struct wpa_eapol e;       /* the handshakes (its reason for the last refusal) */
    uint8_t state, why;       /* FM1WIFI_WPA_*, FM1WIFI_WPA_WHY_* */
    uint8_t rsn;              /* FM1WIFI_RSN_* of the AP's element */
    uint8_t eapol_reason;     /* the handshake's failing reason (WHY_EAPOL), or the last frame refused */
    uint8_t got;              /* bit m: message m (FM1WIFI_WPA_MSG_*) seen, either way */
    uint8_t install;          /* WPA_EAPOL_NEW_*: keys the driver loads next (after sending the frame step returned) */
    uint8_t keys;             /* WPA_EAPOL_NEW_*: keys loaded in the MAC (the driver sets these) */
    uint8_t loaded;           /* WPA_EAPOL_NEW_*: keys loaded at some point since the start (kept after leaving) */
    uint8_t gtk_idx;          /* the key ID of the last GTK loaded */
    uint8_t rsne[FM1WIFI_WPA_RSNE_LEN];
    uint8_t pmk[WPA_PMK_LEN];
    wpa_eapol_nonce_fn nonce;
    void *nonce_arg;
    uint32_t t_assoc;         /* when the association came (ms) */
    uint32_t t_4way;          /* ms from the association to msg 4 sent (0: not reached) */
    uint32_t eapol_rx, eapol_refused;   /* EAPOL frames from the AP; of those, ones wpa_eapol_rx refused */
    uint32_t eapol_tx;        /* EAPOL frames sent to the AP */
    uint32_t prot_rx, prot_ok, prot_err;   /* protected data frames from the AP: all, decrypted by the MAC, refused by
                                            * it (a cipher error, or not decrypted) */
    uint32_t group_rx, group_ok;          /* of those, group addressed (the GTK's), and decrypted */
    uint32_t group_handshakes;            /* group key handshakes answered after the 4-way */
    int key_err;              /* the key tables' first error (WHY_KEYS) */
    fm1wifi_keyread kr_pair, kr_group;    /* the entries as read back after loading (the driver) */
    uint8_t n_rec;
    fm1wifi_wpa_rec rec[FM1WIFI_WPA_REC_N];
} fm1wifi_wpa;

/* Start joining b (from a scan: its RSN element, channel, rates) as sa at time now, with the PMK for its SSID and the
 * passphrase (wpa_crypto.h wpa_pmk_*) and a source of SNonces. When b's RSNE will not do, the station is DOWN with
 * WHY_RSN at once (w->rsn says why). The next step sends the authentication request. */
void fm1wifi_wpa_start(fm1wifi_wpa *w, const uint8_t sa[6], const fm1wifi_bss *b, const uint8_t pmk[WPA_PMK_LEN],
                       wpa_eapol_nonce_fn nonce, void *nonce_arg, uint32_t now);
/* One step at time now (ms) with a received frame (or 0), as fm1wifi_sta_step: the frame to send into out
 * (FM1WIFI_WPA_OUT bytes), *flags, its length; 0 for none. After sending it, load the keys in w->install. */
uint32_t fm1wifi_wpa_step(fm1wifi_wpa *w, const fm1wifi_rx *f, uint32_t now, uint8_t *out, unsigned *flags);
/* Leave: DOWN with WHY_LEFT, the handshake's keys wiped; the deauthentication to send into out, its length (0 when
 * not authenticated). The driver then clears the key tables. */
uint32_t fm1wifi_wpa_leave(fm1wifi_wpa *w, uint8_t *out, unsigned *flags);
/* 1 when the key (group: the last GTK, else the PTK) was loaded and its entries read back as fm1wifi_key_* wrote them
 * for this AP (the key words, the address, the attribute, the IV/EIV, the shared key mode bits) */
int fm1wifi_wpa_readback_ok(const fm1wifi_wpa *w, int group);
/* Wipe everything secret in w: the PMK, the handshake's keys and nonces (the records keep EAPOL frames only). */
void fm1wifi_wpa_wipe(fm1wifi_wpa *w);

/* ---- the driver (fm1wifi.c's FIFOs, clock and key tables; after fm1wifi_link_up) */

/* Tune to b's channel, set the BSSID, start (fm1wifi_wpa_start) and run until UP or DOWN, at most ms milliseconds. The
 * keys go into the tables as the handshake agrees them (the AP at FM1WIFI_WCID_AP, the GTK at FM1WIFI_WCID_GROUP);
 * when that fails the station leaves (WHY_KEYS). FM1WIFI_OK unless the FIFO failed. */
int fm1wifi_wpa_join(fm1wifi_wpa *w, const fm1wifi_bss *b, const uint8_t pmk[WPA_PMK_LEN], wpa_eapol_nonce_fn nonce,
                     void *nonce_arg, unsigned ms, fm1wifi_frames *fr);
/* Stay joined ms milliseconds: beacons counted, group key handshakes answered, protected frames counted. */
int fm1wifi_wpa_listen(fm1wifi_wpa *w, unsigned ms, fm1wifi_frames *fr);
/* Leave (the deauthentication) and clear the key tables. */
int fm1wifi_wpa_leave_now(fm1wifi_wpa *w, fm1wifi_frames *fr);
/* One pass of what _join and _listen loop on, for a caller that also wants the frames (stage 9's IP,
 * fm1wifi_ip.h): one frame from the RX FIFO into f (fm1wifi_poll), the station's step with it, the frame that step
 * said to send, the keys it agreed into the tables (leaving when that fails), the tables cleared when it went DOWN.
 * 1 when a frame was read (f->frame is 0 when it did not parse), 0 when none waited, else the FIFO's error (the
 * tables are then cleared). */
int fm1wifi_wpa_poll(fm1wifi_wpa *w, fm1wifi_rx *f, fm1wifi_frames *fr);
