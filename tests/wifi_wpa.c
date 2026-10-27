/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the WPA2-PSK station (stage 8, firmware/wifi/fm1wifi_wpa.c, docs/WIFI_STAGE8.md).
 *   1. The AP's RSN element: what the station joins (CCMP / CCMP / PSK, a WPA2/WPA3 transition network) and what it
 *      refuses (no RSNE, malformed, a TKIP group cipher, no CCMP, no PSK, management frame protection required); its
 *      own element byte for byte.
 *   2. The station's state machine alone (no register), fed frames laid out as the RX FIFO gives them, against the
 *      made-up AP (tests/wifi/wpa_ap.c, the authenticator moved out of tests/wifi_eapol.c, whose replay of
 *      Wireshark's wpa-Induction handshake checks wpa_eapol.c byte for byte): the association request carries the
 *      RSNE; msg 1-4 as EAPOL data frames (To DS, LLC/SNAP 888E), msg 4 in the clear before the keys are loaded; a
 *      group key handshake answered encrypted (the TXWI's WCID the AP's); the records; the timeouts (no msg 1, no msg
 *      3), a wrong passphrase, a bad MIC, a deauthentication, leaving; protected frames counted.
 *   3. The driver against the register model (tests/wifi/sdio_model.c), whose AP plays the key tables as the MAC
 *      would use them: the key table writes register by register (JieLi's layout), msg 4 sent before the PTK is
 *      written, the read-back, a group key handshake and protected broadcast frames that only decrypt with the right
 *      GTK, the tables cleared on leaving, a rejoin, a wrong passphrase and an AP that never sends msg 1.
 *   wifi_wpa <dir with core_model.txt>
 * Exits 1 when a check fails.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fm1wifi.h"
#include "fm1wifi_wpa.h"
#include "sdio_model.h"
#include "wpa_ap.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = 0;
    unsigned v;
    while (s[0] && s[1] && sscanf(s, "%2x", &v) == 1) { out[n++] = (uint8_t)v; s += 2; }
    return n;
}

static const uint8_t ME[6] = { 0x02, 0x46, 0x4D, 0x31, 0xAB, 0xCD };
static const uint8_t AP[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x06 };
static const char *RSNE = "30140100000fac040100000fac040100000fac020000";   /* CCMP / CCMP / PSK */

static int nonce_n;
static int nonce_cb(void *arg, uint8_t out[32])
{
    (void)arg;
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(0x40 + i + nonce_n);
    nonce_n++;
    return 0;
}

static void pmk_of(const char *pass, const char *ssid, uint8_t pmk[32])
{
    struct wpa_pmk_job j;
    wpa_pmk_begin(&j, pass, strlen(pass), (const uint8_t *)ssid, strlen(ssid));
    while (!wpa_pmk_step(&j, 4096)) {}
    wpa_pmk_take(&j, pmk);
}

static void bss_of(fm1wifi_bss *b, const char *rsne_hex)
{
    memset(b, 0, sizeof *b);
    memcpy(b->bssid, AP, 6);
    strcpy(b->ssid, "XNET");
    b->ssid_len = 4;
    b->channel = 6;
    b->sec = FM1WIFI_SEC_WPA2;
    b->interval = 1000;                    /* beacons lost after 10 s: the tests that send none run shorter */
    b->cap = 0x0411;
    b->rsn_len = rsne_hex ? (uint8_t)unhex(rsne_hex, b->rsn) : 0;
}

/* ---- 1. the RSN element */

static void test_rsn(void)
{
    static const struct { const char *rsne, *what; int want; } T[] = {
        { "30140100000fac040100000fac040100000fac020000", "CCMP / CCMP / PSK", FM1WIFI_RSN_OK },
        { "30180100000fac040100000fac040200000fac02000fac088000", "WPA2/WPA3 transition (PSK and SAE, MFP capable)", FM1WIFI_RSN_OK },
        { "30020100", "version only (the defaults: CCMP, CCMP, PSK)", FM1WIFI_RSN_OK },
        { 0, "none", FM1WIFI_RSN_NONE },
        { "30140200000fac040100000fac040100000fac020000", "version 2", FM1WIFI_RSN_BAD },
        { "30140100000fac040500000fac040100000fac020000", "a pairwise count past the end", FM1WIFI_RSN_BAD },
        { "30140100000fac020100000fac040100000fac020000", "group TKIP", FM1WIFI_RSN_GROUP },
        { "30140100000fac040100000fac020100000fac020000", "pairwise TKIP only", FM1WIFI_RSN_PAIRWISE },
        { "30140100000fac040100000fac040100000fac080000", "SAE only (WPA3)", FM1WIFI_RSN_AKM },
        { "30140100000fac040100000fac040100000fac02c000", "MFP required", FM1WIFI_RSN_MFP },
    };
    uint8_t own[FM1WIFI_WPA_RSNE_LEN], want[32];
    fm1wifi_bss b;
    char what[128];
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        bss_of(&b, T[i].rsne);
        snprintf(what, sizeof what, "RSN: %s -> %s", T[i].what, fm1wifi_rsn_name((unsigned)T[i].want));
        check(fm1wifi_wpa_rsn(&b, own) == T[i].want, what);
    }
    bss_of(&b, T[1].rsne);
    fm1wifi_wpa_rsn(&b, own);
    check(unhex(RSNE, want) == sizeof own && !memcmp(own, want, sizeof own), "RSN: ours is CCMP / CCMP / PSK, capabilities 0");
}

/* ---- 2. the state machine alone */

static uint8_t rxbuf[FM1WIFI_RX_BUF];

/* f (n bytes) as the RX FIFO gives it (length word, RXWI, the frame, trailer info), parsed */
static const fm1wifi_rx *rx_of(fm1wifi_rx *r, const uint8_t *f, unsigned n, uint32_t info)
{
    uint32_t N = 16u + ((n + 3u) & ~3u);
    memset(rxbuf, 0, sizeof rxbuf);
    rxbuf[0] = (uint8_t)N; rxbuf[1] = (uint8_t)(N >> 8);
    rxbuf[4] = 0xFF; rxbuf[6] = (uint8_t)n; rxbuf[7] = (uint8_t)(n >> 8);
    memcpy(rxbuf + 20, f, n);
    for (int k = 0; k < 4; k++) rxbuf[4 + N + k] = (uint8_t)(info >> (8 * k));
    if (fm1wifi_rx_parse(rxbuf, N + 8u, r) != FM1WIFI_OK) { fprintf(stderr, "FAIL: test frame does not parse\n"); exit(1); }
    return r;
}

static unsigned mgmt(uint8_t *f, uint8_t kind, const uint8_t *body, unsigned nb)
{
    memset(f, 0, 24);
    f[0] = kind;
    memcpy(f + 4, ME, 6); memcpy(f + 10, AP, 6); memcpy(f + 16, AP, 6);
    memcpy(f + 24, body, nb);
    return 24 + nb;
}

/* an EAPOL PDU from the AP as a data frame From DS (protected: decrypted by the MAC) */
static unsigned eapol(uint8_t *f, const uint8_t *pdu, size_t n, int prot)
{
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E };
    memset(f, 0, 24);
    f[0] = 0x08; f[1] = (uint8_t)(0x02 | (prot ? 0x40 : 0));
    memcpy(f + 4, ME, 6); memcpy(f + 10, AP, 6); memcpy(f + 16, AP, 6);
    memcpy(f + 24, snap, 8);
    memcpy(f + 32, pdu, n);
    return 32 + (unsigned)n;
}

static fm1wifi_wpa w;
static struct wpa_auth au;
static uint8_t out[FM1WIFI_WPA_OUT];
static unsigned flags;
static uint32_t now;

static uint32_t step(const uint8_t *f, unsigned n, uint32_t info)
{
    fm1wifi_rx r;
    uint32_t len = fm1wifi_wpa_step(&w, f ? rx_of(&r, f, n, info) : 0, now, out, &flags);
    if (w.install) { w.keys |= w.install; w.install = 0; }    /* what the driver does, with no registers */
    return len;
}

/* the station's EAPOL reply in out: its PDU (0 if out is not one) */
static const uint8_t *reply_pdu(uint32_t len, size_t *n)
{
    if (len < 32 || (out[0] & 0xFC) != 0x08 || (out[1] & 3) != 1 || memcmp(out + 4, AP, 6) || memcmp(out + 10, ME, 6) ||
        memcmp(out + 16, AP, 6) || out[24] != 0xAA || out[30] != 0x88 || out[31] != 0x8E) return 0;
    *n = len - 32;
    return out + 32;
}

/* start, authenticate, associate: the station at 4-WAY, the AP's authenticator ready (pmk_ap: the AP's PMK) */
static void to_4way(const uint8_t pmk_sta[32], const uint8_t pmk_ap[32])
{
    static const uint8_t auth_ok[6] = { 0, 0, 2, 0, 0, 0 }, assoc_ok[6] = { 0x01, 0x04, 0, 0, 1, 0xC0 };
    uint8_t f[256], rsne[32], gtk[16];
    fm1wifi_bss b;
    uint32_t len;
    size_t rn = unhex(RSNE, rsne);
    bss_of(&b, RSNE);
    memset(&w, 0, sizeof w);
    now = 1000;
    fm1wifi_wpa_start(&w, ME, &b, pmk_sta, nonce_cb, 0, now);
    len = step(0, 0, 0);
    check(w.state == FM1WIFI_WPA_JOIN && len == 30 && out[0] == FM1WIFI_FC_AUTH, "state: start sends the authentication");
    len = step(f, mgmt(f, FM1WIFI_FC_AUTH, auth_ok, 6), 0);
    check(len > 24 + 22 && out[0] == FM1WIFI_FC_ASSOC_REQ && !memcmp(out + len - 22, rsne, 22),
          "state: the association request ends with our RSNE");
    now += 5;
    len = step(f, mgmt(f, FM1WIFI_FC_ASSOC_RESP, assoc_ok, 6), 0);
    check(len == 0 && w.state == FM1WIFI_WPA_4WAY && w.sta.aid == 1, "state: associated, at the 4-way handshake");
    for (int i = 0; i < 16; i++) gtk[i] = (uint8_t)(0x70 + i);
    wpa_auth_init(&au, pmk_ap, AP, ME, rsne, rn, rsne, rn, gtk, 1, 9);
}

static void test_state(void)
{
    uint8_t pmk[32], pmk2[32], f[512], g[256], ptk[48];
    const uint8_t *p;
    size_t n, gn;
    uint32_t len;
    pmk_of("correct horse battery", "XNET", pmk);
    pmk_of("wrong horse battery", "XNET", pmk2);

    /* the whole handshake */
    to_4way(pmk, pmk);
    now += 3;
    gn = wpa_auth_msg1(&au, g);
    len = step(f, eapol(f, g, gn, 0), 0);
    p = reply_pdu(len, &n);
    check(p && !(out[1] & 0x40) && flags == FM1WIFI_TX_ACK, "state: msg 2 as a data frame To DS, LLC/SNAP 888E, in the clear");
    check(p && wpa_auth_take(&au, p, n) == 2, "state: the AP takes msg 2 (MIC, RSNE)");
    now += 4;
    gn = wpa_auth_msg3(&au, g);
    len = step(f, eapol(f, g, gn, 0), 0);
    p = reply_pdu(len, &n);
    check(p && !(out[1] & 0x40) && flags == FM1WIFI_TX_ACK, "state: msg 4 in the clear (sent before the PTK is loaded)");
    check(p && wpa_auth_take(&au, p, n) == 4, "state: the AP takes msg 4");
    check(w.state == FM1WIFI_WPA_UP && w.t_4way == 7 && w.keys == (WPA_EAPOL_NEW_PTK | WPA_EAPOL_NEW_GTK),
          "state: UP after msg 4, 7 ms after the association; both keys to load");
    check(!memcmp(w.e.tk, au.ptk + 32, 16) && w.e.gtk_len == 16 && w.e.gtk_idx == 1 && !memcmp(w.e.gtk, au.gtk, 16),
          "state: the TK and the GTK (key ID 1) are the AP's");
    check(w.n_rec == 4 && w.rec[0].msg == FM1WIFI_WPA_MSG_1 && !w.rec[0].dir && w.rec[1].msg == FM1WIFI_WPA_MSG_2 &&
          w.rec[1].dir && w.rec[2].msg == FM1WIFI_WPA_MSG_3 && w.rec[3].msg == FM1WIFI_WPA_MSG_4 && w.rec[2].len == gn &&
          !memcmp(w.rec[2].b, g, gn) && w.rec[3].t == 7, "state: the four EAPOL frames recorded as they went");
    check(w.got == (1u << 1 | 1u << 2 | 1u << 3 | 1u << 4), "state: msg 1-4 seen");

    /* beacons keep it up; a group key handshake, encrypted both ways */
    for (int i = 0; i < 30; i++) {
        static const uint8_t bcn[12] = { 0 };
        uint8_t b[64];
        unsigned bn = mgmt(b, FM1WIFI_FC_BEACON, bcn, 12);
        b[4] = 0xFF; memset(b + 4, 0xFF, 6);
        now += 100;
        step(b, bn, 0);
    }
    check(w.state == FM1WIFI_WPA_UP && w.sta.beacons == 30, "state: 30 beacons, still UP");
    for (int i = 0; i < 16; i++) au.gtk[i] = (uint8_t)(0x90 + i);
    au.gtk_idx = 2;
    gn = wpa_auth_group1(&au, g);
    len = step(f, eapol(f, g, gn, 1), FM1WIFI_RXD_DECRYPTED);
    p = reply_pdu(len, &n);
    check(p && (out[1] & 0x40) && flags == (FM1WIFI_TX_ACK | FM1WIFI_TX_CRYPT | FM1WIFI_TX_WCID(FM1WIFI_WCID_AP)),
          "state: group msg 2 protected, the MAC to encrypt it with the AP's key (WCID 1)");
    check(p && wpa_auth_take(&au, p, n) == 6, "state: the AP takes group msg 2");
    check(w.group_handshakes == 1 && w.e.gtk_idx == 2 && !memcmp(w.e.gtk, au.gtk, 16) && w.prot_rx == 1 && w.prot_ok == 1,
          "state: the new GTK (key ID 2); the protected frame counted as decrypted");
    gn = wpa_auth_group1(&au, g);                              /* not decrypted by the MAC: dropped, counted */
    len = step(f, eapol(f, g, gn, 1), 3u << 9);
    check(len == 0 && w.prot_err == 1 && w.eapol_rx == 3, "state: a protected frame with a key error is dropped and counted");
    {   uint8_t bc[64];
        unsigned bn = eapol(bc, (const uint8_t *)"xxxxxxxx", 8, 1);
        memset(bc + 4, 0xFF, 6);
        bc[30] = 0x08; bc[31] = 0x06;
        step(bc, bn, FM1WIFI_RXD_DECRYPTED);
        step(bc, bn, 0);
        check(w.group_rx == 2 && w.group_ok == 1, "state: group addressed protected frames: one decrypted, one not"); }

    /* a deauthentication ends it; leaving */
    {   uint8_t d[2] = { 7, 0 };
        step(f, mgmt(f, FM1WIFI_FC_DEAUTH, d, 2), 0);
        check(w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_LINK && w.sta.why == FM1WIFI_WHY_DEAUTH && w.sta.reason == 7,
              "state: a deauthentication: DOWN, LINK LOST (DEAUTH 7)"); }
    memset(ptk, 0, sizeof ptk);
    check(!memcmp(w.e.ptk, ptk, 48) && !memcmp(w.e.tk, ptk, 16), "state: the handshake's keys wiped when the link went");
    to_4way(pmk, pmk);
    len = fm1wifi_wpa_leave(&w, out, &flags);
    check(len == 26 && out[0] == FM1WIFI_FC_DEAUTH && w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_LEFT,
          "state: leaving sends the deauthentication");

    /* no msg 1 */
    to_4way(pmk, pmk);
    now += FM1WIFI_WPA_MSG1_MS - 1;
    check(step(0, 0, 0) == 0 && w.state == FM1WIFI_WPA_4WAY, "state: waits 2 s for msg 1");
    now += 1;
    len = step(0, 0, 0);
    check(w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_NO_MSG1 && len == 26 && out[0] == FM1WIFI_FC_DEAUTH,
          "state: no msg 1 after 2 s: DOWN, NO MSG 1, deauthenticated");

    /* a wrong passphrase: the AP refuses msg 2 and sends msg 1 again; no msg 3 by 5 s */
    to_4way(pmk2, pmk);
    for (int i = 0; i < 4; i++) {
        gn = wpa_auth_msg1(&au, g);
        len = step(f, eapol(f, g, gn, 0), 0);
        p = reply_pdu(len, &n);
        check(p && wpa_auth_take(&au, p, n) == 0, "state, wrong passphrase: the AP refuses msg 2 (MIC)");
        now += 1000;
    }
    now += 1000;
    step(0, 0, 0);
    check(w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_NO_MSG3 && w.got == (1u << 1 | 1u << 2) && w.n_rec == 8,
          "state, wrong passphrase: NO MSG 3 after 5 s, msg 1 and 2 four times each");

    /* msg 3 with a bad MIC: refused, the handshake stays */
    to_4way(pmk, pmk);
    gn = wpa_auth_msg1(&au, g);
    len = step(f, eapol(f, g, gn, 0), 0);
    p = reply_pdu(len, &n);
    wpa_auth_take(&au, p, n);
    gn = wpa_auth_msg3(&au, g);
    g[F_MIC] ^= 1;
    check(step(f, eapol(f, g, gn, 0), 0) == 0 && w.state == FM1WIFI_WPA_4WAY && w.eapol_refused == 1 &&
          w.eapol_reason == WPA_EAPOL_E_MIC && w.rec[2].reason == WPA_EAPOL_E_MIC, "state: msg 3 with a bad MIC refused (MIC)");

    /* an RSNE it will not join: DOWN at once, nothing sent */
    {   fm1wifi_bss b;
        bss_of(&b, "30140100000fac020100000fac040100000fac020000");
        memset(&w, 0, sizeof w);
        fm1wifi_wpa_start(&w, ME, &b, pmk, nonce_cb, 0, now);
        check(w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_RSN && w.rsn == FM1WIFI_RSN_GROUP && step(0, 0, 0) == 0,
              "state: a TKIP group cipher: DOWN, NOT WPA2-PSK (GROUP NOT CCMP), nothing sent"); }
}

/* ---- 3. the driver against the register model */

static const char *dir;
static fm1wifi_frames fr;
static sdio_model *core(const char *script)
{
    char path[512];
    fm1wifi_radio r;
    sdio_model *m = sdio_model_new();
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    if (sdio_model_load(m, path, stderr) || sdio_model_script(m, script, "test", stderr)) exit(1);
    sdio_model_raw_log(m, 0);
    sdio_model_use(m);
    if (fm1wifi_radio_up(&fm1wifi_cal_default, &r) != FM1WIFI_OK || fm1wifi_link_up(ME, 1) != FM1WIFI_OK) {
        fprintf(stderr, "FAIL: radio and link up\n");
        exit(1);
    }
    memset(&fr, 0, sizeof fr);
    return m;
}

/* the station's EAPOL frames sent so far (tx data frames in the model's frame log), msg 4 among them */
static int msg4_sent;
static void on_wr(void *ctx, uint32_t addr, uint32_t v)
{
    const wifi_fix *fx = sdio_model_frames(ctx);
    (void)v;
    if (addr != FM1_WIFI_CORE(PAIRWISE_KEY_ENTRY(FM1WIFI_WCID_AP)) || msg4_sent) return;
    for (int i = 0; i < fx->n; i++) {
        const wifi_rec *r = &fx->r[i];
        if (r->write && r->n > 40 && (r->data[0] & 0xFC) == 0x08 && r->data[30] == 0x88 && r->data[31] == 0x8E &&
            (r->data[32 + 5] << 8 | r->data[32 + 6]) == (KI_V2 | KI_P | KI_MIC | KI_SECURE))
            msg4_sent = 1;
    }
    if (!msg4_sent) msg4_sent = -1;
}

static uint32_t peek(sdio_model *m, uint32_t a) { return sdio_model_peek(m, FM1_WIFI_CORE(a)); }
static int key_in(sdio_model *m, uint32_t at, const uint8_t *k)
{
    for (unsigned i = 0; i < 8; i++)
        if (peek(m, at + 4 * i) != (i < 4 ? (uint32_t)k[4 * i] | k[4 * i + 1] << 8 | k[4 * i + 2] << 16 | (uint32_t)k[4 * i + 3] << 24 : 0u))
            return 0;
    return 1;
}

static void test_driver(void)
{
    static const char *XNET = "reg 0x3043c 0xe0\n"
        "ap ch=6 bssid=02:00:00:00:00:06 ssid=XNET sec=wpa2 psk=correct_horse rssi=-57 rekey=600 bcast=300\n";
    char script[512];
    uint8_t pmk[32], ptk[48], zero[32] = { 0 };
    fm1wifi_scan_list list;
    const fm1wifi_bss *b;
    int done, gdone;
    uint32_t refused;
    sdio_model *m;

    snprintf(script, sizeof script, "%s", XNET);
    pmk_of("correct_horse", "XNET", pmk);
    m = core(script);
    fm1wifi_scan_clear(&list);
    check(fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr) == FM1WIFI_OK && (b = fm1wifi_scan_find(&list, "XNET")) &&
          b->sec == FM1WIFI_SEC_WPA2 && b->rsn_len == 22, "driver: XNET heard, WPA2, its RSNE kept");
    sdio_model_on_wr(m, on_wr, m);
    check(fm1wifi_wpa_join(&w, b, pmk, nonce_cb, 0, 7000, &fr) == FM1WIFI_OK && w.state == FM1WIFI_WPA_UP, "driver: joined XNET");
    sdio_model_on_wr(m, 0, 0);
    check(msg4_sent == 1, "driver: msg 4 went out before the PTK was written");
    sdio_model_ap_wpa(m, 0, &done, &gdone, &refused, ptk);
    check(done && refused == 0, "driver: the AP finished the 4-way handshake, refused nothing");
    check(w.keys == (WPA_EAPOL_NEW_PTK | WPA_EAPOL_NEW_GTK) && w.key_err == FM1WIFI_OK, "driver: both keys loaded");
    check(peek(m, MAC_WCID_ENTRY(1)) == 0x00000002u && peek(m, MAC_WCID_ENTRY(1) + 4) == 0xFFFF0600u,
          "driver: the AP's address in MAC_WCID_ENTRY(1), the reserved bytes FF");
    check(key_in(m, PAIRWISE_KEY_ENTRY(1), ptk + 32), "driver: the TK in PAIRWISE_KEY_ENTRY(1), MIC keys 0");
    check(peek(m, MAC_WCID_ATTR_ENTRY(1)) == 0x9u && peek(m, MAC_IVEIV_ENTRY(1)) == 0x20000001u && peek(m, MAC_IVEIV_ENTRY(1) + 4) == 0,
          "driver: WCID 1: pairwise AES, BSS 0; IV PN 1, Ext IV, key ID 0");
    check(peek(m, SHARED_KEY_MODE_ENTRY(0)) == 0x40u && peek(m, MAC_WCID_ATTR_ENTRY(0)) == 0x8u &&
          peek(m, MAC_IVEIV_ENTRY(0)) == 0x60000001u, "driver: GTK key ID 1: AES in SHARED_KEY_MODE_ENTRY(0), WCID 0 shared AES, IV key ID 1");
    check(w.kr_pair.err == FM1WIFI_OK && w.kr_pair.key_same == 0xFF && w.kr_pair.wcid[0] == 2u && w.kr_pair.wcid[1] == 0xFFFF0600u &&
          w.kr_pair.attr == 9u && w.kr_pair.iv[0] == 0x20000001u && w.kr_pair.mode == 0,
          "driver: the pairwise entries as read back");
    check(w.kr_group.err == FM1WIFI_OK && w.kr_group.key_same == 0xFF && w.kr_group.mode == 0x40u && w.kr_group.attr == 8u &&
          w.kr_group.iv[0] == 0x60000001u, "driver: the group entries as read back");

    /* 2 s joined: protected broadcasts every 300 ms, a group key handshake at 600 ms */
    for (int i = 0; i < 4 && w.state == FM1WIFI_WPA_UP; i++) fm1wifi_wpa_listen(&w, 500, &fr);
    sdio_model_ap_wpa(m, 0, &done, &gdone, &refused, 0);
    check(w.state == FM1WIFI_WPA_UP && w.sta.beacons >= 18, "driver: 2 s UP, the beacons counted");
    check(w.group_handshakes == 1 && gdone == 1 && refused == 0, "driver: the group key handshake, group msg 2 taken (encrypted with the TK)");
    check(w.group_rx >= 6 && w.group_ok == w.group_rx && w.prot_err == 0, "driver: every protected broadcast decrypted (old and new GTK)");
    check(peek(m, SHARED_KEY_MODE_ENTRY(0)) == 0x440u && peek(m, MAC_IVEIV_ENTRY(0)) == 0xA0000001u,
          "driver: the new GTK as key ID 2: AES in its bits, WCID 0's IV key ID 2");

    /* leave: the tables cleared; rejoin */
    check(fm1wifi_wpa_leave_now(&w, &fr) == FM1WIFI_OK && w.state == FM1WIFI_WPA_DOWN && w.keys == 0, "driver: left");
    check(peek(m, MAC_WCID_ENTRY(1)) == 0xFFFFFFFFu && key_in(m, PAIRWISE_KEY_ENTRY(1), zero) && key_in(m, SHARED_KEY_ENTRY(1), zero) &&
          key_in(m, SHARED_KEY_ENTRY(2), zero) && peek(m, SHARED_KEY_MODE_ENTRY(0)) == 0 && peek(m, MAC_WCID_ATTR_ENTRY(1)) == 0 &&
          peek(m, MAC_WCID_ATTR_ENTRY(0)) == 0, "driver: leaving clears the key tables");
    check(fm1wifi_wpa_join(&w, b, pmk, nonce_cb, 0, 7000, &fr) == FM1WIFI_OK && w.state == FM1WIFI_WPA_UP, "driver: rejoined");
    fm1wifi_wpa_leave_now(&w, &fr);
    fm1wifi_power_down();
    check(sdio_model_bad(m) == 0, "driver: no access outside the WiFi blocks");
    sdio_model_free(m);

    /* a wrong passphrase: the AP sends msg 1 four times, then deauthenticates (4-way timeout, reason 15) */
    m = core(script);
    fm1wifi_scan_clear(&list);
    fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
    pmk_of("wrong_horse", "XNET", pmk);
    fm1wifi_wpa_join(&w, fm1wifi_scan_find(&list, "XNET"), pmk, nonce_cb, 0, 7000, &fr);
    check(w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_LINK && w.sta.why == FM1WIFI_WHY_DEAUTH && w.sta.reason == 15 &&
          w.keys == 0 && !(w.got & 1u << FM1WIFI_WPA_MSG_3) && (w.got & 1u << FM1WIFI_WPA_MSG_2),
          "driver, wrong passphrase: msg 2 sent, no msg 3, the AP's deauthentication 15");
    fm1wifi_power_down();
    sdio_model_free(m);

    /* an AP that never sends msg 1 */
    snprintf(script, sizeof script, "reg 0x3043c 0xe0\nap ch=6 bssid=02:00:00:00:00:06 ssid=XNET sec=wpa2 psk=correct_horse eapol=silent\n");
    m = core(script);
    fm1wifi_scan_clear(&list);
    fm1wifi_scan_channel(6, 120, 0, 0, &list, &fr);
    pmk_of("correct_horse", "XNET", pmk);
    fm1wifi_wpa_join(&w, fm1wifi_scan_find(&list, "XNET"), pmk, nonce_cb, 0, 7000, &fr);
    check(w.state == FM1WIFI_WPA_DOWN && w.why == FM1WIFI_WPA_WHY_NO_MSG1 && w.n_rec == 0, "driver, silent AP: NO MSG 1");
    fm1wifi_power_down();
    sdio_model_free(m);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fputs("usage: wifi_wpa <dir with core_model.txt>\n", stderr); return 2; }
    dir = argv[1];
    test_rsn();
    test_state();
    test_driver();
    if (bad) return 1;
    printf("wifi_wpa: %d checks passed (RSN element, the station against a made-up AP, the key tables against the model)\n", checks);
    return 0;
}
