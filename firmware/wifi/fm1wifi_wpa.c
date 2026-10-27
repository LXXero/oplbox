/* SPDX-License-Identifier: GPL-3.0-only
 * fm1wifi_wpa: a WPA2-PSK station (stage 8): fm1wifi_frame.c's join, wpa_eapol.c's handshakes, the MAC's key tables.
 * See fm1wifi_wpa.h. Own code, from IEEE 802.11-2020 9.4.2.24 (the RSN element), 12.7.6 and 12.7.7. Not run on an
 * FM-1 yet.
 */
#include "fm1wifi_wpa.h"

static void copy(uint8_t *d, const uint8_t *s, uint32_t n) { while (n--) *d++ = *s++; }
static int same(const uint8_t *a, const uint8_t *b, uint32_t n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static void wipe(void *p, uint32_t n) { volatile uint8_t *z = p; while (n--) *z++ = 0; }

const char *fm1wifi_wpa_state_name(unsigned s)
{
    static const char *const N[] = { "IDLE", "JOIN", "4-WAY", "UP", "DOWN" };
    return s < 5u ? N[s] : "?";
}

const char *fm1wifi_wpa_why_name(unsigned why)
{
    static const char *const N[] = { "-", "NOT WPA2-PSK", "JOIN", "NO MSG 1", "NO MSG 3", "HANDSHAKE", "KEY TABLES",
                                     "LINK LOST", "LEFT" };
    return why < sizeof N / sizeof N[0] ? N[why] : "?";
}

const char *fm1wifi_rsn_name(unsigned why)
{
    static const char *const N[] = { "OK", "NO RSNE", "BAD RSNE", "NO CCMP GROUP", "NO CCMP PAIR", "NO PSK", "MFP REQUIRED" };
    return why < sizeof N / sizeof N[0] ? N[why] : "?";
}

const char *fm1wifi_eapol_reason_name(unsigned r)
{
    static const char *const N[] = { "OK", "SHORT", "NOT KEY", "DESC TYPE", "VERSION", "KEY INFO", "KEY LEN", "STATE",
                                     "REPLAY", "NONCE", "KEY DATA LEN", "MIC", "RSNE", "UNWRAP", "GTK", "RNG", "BUF" };
    return r < sizeof N / sizeof N[0] ? N[r] : "?";
}

int fm1wifi_wpa_readback_ok(const fm1wifi_wpa *w, int group)
{
    const fm1wifi_keyread *k = group ? &w->kr_group : &w->kr_pair;
    const uint8_t *a = w->sta.bss.bssid;
    uint32_t idx = w->gtk_idx & 3u;
    if (!(w->loaded & (group ? WPA_EAPOL_NEW_GTK : WPA_EAPOL_NEW_PTK)) || k->err != FM1WIFI_OK || k->key_same != 0xFFu) return 0;
    if (group)
        return (k->mode >> 4u * idx & 0xFu) == FM1WIFI_CIPHER_AES && k->attr == FM1WIFI_CIPHER_AES << 1 &&
               k->iv[0] == ((FM1WIFI_IV3_EXT_IV | idx << 6) << 24 | 1u) && k->iv[1] == 0;
    return k->wcid[0] == (a[0] | (uint32_t)a[1] << 8 | (uint32_t)a[2] << 16 | (uint32_t)a[3] << 24) &&
           k->wcid[1] == (a[4] | (uint32_t)a[5] << 8 | 0xFFFF0000u) && k->attr == (MAC_WCID_ATTRIBUTE_KEYTAB | FM1WIFI_CIPHER_AES << 1) &&
           k->iv[0] == ((uint32_t)FM1WIFI_IV3_EXT_IV << 24 | 1u) && k->iv[1] == 0;
}

const char *fm1wifi_wpa_msg_name(unsigned m)
{
    static const char *const N[] = { "OTHER", "MSG 1", "MSG 2", "MSG 3", "MSG 4", "GROUP 1", "GROUP 2" };
    return m < sizeof N / sizeof N[0] ? N[m] : "?";
}

/* ---- the RSN element (9.4.2.24): version, group suite, pairwise suites, AKM suites, capabilities; a field missing
 * at the end takes its default (CCMP, CCMP, PSK, 0) */

static const uint8_t OUI_CCMP[4] = { 0x00, 0x0F, 0xAC, 4 }, OUI_PSK[4] = { 0x00, 0x0F, 0xAC, 2 };
#define RSN_CAP_MFPR 0x0040u      /* management frame protection required (bit 6) */

static int suites_have(const uint8_t *p, uint32_t n, const uint8_t want[4])
{
    for (uint32_t i = 0; i < n; i++) if (same(p + 4u * i, want, 4)) return 1;
    return 0;
}

int fm1wifi_wpa_rsn(const fm1wifi_bss *b, uint8_t own[FM1WIFI_WPA_RSNE_LEN])
{
    const uint8_t *e = b->rsn, *group = OUI_CCMP;
    uint32_t len, at = 4, n;
    if (!b->rsn_len) return FM1WIFI_RSN_NONE;
    len = 2u + e[1];
    if (e[0] != 48u || len != b->rsn_len || len < 4u || (e[2] | e[3] << 8) != 1u) return FM1WIFI_RSN_BAD;
    if (at + 4u <= len) { group = e + at; at += 4u; }
    if (!same(group, OUI_CCMP, 4)) return FM1WIFI_RSN_GROUP;
    if (at + 2u <= len) {
        n = e[at] | (uint32_t)e[at + 1] << 8;
        if (at + 2u + 4u * n > len) return FM1WIFI_RSN_BAD;
        if (!suites_have(e + at + 2, n, OUI_CCMP)) return FM1WIFI_RSN_PAIRWISE;
        at += 2u + 4u * n;
    }
    if (at + 2u <= len) {
        n = e[at] | (uint32_t)e[at + 1] << 8;
        if (at + 2u + 4u * n > len) return FM1WIFI_RSN_BAD;
        if (!suites_have(e + at + 2, n, OUI_PSK)) return FM1WIFI_RSN_AKM;
        at += 2u + 4u * n;
    }
    if (at + 2u <= len && ((e[at] | e[at + 1] << 8) & RSN_CAP_MFPR)) return FM1WIFI_RSN_MFP;
    own[0] = 48; own[1] = FM1WIFI_WPA_RSNE_LEN - 2u; own[2] = 1; own[3] = 0;
    copy(own + 4, OUI_CCMP, 4);
    own[8] = 1; own[9] = 0; copy(own + 10, OUI_CCMP, 4);
    own[14] = 1; own[15] = 0; copy(own + 16, OUI_PSK, 4);
    own[20] = 0; own[21] = 0;                                   /* capabilities: no MFP, no PMKSA cache */
    return FM1WIFI_RSN_OK;
}

/* ---- the station */

#define KI_P 0x0008u
#define KI_ACK 0x0080u
#define KI_MIC 0x0100u
#define KI_SECURE 0x0200u

static unsigned msg_kind(const uint8_t *p, uint32_t n, int from_ap)
{
    uint32_t ki;
    if (n < 7u || p[1] != 3u) return FM1WIFI_WPA_MSG_OTHER;       /* 802.1X packet type 3: EAPOL-Key */
    ki = (uint32_t)p[5] << 8 | p[6];
    if (from_ap != !!(ki & KI_ACK)) return FM1WIFI_WPA_MSG_OTHER;
    if (!(ki & KI_P)) return from_ap ? FM1WIFI_WPA_MSG_G1 : FM1WIFI_WPA_MSG_G2;
    if (from_ap) return ki & KI_MIC ? FM1WIFI_WPA_MSG_3 : FM1WIFI_WPA_MSG_1;
    return ki & KI_SECURE ? FM1WIFI_WPA_MSG_4 : FM1WIFI_WPA_MSG_2;
}

static fm1wifi_wpa_rec *rec(fm1wifi_wpa *w, int dir, unsigned msg, const uint8_t *p, uint32_t n, int prot, uint32_t now)
{
    fm1wifi_wpa_rec *r;
    uint32_t t = now - w->t_assoc;
    w->got |= (uint8_t)(1u << msg);
    if (w->n_rec >= FM1WIFI_WPA_REC_N) return 0;
    r = &w->rec[w->n_rec++];
    r->dir = (uint8_t)dir; r->msg = (uint8_t)msg; r->reason = 0; r->prot = (uint8_t)prot;
    r->t = (uint16_t)(t > 0xFFFFu ? 0xFFFFu : t);
    r->len = (uint16_t)n;
    copy(r->b, p, n < FM1WIFI_WPA_REC_LEN ? n : FM1WIFI_WPA_REC_LEN);
    return r;
}

void fm1wifi_wpa_start(fm1wifi_wpa *w, const uint8_t sa[6], const fm1wifi_bss *b, const uint8_t pmk[WPA_PMK_LEN],
                       wpa_eapol_nonce_fn nonce, void *nonce_arg, uint32_t now)
{
    uint16_t seq = w->sta.seq;
    wipe(w, sizeof *w);
    w->sta.seq = seq;                                            /* the sequence numbers go on across a rejoin */
    w->rsn = (uint8_t)fm1wifi_wpa_rsn(b, w->rsne);
    copy(w->pmk, pmk, WPA_PMK_LEN);
    w->nonce = nonce; w->nonce_arg = nonce_arg;
    w->key_err = FM1WIFI_OK;
    w->sta.ies = w->rsne;
    w->sta.n_ies = FM1WIFI_WPA_RSNE_LEN;
    if (w->rsn != FM1WIFI_RSN_OK) { w->state = FM1WIFI_WPA_DOWN; w->why = FM1WIFI_WPA_WHY_RSN; return; }
    fm1wifi_sta_start(&w->sta, sa, b, now);
    w->state = FM1WIFI_WPA_JOIN;
}

/* DOWN for why: the handshake's secrets wiped, the deauthentication into out (the station was authenticated) */
static uint32_t fail(fm1wifi_wpa *w, unsigned why, uint8_t *out, unsigned *flags)
{
    uint32_t len = fm1wifi_sta_leave(&w->sta, out, flags);
    w->state = FM1WIFI_WPA_DOWN;
    w->why = (uint8_t)why;
    w->install = 0;
    wpa_eapol_clear(&w->e);
    return len;
}

/* a protected data frame from the AP (to us, or group addressed): counted as the MAC decrypted it or not */
static void count_protected(fm1wifi_wpa *w, const fm1wifi_rx *f)
{
    int group, ok;
    if (!f || !f->frame || (f->fc >> 2 & 3u) != 2u || !(f->fc & FM1WIFI_FC_PROTECTED)) return;
    if ((f->fc & (FM1WIFI_FC_TO_DS | FM1WIFI_FC_FROM_DS)) != FM1WIFI_FC_FROM_DS || !same(f->frame + 10, w->sta.bss.bssid, 6))
        return;
    group = f->frame[4] & 1u;
    if (!group && !same(f->frame + 4, w->sta.sa, 6)) return;
    ok = (f->info & FM1WIFI_RXD_DECRYPTED) && !(f->info & FM1WIFI_RXD_CIPHER_ERR);
    w->prot_rx++;
    if (ok) w->prot_ok++; else w->prot_err++;
    if (group) { w->group_rx++; if (ok) w->group_ok++; }
}

uint32_t fm1wifi_wpa_step(fm1wifi_wpa *w, const fm1wifi_rx *f, uint32_t now, uint8_t *out, unsigned *flags)
{
    uint8_t reply[WPA_EAPOL_REPLY_MAX];
    const uint8_t *pdu;
    size_t rlen = 0;
    uint32_t len, n;
    *flags = 0;
    if (w->state != FM1WIFI_WPA_JOIN && w->state != FM1WIFI_WPA_4WAY && w->state != FM1WIFI_WPA_UP) return 0;
    len = fm1wifi_sta_step(&w->sta, f, now, out, flags);
    if (w->state == FM1WIFI_WPA_JOIN) {
        if (w->sta.state == FM1WIFI_STA_DOWN) { w->state = FM1WIFI_WPA_DOWN; w->why = FM1WIFI_WPA_WHY_JOIN; }
        else if (w->sta.state == FM1WIFI_STA_UP) {
            w->state = FM1WIFI_WPA_4WAY;
            w->t_assoc = now;
            if (wpa_eapol_init(&w->e, w->pmk, w->sta.sa, w->sta.bss.bssid, w->rsne, FM1WIFI_WPA_RSNE_LEN, w->sta.bss.rsn,
                               w->sta.bss.rsn_len, w->nonce, w->nonce_arg))
                return fail(w, FM1WIFI_WPA_WHY_RSN, out, flags);
        }
        return len;
    }
    if (w->sta.state != FM1WIFI_STA_UP) {                       /* deauthenticated, disassociated, beacons lost */
        w->state = FM1WIFI_WPA_DOWN;
        w->why = FM1WIFI_WPA_WHY_LINK;
        w->install = 0;
        wpa_eapol_clear(&w->e);
        return 0;
    }
    count_protected(w, f);
    n = f ? fm1wifi_eapol_pdu(f, w->sta.bss.bssid, w->sta.sa, &pdu) : 0;
    if (n) {
        unsigned msg = msg_kind(pdu, n, 1);
        fm1wifi_wpa_rec *r = rec(w, 0, msg, pdu, n, (f->fc & FM1WIFI_FC_PROTECTED) != 0, now);
        enum wpa_eapol_reason why = wpa_eapol_rx(&w->e, pdu, n, reply, sizeof reply, &rlen);
        w->eapol_rx++;
        if (r) r->reason = (uint8_t)why;
        if (why != WPA_EAPOL_OK) { w->eapol_refused++; w->eapol_reason = (uint8_t)why; }
        if (w->e.state == WPA_EAPOL_FAILED) return fail(w, FM1WIFI_WPA_WHY_EAPOL, out, flags);
        w->install |= w->e.new_keys;
        if (why == WPA_EAPOL_OK && msg == FM1WIFI_WPA_MSG_G1 && w->state == FM1WIFI_WPA_UP) w->group_handshakes++;
        if (rlen) {
            int prot = (w->keys & WPA_EAPOL_NEW_PTK) != 0;      /* msg 4 goes out before the PTK is loaded: in the clear */
            len = fm1wifi_eapol_frame(out, w->sta.sa, w->sta.bss.bssid, reply, (uint32_t)rlen, prot, w->sta.seq++);
            *flags = FM1WIFI_TX_ACK | (prot ? FM1WIFI_TX_CRYPT | FM1WIFI_TX_WCID(FM1WIFI_WCID_AP) : 0u);
            rec(w, 1, msg_kind(reply, (uint32_t)rlen, 0), reply, (uint32_t)rlen, prot, now);
            w->eapol_tx++;
        }
        wipe(reply, sizeof reply);
        if (w->state == FM1WIFI_WPA_4WAY && w->e.state == WPA_EAPOL_DONE) {
            w->state = FM1WIFI_WPA_UP;
            w->t_4way = now - w->t_assoc ? now - w->t_assoc : 1u;
        }
        return len;
    }
    if (w->state == FM1WIFI_WPA_4WAY) {
        if (!(w->got & 1u << FM1WIFI_WPA_MSG_1) && now - w->t_assoc >= FM1WIFI_WPA_MSG1_MS)
            return fail(w, FM1WIFI_WPA_WHY_NO_MSG1, out, flags);
        if (now - w->t_assoc >= FM1WIFI_WPA_4WAY_MS) return fail(w, FM1WIFI_WPA_WHY_NO_MSG3, out, flags);
    }
    return 0;
}

uint32_t fm1wifi_wpa_leave(fm1wifi_wpa *w, uint8_t *out, unsigned *flags)
{
    uint32_t len = fm1wifi_sta_leave(&w->sta, out, flags);
    if (w->state != FM1WIFI_WPA_IDLE && w->state != FM1WIFI_WPA_DOWN) { w->state = FM1WIFI_WPA_DOWN; w->why = FM1WIFI_WPA_WHY_LEFT; }
    w->install = 0;
    wpa_eapol_clear(&w->e);
    return len;
}

void fm1wifi_wpa_wipe(fm1wifi_wpa *w)
{
    wpa_eapol_clear(&w->e);
    wipe(w->pmk, sizeof w->pmk);
    w->install = 0;
}

/* ---- the driver */

static uint8_t wpa_out[FM1WIFI_WPA_OUT];

/* the keys the last step agreed, into the tables (after its frame went out: msg 4 before the PTK) */
static void load(fm1wifi_wpa *w, fm1wifi_frames *fr)
{
    int r = FM1WIFI_OK;
    unsigned flags;
    uint32_t len;
    if (w->install & WPA_EAPOL_NEW_PTK) {
        r = fm1wifi_key_pairwise(FM1WIFI_WCID_AP, w->sta.bss.bssid, w->e.tk, &w->kr_pair);
        if (r == FM1WIFI_OK) w->keys |= WPA_EAPOL_NEW_PTK;
    }
    if (r == FM1WIFI_OK && (w->install & WPA_EAPOL_NEW_GTK)) {
        r = w->e.gtk_len == 16u ? fm1wifi_key_group(FM1WIFI_WCID_GROUP, w->e.gtk_idx & 3u, w->e.gtk, &w->kr_group)
                                : FM1WIFI_ERANGE;
        if (r == FM1WIFI_OK) { w->keys |= WPA_EAPOL_NEW_GTK; w->gtk_idx = w->e.gtk_idx & 3u; }
    }
    w->install = 0;
    w->loaded |= w->keys;
    if (r == FM1WIFI_OK) return;
    w->key_err = r;
    len = fail(w, FM1WIFI_WPA_WHY_KEYS, wpa_out, &flags);
    if (len) fm1wifi_send(wpa_out, len, flags, fr);
}

static void unload(fm1wifi_wpa *w)
{
    if (!w->keys) return;
    fm1wifi_key_clear(FM1WIFI_WCID_AP, FM1WIFI_WCID_GROUP);
    w->keys = 0;
}

#define FRAMES_PER_MS 50u         /* as fm1wifi.c: frames taken without advancing the clock, at most */

/* the station for ms milliseconds, or (until_settled) until it is UP or DOWN; else while it stays UP */
static int run(fm1wifi_wpa *w, unsigned ms, int until_settled, fm1wifi_frames *fr)
{
    uint32_t t = 0, burst = 0, len;
    unsigned flags;
    fm1wifi_rx f;
    while (t < ms * 10u) {
        int k = fm1wifi_poll(&f, w->sta.bss.channel, fr);
        if (k < 0) { unload(w); return k; }
        len = fm1wifi_wpa_step(w, k && f.frame ? &f : 0, fm1wifi_now_ms(), wpa_out, &flags);
        if (len) fm1wifi_send(wpa_out, len, flags, fr);
        if (w->install) load(w, fr);
        if (w->state == FM1WIFI_WPA_DOWN) unload(w);
        if (until_settled ? w->state == FM1WIFI_WPA_UP || w->state == FM1WIFI_WPA_DOWN : w->state != FM1WIFI_WPA_UP) break;
        if (k && burst++ < FRAMES_PER_MS) continue;
        burst = 0;
        fm1wifi_wait_us(100);
        t++;
    }
    return FM1WIFI_OK;
}

int fm1wifi_wpa_join(fm1wifi_wpa *w, const fm1wifi_bss *b, const uint8_t pmk[WPA_PMK_LEN], wpa_eapol_nonce_fn nonce,
                     void *nonce_arg, unsigned ms, fm1wifi_frames *fr)
{
    int r;
    if (!fm1wifi_link_is_up()) return FM1WIFI_ESTATE;
    r = fm1wifi_tune(b->channel);
    if (r == FM1WIFI_OK) r = fm1wifi_set_bssid(b->bssid);
    if (r != FM1WIFI_OK) return r;
    fm1wifi_wpa_start(w, fm1wifi_link_mac(), b, pmk, nonce, nonce_arg, fm1wifi_now_ms());
    if (w->state == FM1WIFI_WPA_DOWN) return FM1WIFI_OK;
    return run(w, ms, 1, fr);
}

int fm1wifi_wpa_listen(fm1wifi_wpa *w, unsigned ms, fm1wifi_frames *fr)
{
    if (!fm1wifi_link_is_up()) return FM1WIFI_ESTATE;
    return w->state == FM1WIFI_WPA_UP ? run(w, ms, 0, fr) : FM1WIFI_OK;
}

int fm1wifi_wpa_leave_now(fm1wifi_wpa *w, fm1wifi_frames *fr)
{
    unsigned flags;
    uint32_t len;
    int r = FM1WIFI_OK;
    if (!fm1wifi_link_is_up()) return FM1WIFI_ESTATE;
    len = fm1wifi_wpa_leave(w, wpa_out, &flags);
    if (len) r = fm1wifi_send(wpa_out, len, flags, fr);
    unload(w);
    return r;
}
