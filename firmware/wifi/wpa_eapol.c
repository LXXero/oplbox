/* SPDX-License-Identifier: GPL-3.0-only
 * The station side of the WPA2-PSK EAPOL-Key handshakes (see wpa_eapol.h). Own code from IEEE 802.11-2020 12.7.2
 * (EAPOL-Key frame layout, key information bits, key data and KDE encapsulation), 12.7.6 (4-way handshake) and
 * 12.7.7 (group key handshake). The MIC is HMAC-SHA1 (BearSSL, through wpa_crypto.h) cut to 16 bytes, the key data
 * is unwrapped with wpa_aes_unwrap.
 */
#include "wpa_eapol.h"           /* memcpy and memset come declared through bearssl_hash.h (no C library headers) */

/* key information (12.7.2, figure 12-33) */
#define KI_VER      0x0007u
#define KI_PAIRWISE 0x0008u
#define KI_INSTALL  0x0040u
#define KI_ACK      0x0080u
#define KI_MIC      0x0100u
#define KI_SECURE   0x0200u
#define KI_ERROR    0x0400u
#define KI_REQUEST  0x0800u
#define KI_ENC      0x1000u
#define KI_SMK      0x2000u
#define KEY_DESC_VER 2              /* HMAC-SHA1-128 MIC, AES key wrap: AKM 2 with CCMP */
#define DESC_TYPE_80211 2
#define EAPOL_KEY 3                 /* 802.1X packet type */

/* byte offsets in the frame, the 802.1X header included */
#define O_DESC 4
#define O_INFO 5
#define O_KEYLEN 7
#define O_REPLAY 9
#define O_NONCE 17
#define O_RSC 65
#define O_MIC 81
#define O_KDLEN 97
#define O_KD 99

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--)
        *v++ = 0;
}

/* nonzero when the n bytes differ (memcmp is not declared without the C library's headers) */
static int differ(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    while (n--)
        d |= *a++ ^ *b++;
    return d;
}

static unsigned be16(const uint8_t *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* a > b, both 8-byte big-endian counters */
static int above(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 8; i++)
        if (a[i] != b[i])
            return a[i] > b[i];
    return 0;
}

/* HMAC-SHA1-128 over the frame with its MIC field taken as zero (12.7.2 "Key MIC") */
static void mic(const uint8_t kck[16], const uint8_t *f, size_t flen, uint8_t out[16])
{
    static const uint8_t zero[16];
    br_hmac_key_context k;
    br_hmac_context c;
    uint8_t h[20];
    br_hmac_key_init(&k, &br_sha1_vtable, kck, 16);
    br_hmac_init(&c, &k, 0);
    br_hmac_update(&c, f, O_MIC);
    br_hmac_update(&c, zero, 16);
    br_hmac_update(&c, f + O_MIC + 16, flen - (O_MIC + 16));
    br_hmac_out(&c, h);
    memcpy(out, h, 16);
    wipe(&k, sizeof k);
    wipe(&c, sizeof c);
    wipe(h, sizeof h);
}

static int mic_ok(const uint8_t kck[16], const uint8_t *f, size_t flen)
{
    uint8_t m[16], bad = 0;
    mic(kck, f, flen, m);
    for (int i = 0; i < 16; i++)                /* every byte compared, so the time does not say which */
        bad |= m[i] ^ f[O_MIC + i];
    wipe(m, sizeof m);
    return !bad;
}

/* An EAPOL-Key frame from the station: everything zero but the fields given, MIC computed under kck. */
static size_t reply_frame(uint8_t *r, uint8_t version, unsigned info, const uint8_t replay[8],
                          const uint8_t *nonce, const uint8_t *kd, size_t kdl, const uint8_t kck[16])
{
    size_t len = WPA_EAPOL_HDR + WPA_EAPOL_KEY_FIXED + kdl;
    memset(r, 0, len);
    r[0] = version;
    r[1] = EAPOL_KEY;
    put16(r + 2, (unsigned)(WPA_EAPOL_KEY_FIXED + kdl));
    r[O_DESC] = DESC_TYPE_80211;
    put16(r + O_INFO, info);                    /* key length 0 in msg 2, 4 and group msg 2 (12.7.6.3, .5, 12.7.7.3) */
    memcpy(r + O_REPLAY, replay, 8);
    if (nonce)
        memcpy(r + O_NONCE, nonce, 32);
    put16(r + O_KDLEN, (unsigned)kdl);
    if (kdl)
        memcpy(r + O_KD, kd, kdl);
    mic(kck, r, len, r + O_MIC);
    return len;
}

static enum wpa_eapol_reason fail(struct wpa_eapol *s, enum wpa_eapol_reason why)
{
    wipe(s->tptk, sizeof s->tptk);
    wipe(s->ptk, sizeof s->ptk);
    wipe(s->tk, sizeof s->tk);
    wipe(s->gtk, sizeof s->gtk);
    s->gtk_len = 0;
    s->new_keys = 0;
    s->state = WPA_EAPOL_FAILED;
    return s->reason = (uint8_t)why;
}

static int rsne_ok(const uint8_t *e, size_t len)
{
    return len >= 4 && len <= WPA_EAPOL_RSNE_MAX && e[0] == 48 && (size_t)e[1] + 2 == len;
}

int wpa_eapol_init(struct wpa_eapol *s, const uint8_t pmk[WPA_PMK_LEN], const uint8_t spa[6], const uint8_t aa[6],
                   const uint8_t *own_rsne, size_t own_rsne_len, const uint8_t *ap_rsne, size_t ap_rsne_len,
                   wpa_eapol_nonce_fn nonce, void *nonce_arg)
{
    wipe(s, sizeof *s);
    if (!rsne_ok(own_rsne, own_rsne_len) || !rsne_ok(ap_rsne, ap_rsne_len) || !nonce) {
        s->state = WPA_EAPOL_FAILED;
        s->reason = WPA_EAPOL_E_RSNE;
        return -1;
    }
    memcpy(s->pmk, pmk, WPA_PMK_LEN);
    memcpy(s->spa, spa, 6);
    memcpy(s->aa, aa, 6);
    memcpy(s->own_rsne, own_rsne, own_rsne_len);
    s->own_rsne_len = (uint8_t)own_rsne_len;
    memcpy(s->ap_rsne, ap_rsne, ap_rsne_len);
    s->ap_rsne_len = (uint8_t)ap_rsne_len;
    s->nonce = nonce;
    s->nonce_arg = nonce_arg;
    s->state = WPA_EAPOL_WAIT_MSG1;
    return 0;
}

void wpa_eapol_clear(struct wpa_eapol *s)
{
    wipe(s, sizeof *s);
}

/* Walk the unwrapped key data (12.7.2: elements and KDEs, then 0xDD and zeros as padding). With want_rsne the first
 * RSNE has to be the AP's and there may be no second (this station only does the pairwise cipher it asked for).
 * The GTK KDE (00-0F-AC:1, figure 12-35) is required; its key goes to *gtk_at (it points into kd). */
static enum wpa_eapol_reason parse_kd(const struct wpa_eapol *s, const uint8_t *kd, size_t n, int want_rsne,
                                      const uint8_t **gtk_at, size_t *gtk_len, uint8_t *key_id)
{
    int rsnes = 0;
    *gtk_at = 0;
    for (size_t i = 0; i < n;) {
        if (kd[i] == 0xDD) {                    /* padding: 0xDD and only zeros after it */
            size_t z = i + 1;
            while (z < n && kd[z] == 0)
                z++;
            if (z == n)
                break;
        }
        if (n - i < 2 || (size_t)kd[i + 1] > n - i - 2)
            return WPA_EAPOL_E_GTK;
        const uint8_t *e = kd + i;
        size_t l = e[1];
        if (e[0] == 48) {
            if (rsnes++ || (want_rsne && (l + 2 != s->ap_rsne_len || differ(e, s->ap_rsne, l + 2))))
                return WPA_EAPOL_E_RSNE;
        } else if (e[0] == 0xDD && l >= 4 && e[2] == 0x00 && e[3] == 0x0F && e[4] == 0xAC && e[5] == 1) {
            if (*gtk_at || l < 6 + 5 || l > 6 + WPA_EAPOL_GTK_MAX)
                return WPA_EAPOL_E_GTK;      /* two GTKs, or one outside WEP-40's 5 to TKIP's 32 bytes */
            *key_id = e[6];
            *gtk_at = e + 8;
            *gtk_len = l - 6;
        }
        i += 2 + l;
    }
    if (want_rsne && !rsnes)
        return WPA_EAPOL_E_RSNE;
    return *gtk_at ? WPA_EAPOL_OK : WPA_EAPOL_E_GTK;
}

/* Take a GTK; flagged as new only when it differs from the one held, so a repeated msg 3 or group msg 1 does not
 * make the caller reload the same key and reset its receive counter. */
static void take_gtk(struct wpa_eapol *s, const uint8_t *g, size_t len, uint8_t key_id, const uint8_t *rsc)
{
    uint8_t idx = key_id & 3, tx = (key_id >> 2) & 1;
    if (len == s->gtk_len && idx == s->gtk_idx && !differ(g, s->gtk, len))
        return;
    wipe(s->gtk, sizeof s->gtk);
    memcpy(s->gtk, g, len);
    s->gtk_len = (uint8_t)len;
    s->gtk_idx = idx;
    s->gtk_tx = tx;
    memcpy(s->gtk_rsc, rsc, 8);
    s->new_keys |= WPA_EAPOL_NEW_GTK;
}

enum wpa_eapol_reason wpa_eapol_rx(struct wpa_eapol *s, const uint8_t *f, size_t len, uint8_t *reply,
                                   size_t cap, size_t *reply_len)
{
    enum { MSG1, MSG3, GROUP1 } msg;
    uint8_t plain[WPA_EAPOL_KEY_DATA_MAX - 8];
    const uint8_t *gtk, *kck;
    size_t gtk_len, kdl, flen;
    uint8_t key_id = 0;
    unsigned info;
    enum wpa_eapol_reason r;

    *reply_len = 0;
    s->new_keys = 0;
    if (s->state == WPA_EAPOL_FAILED)
        return WPA_EAPOL_E_STATE;               /* reason keeps why it failed */
    if (len < WPA_EAPOL_HDR)
        return s->reason = WPA_EAPOL_E_SHORT;
    if (f[1] != EAPOL_KEY)
        return s->reason = WPA_EAPOL_E_NOT_KEY;
    flen = WPA_EAPOL_HDR + be16(f + 2);
    if (flen < WPA_EAPOL_HDR + WPA_EAPOL_KEY_FIXED || len < flen)
        return s->reason = WPA_EAPOL_E_SHORT;
    if (f[O_DESC] != DESC_TYPE_80211)
        return s->reason = WPA_EAPOL_E_DESC_TYPE;
    info = be16(f + O_INFO);
    if ((info & KI_VER) != KEY_DESC_VER)
        return s->reason = WPA_EAPOL_E_VERSION;
    kdl = be16(f + O_KDLEN);
    if (O_KD + kdl > flen)
        return s->reason = WPA_EAPOL_E_SHORT;

    /* which message (12.7.6.2, 12.7.6.4, 12.7.7.2); the AP never sends a request, error or SMK message */
    if (info & (KI_ERROR | KI_REQUEST | KI_SMK) || !(info & KI_ACK))
        return s->reason = WPA_EAPOL_E_KEY_INFO;
    if ((info & (KI_PAIRWISE | KI_MIC | KI_INSTALL | KI_ENC)) == KI_PAIRWISE)
        msg = MSG1;
    else if ((info & (KI_PAIRWISE | KI_MIC | KI_INSTALL | KI_SECURE | KI_ENC)) ==
             (KI_PAIRWISE | KI_MIC | KI_INSTALL | KI_SECURE | KI_ENC))
        msg = MSG3;
    else if ((info & (KI_PAIRWISE | KI_MIC | KI_SECURE | KI_ENC)) == (KI_MIC | KI_SECURE | KI_ENC))
        msg = GROUP1;
    else
        return s->reason = WPA_EAPOL_E_KEY_INFO;

    if (msg == MSG1) {
        /* 12.7.6.2: a counter used before is discarded; a new SNonce and a new PTK (kept aside until msg 3) */
        if ((s->have_replay && !above(f + O_REPLAY, s->replay)) ||
            (s->have_msg1 && !above(f + O_REPLAY, s->msg1_replay)))
            return s->reason = WPA_EAPOL_E_REPLAY;
        if (cap < WPA_EAPOL_HDR + WPA_EAPOL_KEY_FIXED + (size_t)s->own_rsne_len)
            return s->reason = WPA_EAPOL_E_BUF;
        if (s->nonce(s->nonce_arg, s->snonce))
            return fail(s, WPA_EAPOL_E_RNG);
        memcpy(s->anonce, f + O_NONCE, 32);
        memcpy(s->msg1_replay, f + O_REPLAY, 8);
        s->have_msg1 = 1;
        wpa_ptk(s->pmk, s->aa, s->spa, s->anonce, s->snonce, s->tptk);
        *reply_len = reply_frame(reply, f[0], KEY_DESC_VER | KI_PAIRWISE | KI_MIC, s->msg1_replay, s->snonce,
                                 s->own_rsne, s->own_rsne_len, s->tptk);
        s->state = WPA_EAPOL_WAIT_MSG3;
        return s->reason = WPA_EAPOL_OK;
    }

    /* msg 3 and group msg 1: checks before the MIC drop the frame, failures after it end the association */
    if (msg == MSG3 ? !s->have_msg1 : s->state != WPA_EAPOL_DONE)
        return s->reason = WPA_EAPOL_E_STATE;
    if ((s->have_replay && !above(f + O_REPLAY, s->replay)) || (msg == MSG3 && !above(f + O_REPLAY, s->msg1_replay)))
        return s->reason = WPA_EAPOL_E_REPLAY;
    if (msg == MSG3 && be16(f + O_KEYLEN) != 16)
        return s->reason = WPA_EAPOL_E_KEY_LEN;
    if (msg == MSG3 && differ(f + O_NONCE, s->anonce, 32))
        return s->reason = WPA_EAPOL_E_NONCE;
    if (kdl % 8 || kdl < 24 || kdl > WPA_EAPOL_KEY_DATA_MAX)    /* RFC 3394: n >= 2 blocks of 8, plus 8 */
        return s->reason = WPA_EAPOL_E_KEY_DATA_LEN;
    if (cap < WPA_EAPOL_HDR + WPA_EAPOL_KEY_FIXED)
        return s->reason = WPA_EAPOL_E_BUF;
    kck = msg == MSG3 ? s->tptk : s->ptk;
    if (!mic_ok(kck, f, flen))
        return s->reason = WPA_EAPOL_E_MIC;
    memcpy(s->replay, f + O_REPLAY, 8);         /* 12.7.2: the counter moves only on a valid MIC */
    s->have_replay = 1;

    if (wpa_aes_unwrap(kck + 16, 16, kdl / 8 - 1, f + O_KD, plain))
        return fail(s, WPA_EAPOL_E_UNWRAP);
    r = parse_kd(s, plain, kdl - 8, msg == MSG3, &gtk, &gtk_len, &key_id);
    if (r != WPA_EAPOL_OK) {
        wipe(plain, sizeof plain);
        return fail(s, r);
    }
    if (msg == MSG3) {
        *reply_len = reply_frame(reply, f[0], KEY_DESC_VER | KI_PAIRWISE | KI_MIC | KI_SECURE, s->replay, 0, 0, 0,
                                 s->tptk);
        if (s->state == WPA_EAPOL_WAIT_MSG3) {  /* a repeated msg 3 once done is answered, the PTK not reloaded */
            memcpy(s->ptk, s->tptk, WPA_PTK_LEN);
            memcpy(s->tk, s->ptk + 32, 16);
            s->new_keys |= WPA_EAPOL_NEW_PTK;
        }
        s->state = WPA_EAPOL_DONE;
    } else {
        *reply_len = reply_frame(reply, f[0], KEY_DESC_VER | KI_MIC | KI_SECURE, s->replay, 0, 0, 0, s->ptk);
    }
    take_gtk(s, gtk, gtk_len, key_id, f + O_RSC);
    wipe(plain, sizeof plain);
    return s->reason = WPA_EAPOL_OK;
}
