/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_frame: received frames, the scan list, the TX FIFO image, a station's management frames and its state
 * machine (stages 4-6). See fm1wifi_frame.h. Own code: the RX and TX descriptor fields from Linux rt2800.h
 * (GPL-2.0-or-later, its names), the frame formats and the station's behaviour from IEEE 802.11-2020 (9.3.3,
 * 9.4.2, 11.3), the FIFO's byte layout from docs/WIFI_STAGE4.md. Nothing here touches a register.
 */
#include "fm1wifi.h"
#include "fm1wifi_frame.h"

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void copy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }

/* ---- received frames */

/* rt2800.h's RXWI fields */
#define RXWI_W0_WCID 0x000000FFu
#define RXWI_W0_MPDU_TOTAL_BYTE_COUNT 0x0FFF0000u
#define RXWI_W1_MCS 0x007F0000u
#define RXWI_W1_BW 0x00800000u
#define RXWI_W1_PHYMODE 0xC0000000u
#define RXWI_W2_RSSI0 0x000000FFu

/* The 802.11 header's length (IEEE 802.11-2020 9.2.3): 24 for management frames; data frames 24, 6 more with both
 * DS bits (four addresses), 2 more for QoS, 4 more for an HT Control field (+HTC: QoS and the Order bit); a control
 * frame is header only. */
static uint32_t header_len(uint32_t fc, uint32_t len)
{
    uint32_t type = fc >> 2 & 3u, n;
    if (type == 0u) return 24u;
    if (type == 1u) return len;
    if (type != 2u) return 0;
    n = 24u;
    if ((fc & 0x0300u) == 0x0300u) n += 6u;
    if (fc & 0x80u) { n += 2u; if (fc & 0x8000u) n += 4u; }
    return n;
}

int fm1wifi_rx_parse(const uint8_t *buf, uint32_t n, fm1wifi_rx *f)
{
    uint32_t N, i, pad;
    uint8_t *z = (uint8_t *)f;
    for (i = 0; i < sizeof *f; i++) z[i] = 0;
    if (n < 8u + FM1WIFI_RXWI_LEN) return FM1WIFI_EFRAME;
    N = le32(buf);
    if (N < FM1WIFI_RXWI_LEN || N > FM1WIFI_RX_LEN_MAX || N + 8u > n) return FM1WIFI_EFRAME;
    for (i = 0; i < 4u; i++) f->rxwi[i] = le32(buf + 4u + 4u * i);
    f->info = le32(buf + 4u + N);
    f->wcid = (uint8_t)(f->rxwi[0] & RXWI_W0_WCID);
    f->len = (uint16_t)((f->rxwi[0] & RXWI_W0_MPDU_TOTAL_BYTE_COUNT) >> 16);
    f->mcs = (uint8_t)((f->rxwi[1] & RXWI_W1_MCS) >> 16);
    f->bw = (f->rxwi[1] & RXWI_W1_BW) != 0;
    f->phymode = (uint8_t)(f->rxwi[1] >> 30);
    f->rssi = (int8_t)(uint8_t)(f->rxwi[2] & RXWI_W2_RSSI0);
    if (f->len < 10u || FM1WIFI_RXWI_LEN + f->len > N) return FM1WIFI_EFRAME;   /* 10: an ACK, the shortest frame */
    f->frame = buf + 4u + FM1WIFI_RXWI_LEN;
    f->fc = (uint16_t)le16(f->frame);
    f->hdr_len = (uint16_t)header_len(f->fc, f->len);
    if (!f->hdr_len || f->hdr_len > f->len) return FM1WIFI_EFRAME;
    pad = (f->info & FM1WIFI_RXD_L2PAD) && (f->hdr_len & 3u) ? 4u - (f->hdr_len & 3u) : 0;
    if (FM1WIFI_RXWI_LEN + f->len + pad > N) return FM1WIFI_EFRAME;
    f->body = f->frame + f->hdr_len + pad;
    f->body_len = (uint16_t)(f->len - f->hdr_len);
    return FM1WIFI_OK;
}

/* ---- beacons and probe responses */

const char *fm1wifi_sec_name(unsigned sec)
{
    static const char *const N[] = { "OPEN", "WEP", "WPA", "WPA2" };
    return sec < 4u ? N[sec] : "?";
}

int fm1wifi_bss_parse(const fm1wifi_rx *f, unsigned ch, fm1wifi_bss *b)
{
    static const uint8_t WPA_OUI[4] = { 0x00, 0x50, 0xF2, 0x01 };
    uint32_t kind = FM1WIFI_FC_KIND(f->fc), i, k, wpa = 0, rsn = 0;
    const uint8_t *e;
    uint8_t *z = (uint8_t *)b;
    for (i = 0; i < sizeof *b; i++) z[i] = 0;
    if (!f->frame || (kind != FM1WIFI_FC_BEACON && kind != FM1WIFI_FC_PROBE_RESP) || f->body_len < 12u)
        return FM1WIFI_EFRAME;
    copy(b->bssid, f->frame + 16, 6);
    b->rssi = f->rssi;
    b->channel = (uint8_t)ch;
    b->interval = (uint16_t)le16(f->body + 8);
    b->cap = (uint16_t)le16(f->body + 10);
    b->heard = 1;
    b->probed = kind == FM1WIFI_FC_PROBE_RESP;
    e = f->body + 12;
    for (i = 12; i + 2u <= f->body_len; i += 2u + e[1], e += 2 + e[1]) {
        uint32_t id = e[0], n = e[1];
        const uint8_t *v = e + 2;
        if (i + 2u + n > f->body_len) break;                    /* runs past the frame: the rest is not elements */
        if (id == 0u && n <= 32u) {                             /* SSID; all zero bytes is a hidden network too */
            uint32_t any = 0;
            for (k = 0; k < n; k++) any |= v[k];
            if (any) { copy((uint8_t *)b->ssid, v, n); b->ssid[n] = 0; b->ssid_len = (uint8_t)n; }
        } else if (id == 1u || id == 50u) {                     /* Supported Rates, Extended Supported Rates */
            for (k = 0; k < n && b->n_rates < sizeof b->rates; k++) b->rates[b->n_rates++] = v[k];
        } else if (id == 3u && n >= 1u && v[0] >= 1u && v[0] <= 14u) {   /* DS Parameter Set */
            b->channel = v[0];
        } else if (id == 48u) {                                 /* RSN */
            rsn = 1;
            if (n + 2u <= sizeof b->rsn) { copy(b->rsn, e, n + 2u); b->rsn_len = (uint8_t)(n + 2u); }
        } else if (id == 221u && n >= 4u && same(v, WPA_OUI, 4)) {   /* WPA's vendor element */
            wpa = 1;
        }
    }
    b->sec = rsn ? FM1WIFI_SEC_WPA2 : wpa ? FM1WIFI_SEC_WPA : (b->cap & 0x0010u) ? FM1WIFI_SEC_WEP : FM1WIFI_SEC_OPEN;
    return FM1WIFI_OK;
}

void fm1wifi_scan_clear(fm1wifi_scan_list *l) { l->n = 0; l->full = 0; }

fm1wifi_bss *fm1wifi_scan_add(fm1wifi_scan_list *l, const fm1wifi_bss *b)
{
    uint32_t i;
    for (i = 0; i < l->n; i++) {
        fm1wifi_bss *e = &l->e[i];
        if (!same(e->bssid, b->bssid, 6)) continue;
        if (b->rssi > e->rssi) e->rssi = b->rssi;
        if (!e->ssid_len && b->ssid_len) { copy((uint8_t *)e->ssid, (const uint8_t *)b->ssid, sizeof e->ssid); e->ssid_len = b->ssid_len; }
        if (!e->n_rates) { copy(e->rates, b->rates, sizeof e->rates); e->n_rates = b->n_rates; }
        if (!e->rsn_len && b->rsn_len) { copy(e->rsn, b->rsn, sizeof e->rsn); e->rsn_len = b->rsn_len; }
        e->probed |= b->probed;
        if (e->heard < 255u) e->heard++;
        return e;
    }
    if (l->n >= FM1WIFI_SCAN_MAX) { if (l->full < 255u) l->full++; return 0; }
    l->e[l->n] = *b;
    return &l->e[l->n++];
}

const fm1wifi_bss *fm1wifi_scan_find(const fm1wifi_scan_list *l, const char *ssid)
{
    const fm1wifi_bss *best = 0;
    uint32_t i, k;
    for (i = 0; i < l->n; i++) {
        const fm1wifi_bss *e = &l->e[i];
        for (k = 0; ssid[k] && e->ssid[k] == ssid[k]; k++) {}
        if (ssid[k] || e->ssid[k]) continue;
        if (!best || e->rssi > best->rssi) best = e;
    }
    return best;
}

/* ---- the TX FIFO image */

#define TX_QSEL_EDCA 2u          /* rt2800usb: every frame goes to the EDCA queue */
#define TX_OP_BACKOFF 3u         /* rt2x00's TXOP_BACKOFF: the frame contends for the medium */

uint32_t fm1wifi_tx_image(uint8_t *img, const uint8_t *frame, uint32_t len, unsigned flags)
{
    uint32_t w0, w1, i;
    if (!len || len > FM1WIFI_TX_FRAME_MAX) return 0;
    put32(img, len + 22u);                                       /* JieLi's word: the DMA starts after it */
    put32(img + 4, 1u | TX_QSEL_EDCA << 1);                      /* WIV, QSEL */
    w0 = TX_OP_BACKOFF << 8 | ((flags & FM1WIFI_TX_OFDM6) ? 1u << 30 : 0u);   /* MCS 0: CCK 1 or OFDM 6 Mbit/s */
    w1 = (flags & FM1WIFI_TX_ACK ? FM1WIFI_TXWI_W1_ACK : 0u) | 0xFFu << 8 | len << 16;   /* WCID 255: no entry */
    put32(img + 8, w0);
    put32(img + 12, w1);
    put32(img + 16, 0);                                          /* IV, EIV: no hardware encryption yet */
    put32(img + 20, 0);
    copy(img + FM1WIFI_TX_HDR, frame, len);
    for (i = FM1WIFI_TX_HDR + len; i < 4u + len + 26u; i++) img[i] = 0;
    return len + 26u;
}

/* ---- a station's management frames */

/* The rates the FM-1 says it supports: 802.11b's four (basic) and 802.11g's eight, in 500 kbit/s units. */
static const uint8_t RATES[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
static const uint8_t EXT_RATES[4] = { 0x30, 0x48, 0x60, 0x6C };
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
#define DUR_ACK_1M 314u          /* SIFS 10 us + an ACK at 1 Mbit/s with the long preamble (192 + 112 us) */

static uint32_t mgmt(uint8_t *f, uint32_t kind, uint32_t dur, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid,
                     uint16_t seq)
{
    f[0] = (uint8_t)kind; f[1] = 0;
    put16(f + 2, dur);
    copy(f + 4, da, 6);
    copy(f + 10, sa, 6);
    copy(f + 16, bssid, 6);
    put16(f + 22, (uint32_t)(seq & 0x0FFFu) << 4);
    return 24u;
}

static uint32_t elem(uint8_t *f, uint32_t id, const uint8_t *v, uint32_t n)
{
    f[0] = (uint8_t)id; f[1] = (uint8_t)n;
    copy(f + 2, v, n);
    return 2u + n;
}

static uint32_t rates(uint8_t *f)
{
    uint32_t n = elem(f, 1, RATES, sizeof RATES);
    return n + elem(f + n, 50, EXT_RATES, sizeof EXT_RATES);
}

uint32_t fm1wifi_probe_req(uint8_t *f, const uint8_t sa[6], const char *ssid, uint16_t seq)
{
    uint32_t n = mgmt(f, FM1WIFI_FC_PROBE_REQ, 0, BCAST, sa, BCAST, seq), k = 0;
    while (ssid && k < 32u && ssid[k]) k++;
    n += elem(f + n, 0, (const uint8_t *)ssid, k);
    return n + rates(f + n);
}

uint32_t fm1wifi_auth_open(uint8_t *f, const uint8_t sa[6], const uint8_t bssid[6], uint16_t seq)
{
    uint32_t n = mgmt(f, FM1WIFI_FC_AUTH, DUR_ACK_1M, bssid, sa, bssid, seq);
    put16(f + n, 0);                   /* open system */
    put16(f + n + 2, 1);               /* transaction sequence 1 */
    put16(f + n + 4, 0);               /* status */
    return n + 6u;
}

uint32_t fm1wifi_assoc_req(uint8_t *f, const uint8_t sa[6], const fm1wifi_bss *b, const uint8_t *ies, uint32_t n_ies,
                           uint16_t seq)
{
    uint32_t n = mgmt(f, FM1WIFI_FC_ASSOC_REQ, DUR_ACK_1M, b->bssid, sa, b->bssid, seq);
    put16(f + n, 0x0001u | (b->cap & 0x0430u));   /* ESS; privacy, short preamble and short slot as the AP has them */
    put16(f + n + 2, 10);                          /* listen interval: 10 beacons */
    n += 4u;
    n += elem(f + n, 0, (const uint8_t *)b->ssid, b->ssid_len);
    n += rates(f + n);
    if (ies && n_ies) { copy(f + n, ies, n_ies); n += n_ies; }
    return n;
}

uint32_t fm1wifi_deauth(uint8_t *f, const uint8_t sa[6], const uint8_t bssid[6], uint16_t reason, uint16_t seq)
{
    uint32_t n = mgmt(f, FM1WIFI_FC_DEAUTH, DUR_ACK_1M, bssid, sa, bssid, seq);
    put16(f + n, reason);
    return n + 2u;
}

/* ---- the station */

const char *fm1wifi_sta_state_name(unsigned s)
{
    static const char *const N[] = { "IDLE", "AUTH", "ASSOC", "UP", "DOWN" };
    return s < 5u ? N[s] : "?";
}

const char *fm1wifi_why_name(unsigned why)
{
    static const char *const N[] = { "-", "AUTH TIMEOUT", "AUTH REFUSED", "ASSOC TIMEOUT", "ASSOC REFUSED", "DEAUTH",
                                     "DISASSOC", "BEACON LOSS", "LEFT" };
    return why < sizeof N / sizeof N[0] ? N[why] : "?";
}

void fm1wifi_sta_start(fm1wifi_sta *s, const uint8_t sa[6], const fm1wifi_bss *b, uint32_t now)
{
    const uint8_t *ies = s->ies;
    uint32_t n_ies = s->n_ies, i;
    uint16_t seq = s->seq;
    uint8_t *z = (uint8_t *)s;
    for (i = 0; i < sizeof *s; i++) z[i] = 0;
    s->ies = ies; s->n_ies = n_ies; s->seq = seq;    /* kept across a rejoin */
    copy(s->sa, sa, 6);
    s->bss = *b;
    s->state = FM1WIFI_STA_AUTH;
    s->t_sent = s->t_beacon = now;
}

static void down(fm1wifi_sta *s, unsigned why) { s->state = FM1WIFI_STA_DOWN; s->why = (uint8_t)why; }

static void sta_rx(fm1wifi_sta *s, const fm1wifi_rx *f, uint32_t now)
{
    uint32_t kind = FM1WIFI_FC_KIND(f->fc);
    if (!f->frame || (f->fc >> 2 & 3u) != 0u || f->hdr_len != 24u) return;    /* management frames only */
    if (!same(f->frame + 16, s->bss.bssid, 6)) return;                         /* from the BSS */
    if (kind == FM1WIFI_FC_BEACON) { s->t_beacon = now; if (s->state == FM1WIFI_STA_UP) s->beacons++; return; }
    if (!same(f->frame + 4, s->sa, 6) || !same(f->frame + 10, s->bss.bssid, 6)) return;   /* to us, from the AP */
    if (kind == FM1WIFI_FC_AUTH && s->state == FM1WIFI_STA_AUTH && f->body_len >= 6u &&
        le16(f->body) == 0u && le16(f->body + 2) == 2u) {
        s->status = (uint16_t)le16(f->body + 4);
        if (s->status) { down(s, FM1WIFI_WHY_AUTH_REFUSED); return; }
        s->state = FM1WIFI_STA_ASSOC;
        s->tries = 0;
    } else if (kind == FM1WIFI_FC_ASSOC_RESP && s->state == FM1WIFI_STA_ASSOC && f->body_len >= 6u) {
        s->status = (uint16_t)le16(f->body + 2);
        if (s->status) { down(s, FM1WIFI_WHY_ASSOC_REFUSED); return; }
        s->aid = (uint16_t)(le16(f->body + 4) & 0x3FFFu);
        s->state = FM1WIFI_STA_UP;
        s->t_beacon = now;
        s->beacons = 0;
    } else if ((kind == FM1WIFI_FC_DEAUTH || kind == FM1WIFI_FC_DISASSOC) && f->body_len >= 2u &&
               (s->state == FM1WIFI_STA_AUTH || s->state == FM1WIFI_STA_ASSOC || s->state == FM1WIFI_STA_UP)) {
        s->reason = (uint16_t)le16(f->body);
        down(s, kind == FM1WIFI_FC_DEAUTH ? FM1WIFI_WHY_DEAUTH : FM1WIFI_WHY_DISASSOC);
    }
}

uint32_t fm1wifi_sta_step(fm1wifi_sta *s, const fm1wifi_rx *f, uint32_t now, uint8_t *out, unsigned *flags)
{
    uint32_t loss;
    *flags = 0;
    if (f) sta_rx(s, f, now);
    if (s->state == FM1WIFI_STA_AUTH || s->state == FM1WIFI_STA_ASSOC) {
        if (s->tries && now - s->t_sent < FM1WIFI_STA_WAIT_MS) return 0;
        if (s->tries >= FM1WIFI_STA_TRIES) {
            down(s, s->state == FM1WIFI_STA_AUTH ? FM1WIFI_WHY_AUTH_TIMEOUT : FM1WIFI_WHY_ASSOC_TIMEOUT);
            return 0;
        }
        s->tries++;
        s->t_sent = now;
        *flags = FM1WIFI_TX_ACK;
        return s->state == FM1WIFI_STA_AUTH ? fm1wifi_auth_open(out, s->sa, s->bss.bssid, s->seq++)
                                            : fm1wifi_assoc_req(out, s->sa, &s->bss, s->ies, s->n_ies, s->seq++);
    }
    if (s->state == FM1WIFI_STA_UP) {
        loss = FM1WIFI_STA_LOSS_BEACONS * (s->bss.interval ? s->bss.interval : 100u) * 1024u / 1000u;
        if (loss < 1000u) loss = 1000u;
        if (now - s->t_beacon >= loss) down(s, FM1WIFI_WHY_BEACON_LOSS);
    }
    return 0;
}

uint32_t fm1wifi_sta_leave(fm1wifi_sta *s, uint8_t *out, unsigned *flags)
{
    int authed = s->state == FM1WIFI_STA_ASSOC || s->state == FM1WIFI_STA_UP;
    *flags = 0;
    if (s->state != FM1WIFI_STA_IDLE) down(s, FM1WIFI_WHY_LEFT);
    if (!authed) return 0;
    *flags = FM1WIFI_TX_ACK;
    return fm1wifi_deauth(out, s->sa, s->bss.bssid, 3, s->seq++);   /* reason 3: leaving */
}
