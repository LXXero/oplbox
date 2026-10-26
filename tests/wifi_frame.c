/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of firmware/wifi/fm1wifi_frame.c (stages 4-6, docs/WIFI_STAGE4.md): the received-frame parser on
 * buffers laid out as the RX FIFO gives them, beacons and probe responses into the scan list, the TX FIFO image,
 * the management frames byte for byte, and the station's state machine with its timeouts. Pure code: no register
 * model. Exits 1 when a check fails.
 *
 * The frames are written here from IEEE 802.11-2020 9.3.3 (they are not recorded off an FM-1: the WiFi page dumps
 * the first received frames' bytes, and those become fixtures once xero's FM-1 has shown them).
 */
#include <stdio.h>
#include <string.h>
#include "fm1wifi.h"
#include "fm1wifi_frame.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static const uint8_t ME[6] = { 0x02, 0x46, 0x4D, 0x31, 0x00, 0x01 };
static const uint8_t AP[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x06 };
static const uint8_t AP2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0B };
static const uint8_t BC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* A buffer as fm1wifi_rx_read leaves it: length word N (CPU order), RXWI, the frame, pad to 4, trailer (CPU order). */
static uint32_t rxbuf(uint8_t *b, const uint8_t *f, uint32_t n, int8_t rssi, uint32_t info, uint32_t pad_after_hdr)
{
    uint32_t N = 16u + ((n + pad_after_hdr + 3u) & ~3u);
    memset(b, 0, N + 8u);
    le32(b, N);
    le32(b + 4, n << 16 | 0x01u);                       /* MPDU byte count, WCID 1 */
    le32(b + 8, 0x40000000u | 0x00030000u);             /* PHYMODE 1 (OFDM), MCS 3 */
    le32(b + 12, (uint8_t)rssi);
    if (pad_after_hdr) {                                 /* the header, the pad, the body (L2PAD) */
        uint32_t h = 26u;
        memcpy(b + 20, f, h);
        memcpy(b + 20 + h + pad_after_hdr, f + h, n - h);
    } else memcpy(b + 20, f, n);
    le32(b + 4 + N, info);
    return N + 8u;
}

static uint32_t hdr(uint8_t *f, uint8_t kind, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid)
{
    memset(f, 0, 24);
    f[0] = kind;
    memcpy(f + 4, da, 6); memcpy(f + 10, sa, 6); memcpy(f + 16, bssid, 6);
    return 24;
}

/* A beacon (or probe response): timestamp, interval 100, capability, then the elements given. */
static uint32_t beacon(uint8_t *f, uint8_t kind, const uint8_t *bssid, uint16_t cap, const uint8_t *el, uint32_t nel)
{
    uint32_t n = hdr(f, kind, kind == FM1WIFI_FC_BEACON ? BC : ME, bssid, bssid);
    memset(f + n, 0, 8); n += 8;
    f[n++] = 100; f[n++] = 0;
    f[n++] = (uint8_t)cap; f[n++] = (uint8_t)(cap >> 8);
    memcpy(f + n, el, nel);
    return n + nel;
}

static const uint8_t EL_OPEN[] = { 0, 6, 'O', 'P', 'L', 'B', 'O', 'X', 1, 4, 0x82, 0x84, 0x8B, 0x96, 3, 1, 6,
                                   50, 2, 0x30, 0x6C };
static const uint8_t EL_WPA2[] = { 0, 4, 'H', 'O', 'M', 'E', 1, 1, 0x82, 3, 1, 11,
                                   48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0 };
static const uint8_t EL_WPA[] = { 0, 3, 'O', 'L', 'D', 3, 1, 1,
                                  221, 8, 0x00, 0x50, 0xF2, 1, 1, 0, 0x00, 0x50 };
static const uint8_t EL_HIDDEN[] = { 0, 5, 0, 0, 0, 0, 0, 3, 1, 1 };
static const uint8_t EL_OVERRUN[] = { 0, 2, 'A', 'B', 3, 1, 9, 48, 40, 1, 0 };   /* RSN says 40 bytes, has 2 */

static void parser(void)
{
    uint8_t f[256], b[512];
    fm1wifi_rx r;
    fm1wifi_bss s;
    uint32_t n = beacon(f, FM1WIFI_FC_BEACON, AP, 0x0401, EL_OPEN, sizeof EL_OPEN), m;

    m = rxbuf(b, f, n, -42, FM1WIFI_RXD_BROADCAST, 0);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_OK, "a beacon parses");
    check(r.len == n && r.hdr_len == 24u && r.body_len == n - 24u && r.frame == b + 20 && r.body == b + 44,
          "beacon: MPDU length from RXWI, a 24-byte header, the body after it");
    check(r.rssi == -42 && r.mcs == 3 && r.phymode == 1 && r.wcid == 1 && r.info == FM1WIFI_RXD_BROADCAST,
          "beacon: RSSI0 as signed dBm, MCS, PHY mode, WCID, the trailer");
    check(FM1WIFI_FC_KIND(r.fc) == FM1WIFI_FC_BEACON, "beacon: its kind");
    check(fm1wifi_bss_parse(&r, 4, &s) == FM1WIFI_OK, "the beacon describes a network");
    check(!strcmp(s.ssid, "OPLBOX") && s.ssid_len == 6 && !memcmp(s.bssid, AP, 6) && s.channel == 6 && s.rssi == -42,
          "beacon: SSID, BSSID, the DS element's channel (not the one it was heard on), RSSI");
    check(s.sec == FM1WIFI_SEC_OPEN && s.interval == 100 && s.cap == 0x0401 && s.n_rates == 6 && s.rates[0] == 0x82 &&
          s.rates[5] == 0x6C && !s.probed && s.heard == 1, "beacon: open, interval, capability, both rate elements");

    n = beacon(f, FM1WIFI_FC_PROBE_RESP, AP2, 0x0011, EL_WPA2, sizeof EL_WPA2);
    m = rxbuf(b, f, n, -70, FM1WIFI_RXD_UNICAST_TO_ME, 0);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_OK && fm1wifi_bss_parse(&r, 11, &s) == FM1WIFI_OK, "a WPA2 probe response");
    check(s.sec == FM1WIFI_SEC_WPA2 && s.probed && s.rsn_len == 22 && s.rsn[0] == 48 && s.rsn[1] == 20 && s.channel == 11,
          "WPA2: the RSN element kept whole, probed");

    n = beacon(f, FM1WIFI_FC_BEACON, AP2, 0x0011, EL_WPA, sizeof EL_WPA);
    m = rxbuf(b, f, n, -60, 0, 0);
    fm1wifi_rx_parse(b, m, &r);
    check(fm1wifi_bss_parse(&r, 1, &s) == FM1WIFI_OK && s.sec == FM1WIFI_SEC_WPA, "WPA: the vendor element");
    n = beacon(f, FM1WIFI_FC_BEACON, AP2, 0x0011, EL_HIDDEN, 2);   /* no DS element: the channel heard on */
    m = rxbuf(b, f, n, -60, 0, 0);
    fm1wifi_rx_parse(b, m, &r);
    check(fm1wifi_bss_parse(&r, 5, &s) == FM1WIFI_OK && s.sec == FM1WIFI_SEC_WEP && s.channel == 5,
          "privacy without RSN or WPA: WEP; no DS element: the channel it was heard on");
    n = beacon(f, FM1WIFI_FC_BEACON, AP2, 0x0001, EL_HIDDEN, sizeof EL_HIDDEN);
    m = rxbuf(b, f, n, -60, 0, 0);
    fm1wifi_rx_parse(b, m, &r);
    check(fm1wifi_bss_parse(&r, 1, &s) == FM1WIFI_OK && s.ssid_len == 0 && s.ssid[0] == 0, "an SSID of zero bytes: hidden");
    n = beacon(f, FM1WIFI_FC_BEACON, AP2, 0x0001, EL_OVERRUN, sizeof EL_OVERRUN);
    m = rxbuf(b, f, n, -60, 0, 0);
    fm1wifi_rx_parse(b, m, &r);
    check(fm1wifi_bss_parse(&r, 1, &s) == FM1WIFI_OK && !strcmp(s.ssid, "AB") && s.channel == 9 && s.sec == FM1WIFI_SEC_OPEN,
          "an element running past the frame ends the elements, the earlier ones stay");

    /* a QoS data frame with L2PAD: the body starts after the 26-byte header and 2 pad bytes */
    memset(f, 0, 64);
    f[0] = FM1WIFI_FC_QOS_DATA; f[1] = 0x02;                       /* FromDS */
    memcpy(f + 4, ME, 6); memcpy(f + 10, AP, 6);
    for (int i = 26; i < 40; i++) f[i] = (uint8_t)i;
    m = rxbuf(b, f, 40, -30, FM1WIFI_RXD_L2PAD | FM1WIFI_RXD_DATA, 2);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_OK && r.hdr_len == 26u && r.body_len == 14u && r.body == b + 20 + 28 &&
          r.body[0] == 26 && r.body[13] == 39, "QoS data with L2PAD: header 26, then 2 pad bytes, then the body");
    check(fm1wifi_bss_parse(&r, 1, &s) == FM1WIFI_EFRAME, "a data frame is not a network's description");

    /* refusals */
    n = beacon(f, FM1WIFI_FC_BEACON, AP, 0x0401, EL_OPEN, sizeof EL_OPEN);
    m = rxbuf(b, f, n, -42, 0, 0);
    check(fm1wifi_rx_parse(b, m - 1, &r) == FM1WIFI_EFRAME, "refused: fewer bytes than the length word says");
    le32(b, 0);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_EFRAME, "refused: a length word of 0");
    le32(b, 4092);
    check(fm1wifi_rx_parse(b, 5000, &r) == FM1WIFI_EFRAME, "refused: a length word past 4088");
    m = rxbuf(b, f, n, -42, 0, 0);
    le32(b + 4, (n + 40u) << 16);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_EFRAME, "refused: an MPDU longer than the length word holds");
    le32(b + 4, 8u << 16);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_EFRAME, "refused: an MPDU shorter than an ACK");
    m = rxbuf(b, f, 20, -42, 0, 0);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_EFRAME, "refused: a management frame shorter than its header");
    m = rxbuf(b, f, 30, -42, 0, 0);
    check(fm1wifi_rx_parse(b, m, &r) == FM1WIFI_OK && fm1wifi_bss_parse(&r, 1, &s) == FM1WIFI_EFRAME,
          "a beacon without its fixed fields describes nothing");
}

static void scan_list(void)
{
    static fm1wifi_scan_list l;
    fm1wifi_bss a, b;
    memset(&a, 0, sizeof a);
    memcpy(a.bssid, AP, 6); a.rssi = -60; a.heard = 1; a.channel = 1;
    fm1wifi_scan_clear(&l);
    check(fm1wifi_scan_add(&l, &a) == &l.e[0] && l.n == 1, "the first network");
    b = a; b.rssi = -40; strcpy(b.ssid, "SECRET"); b.ssid_len = 6; b.probed = 1;
    check(fm1wifi_scan_add(&l, &b) == &l.e[0] && l.n == 1, "the same BSSID again: merged");
    check(l.e[0].rssi == -40 && !strcmp(l.e[0].ssid, "SECRET") && l.e[0].probed && l.e[0].heard == 2,
          "merged: strongest RSSI, a hidden SSID filled in, probed, heard twice");
    b.rssi = -90; b.ssid[0] = 0; b.ssid_len = 0; b.probed = 0;
    fm1wifi_scan_add(&l, &b);
    check(l.e[0].rssi == -40 && !strcmp(l.e[0].ssid, "SECRET") && l.e[0].probed, "a weaker hidden beacon takes nothing away");
    for (int i = 1; i < 20; i++) { b.bssid[5] = (uint8_t)(0x40 + i); b.rssi = (int8_t)(-50 - i); snprintf(b.ssid, sizeof b.ssid, "N%d", i); b.ssid_len = 2; fm1wifi_scan_add(&l, &b); }
    check(l.n == FM1WIFI_SCAN_MAX && l.full == 20 - FM1WIFI_SCAN_MAX, "the list holds 16, the rest counted");
    b.bssid[5] = 0x50; strcpy(b.ssid, "N1"); b.rssi = -20;
    l.n--;                                                         /* room for one more */
    fm1wifi_scan_add(&l, &b);
    check(fm1wifi_scan_find(&l, "N1") && fm1wifi_scan_find(&l, "N1")->rssi == -20, "find: the strongest of two with one SSID");
    check(fm1wifi_scan_find(&l, "SECRET") == &l.e[0] && !fm1wifi_scan_find(&l, "SECRE") && !fm1wifi_scan_find(&l, "NOPE"),
          "find: whole SSIDs only");
}

static void tx(void)
{
    static uint32_t img32[FM1WIFI_TX_BUF / 4u + 1u];
    uint8_t *img = (uint8_t *)img32, f[256];
    uint32_t n = fm1wifi_auth_open(f, ME, AP, 7), d;
    static const uint8_t AUTH[30] = { 0xB0, 0, 0x3A, 0x01, 2, 0, 0, 0, 0, 6, 2, 0x46, 0x4D, 0x31, 0, 1, 2, 0, 0, 0, 0, 6,
                                      0x70, 0, 0, 0, 1, 0, 0, 0 };
    check(n == 30 && !memcmp(f, AUTH, 30), "authentication request: open system, sequence 1, duration 314, seq 7");
    memset(img, 0xEE, FM1WIFI_TX_BUF);
    d = fm1wifi_tx_image(img, f, n, FM1WIFI_TX_ACK);
    check(d == n + 26u, "TX image: the DMA sends frame + 26 bytes (JieLi's count)");
    check(rd32(img) == n + 22u && rd32(img + 4) == 0x5u, "TX image: JieLi's length word, its header (WIV, QSEL 2)");
    check(rd32(img + 8) == 0x00000300u, "TXWI word 0: TX_OP backoff, MCS 0, CCK");
    check(rd32(img + 12) == (FM1WIFI_TXWI_W1_ACK | 0xFF00u | n << 16), "TXWI word 1: ACK, WCID 255, the MPDU length");
    check(rd32(img + 16) == 0 && rd32(img + 20) == 0 && !memcmp(img + FM1WIFI_TX_HDR, f, n), "TXWI IV/EIV 0, then the frame");
    { int z = 1; for (uint32_t i = FM1WIFI_TX_HDR + n; i < 4u + d; i++) z &= img[i] == 0; check(z && img[4 + d] == 0xEE, "TX image: zeros to the DMA's end, nothing past it"); }
    d = fm1wifi_tx_image(img, f, n, FM1WIFI_TX_OFDM6);
    check((rd32(img + 8) & FM1WIFI_TXWI_W0_PHYMODE) == 0x40000000u && !(rd32(img + 12) & FM1WIFI_TXWI_W1_ACK),
          "OFDM 6: PHY mode 1; no ACK asked");
    check(!fm1wifi_tx_image(img, f, 0, 0) && !fm1wifi_tx_image(img, f, FM1WIFI_TX_FRAME_MAX + 1u, 0) &&
          fm1wifi_tx_image(img, f, FM1WIFI_TX_FRAME_MAX, 0) == FM1WIFI_TX_FRAME_MAX + 26u, "TX image: 1-1600 bytes");

    n = fm1wifi_probe_req(f, ME, 0, 0x123);
    {
        static const uint8_t PROBE[24 + 2 + 10 + 6] = { 0x40, 0, 0, 0, 255, 255, 255, 255, 255, 255, 2, 0x46, 0x4D, 0x31, 0, 1,
            255, 255, 255, 255, 255, 255, 0x30, 0x12, 0, 0, 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30,
            0x48, 0x60, 0x6C };
        check(n == sizeof PROBE && !memcmp(f, PROBE, n), "probe request for any network: broadcast, SSID element empty, rates");
    }
    n = fm1wifi_probe_req(f, ME, "SECRET", 1);
    check(n == 48 && f[24] == 0 && f[25] == 6 && !memcmp(f + 26, "SECRET", 6), "probe request for one SSID");
    {
        fm1wifi_bss b;
        static const uint8_t RSN[4] = { 48, 2, 1, 0 };
        memset(&b, 0, sizeof b);
        memcpy(b.bssid, AP, 6); strcpy(b.ssid, "OPLBOX"); b.ssid_len = 6; b.cap = 0x0431;
        n = fm1wifi_assoc_req(f, ME, &b, RSN, sizeof RSN, 2);
        check(n == 24 + 4 + 8 + 10 + 6 + 4 && f[0] == 0x00 && !memcmp(f + 4, AP, 6) && !memcmp(f + 10, ME, 6),
              "association request: to the AP, from us");
        check(f[24] == 0x31 && f[25] == 0x04 && f[26] == 10 && f[27] == 0, "association request: ESS with the AP's privacy, "
              "short preamble and short slot; listen interval 10");
        check(f[28] == 0 && f[29] == 6 && !memcmp(f + 30, "OPLBOX", 6) && f[36] == 1 && f[46] == 50 && !memcmp(f + n - 4, RSN, 4),
              "association request: SSID, both rate elements, the extra elements last");
    }
    n = fm1wifi_deauth(f, ME, AP, 3, 9);
    check(n == 26 && f[0] == 0xC0 && f[24] == 3 && f[25] == 0 && f[22] == 0x90, "deauthentication: reason 3, seq 9");
}

/* ---- the station */

static uint8_t rxb[512];
static fm1wifi_rx rx;

static const fm1wifi_rx *mk(uint8_t kind, const uint8_t *from, const uint8_t *to, const uint8_t *body, uint32_t nb)
{
    uint8_t f[128];
    uint32_t n = hdr(f, kind, to, from, from), m;
    memcpy(f + n, body, nb);
    m = rxbuf(rxb, f, n + nb, -40, 0, 0);
    if (fm1wifi_rx_parse(rxb, m, &rx) != FM1WIFI_OK) { check(0, "station test frame parses"); return 0; }
    return &rx;
}
static const fm1wifi_rx *auth_resp(uint16_t status) { uint8_t b[6] = { 0, 0, 2, 0, (uint8_t)status, 0 }; return mk(FM1WIFI_FC_AUTH, AP, ME, b, 6); }
static const fm1wifi_rx *assoc_resp(uint16_t status) { uint8_t b[6] = { 1, 0, (uint8_t)status, 0, 5, 0xC0 }; return mk(FM1WIFI_FC_ASSOC_RESP, AP, ME, b, 6); }
static const fm1wifi_rx *bcn(void) { uint8_t b[12] = { 0 }; b[8] = 100; return mk(FM1WIFI_FC_BEACON, AP, BC, b, 12); }

static void station(void)
{
    static fm1wifi_sta s;
    fm1wifi_bss b;
    uint8_t out[256];
    unsigned fl;
    uint32_t n;
    uint8_t one[2] = { 7, 0 };
    memset(&b, 0, sizeof b);
    memcpy(b.bssid, AP, 6); strcpy(b.ssid, "OPLBOX"); b.ssid_len = 6; b.interval = 100; b.channel = 6;

    memset(&s, 0, sizeof s);
    fm1wifi_sta_start(&s, ME, &b, 1000);
    n = fm1wifi_sta_step(&s, 0, 1000, out, &fl);
    check(s.state == FM1WIFI_STA_AUTH && n == 30 && out[0] == FM1WIFI_FC_AUTH && fl == FM1WIFI_TX_ACK, "start: the authentication request");
    check(!fm1wifi_sta_step(&s, 0, 1100, out, &fl), "nothing again before 300 ms");
    check(!fm1wifi_sta_step(&s, mk(FM1WIFI_FC_AUTH, AP2, ME, (const uint8_t[]){ 0, 0, 2, 0, 0, 0 }, 6), 1150, out, &fl) &&
          s.state == FM1WIFI_STA_AUTH, "an answer from another BSS is ignored");
    n = fm1wifi_sta_step(&s, auth_resp(0), 1200, out, &fl);
    check(s.state == FM1WIFI_STA_ASSOC && n > 30 && out[0] == FM1WIFI_FC_ASSOC_REQ, "authenticated: the association request at once");
    n = fm1wifi_sta_step(&s, assoc_resp(0), 1250, out, &fl);
    check(s.state == FM1WIFI_STA_UP && s.aid == 5 && !n && s.status == 0, "associated: UP, AID 5 (the top two bits dropped)");
    check(((out[22] | out[23] << 8) >> 4) == 1, "the association request carried sequence number 1");
    fm1wifi_sta_step(&s, bcn(), 1300, out, &fl);
    fm1wifi_sta_step(&s, bcn(), 1400, out, &fl);
    check(s.beacons == 2 && s.state == FM1WIFI_STA_UP, "beacons counted while UP");
    fm1wifi_sta_step(&s, 0, 2423, out, &fl);
    check(s.state == FM1WIFI_STA_UP, "UP 1023 ms after the last beacon (10 intervals of 102.4 ms)");
    fm1wifi_sta_step(&s, 0, 2424, out, &fl);
    check(s.state == FM1WIFI_STA_DOWN && s.why == FM1WIFI_WHY_BEACON_LOSS, "DOWN 1024 ms after: beacon loss");
    check(!strcmp(fm1wifi_why_name(s.why), "BEACON LOSS") && !strcmp(fm1wifi_sta_state_name(s.state), "DOWN"), "names");

    /* rejoin, then a deauthentication */
    fm1wifi_sta_start(&s, ME, &b, 3000);
    check(s.seq == 2, "a rejoin keeps counting sequence numbers");
    fm1wifi_sta_step(&s, 0, 3000, out, &fl);
    fm1wifi_sta_step(&s, auth_resp(0), 3010, out, &fl);
    fm1wifi_sta_step(&s, assoc_resp(0), 3020, out, &fl);
    check(s.state == FM1WIFI_STA_UP, "rejoined");
    fm1wifi_sta_step(&s, mk(FM1WIFI_FC_DEAUTH, AP, AP2, one, 2), 3030, out, &fl);
    check(s.state == FM1WIFI_STA_UP, "a deauthentication for someone else is ignored");
    fm1wifi_sta_step(&s, mk(FM1WIFI_FC_DEAUTH, AP, ME, one, 2), 3040, out, &fl);
    check(s.state == FM1WIFI_STA_DOWN && s.why == FM1WIFI_WHY_DEAUTH && s.reason == 7, "deauthenticated: DOWN, reason 7");
    fm1wifi_sta_start(&s, ME, &b, 4000);
    fm1wifi_sta_step(&s, 0, 4000, out, &fl);
    fm1wifi_sta_step(&s, auth_resp(0), 4010, out, &fl);
    fm1wifi_sta_step(&s, assoc_resp(0), 4020, out, &fl);
    fm1wifi_sta_step(&s, mk(FM1WIFI_FC_DISASSOC, AP, ME, one, 2), 4030, out, &fl);
    check(s.state == FM1WIFI_STA_DOWN && s.why == FM1WIFI_WHY_DISASSOC, "disassociated: DOWN");

    /* timeouts and refusals */
    fm1wifi_sta_start(&s, ME, &b, 0);
    for (uint32_t t = 0, k = 0; t <= 1000; t += 10) k += fm1wifi_sta_step(&s, 0, t, out, &fl) != 0, n = k;
    check(n == FM1WIFI_STA_TRIES && s.state == FM1WIFI_STA_DOWN && s.why == FM1WIFI_WHY_AUTH_TIMEOUT,
          "no answer: 3 authentication requests 300 ms apart, then AUTH TIMEOUT");
    fm1wifi_sta_start(&s, ME, &b, 0);
    fm1wifi_sta_step(&s, 0, 0, out, &fl);
    fm1wifi_sta_step(&s, auth_resp(13), 5, out, &fl);
    check(s.state == FM1WIFI_STA_DOWN && s.why == FM1WIFI_WHY_AUTH_REFUSED && s.status == 13, "authentication refused, status 13");
    fm1wifi_sta_start(&s, ME, &b, 0);
    fm1wifi_sta_step(&s, 0, 0, out, &fl);
    fm1wifi_sta_step(&s, auth_resp(0), 5, out, &fl);
    for (uint32_t t = 10, k = 1; t <= 1000; t += 10) k += fm1wifi_sta_step(&s, 0, t, out, &fl) != 0, n = k;
    check(n == FM1WIFI_STA_TRIES && s.why == FM1WIFI_WHY_ASSOC_TIMEOUT, "no association response: 3 requests, ASSOC TIMEOUT");
    fm1wifi_sta_start(&s, ME, &b, 0);
    fm1wifi_sta_step(&s, 0, 0, out, &fl);
    fm1wifi_sta_step(&s, auth_resp(0), 5, out, &fl);
    fm1wifi_sta_step(&s, assoc_resp(17), 10, out, &fl);
    check(s.why == FM1WIFI_WHY_ASSOC_REFUSED && s.status == 17, "association refused, status 17");

    /* leaving */
    fm1wifi_sta_start(&s, ME, &b, 0);
    fm1wifi_sta_step(&s, 0, 0, out, &fl);
    check(!fm1wifi_sta_leave(&s, out, &fl) && s.why == FM1WIFI_WHY_LEFT, "leaving before authentication: nothing to send");
    fm1wifi_sta_start(&s, ME, &b, 0);
    fm1wifi_sta_step(&s, 0, 0, out, &fl);
    fm1wifi_sta_step(&s, auth_resp(0), 5, out, &fl);
    fm1wifi_sta_step(&s, assoc_resp(0), 10, out, &fl);
    n = fm1wifi_sta_leave(&s, out, &fl);
    check(n == 26 && out[0] == FM1WIFI_FC_DEAUTH && out[24] == 3 && fl == FM1WIFI_TX_ACK && s.state == FM1WIFI_STA_DOWN &&
          s.why == FM1WIFI_WHY_LEFT, "leaving when UP: a deauthentication, reason 3");
}

int main(void)
{
    parser();
    scan_list();
    tx();
    station();
    if (bad) return 1;
    printf("wifi_frame: %d checks passed (RX parser, scan list, TX image, management frames, station)\n", checks);
    return 0;
}
