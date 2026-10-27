/* SPDX-License-Identifier: GPL-3.0-only
 * Host register model of the FM-1's WiFi core, the fixture format and its diff (see sdio_model.h); fm1wifi's
 * port on the host. Own code, written from docs/WIFI.md. Host only.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "fm1wifi_port.h"
#include "fm1wifi_regs.h"
#include "sdio_model.h"
#include "wpa_ap.h"

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("sdio_model: out of memory\n", stderr); exit(2); }
    return q;
}

/* ---- the fixture format ---- */

static wifi_rec *fix_add(wifi_fix *f)
{
    if (f->n == f->cap) {
        f->cap = f->cap ? 2 * f->cap : 64;
        f->r = xrealloc(f->r, (size_t)f->cap * sizeof *f->r);
    }
    wifi_rec *r = &f->r[f->n++];
    memset(r, 0, sizeof *r);
    return r;
}

static void rec_byte(wifi_rec *r, uint8_t b, uint8_t any)
{
    if (r->n % 64 == 0) {
        r->data = xrealloc(r->data, r->n + 64);
        r->bany = xrealloc(r->bany, r->n + 64);
    }
    r->data[r->n] = b;
    r->bany[r->n++] = any;
}

void wifi_fix_free(wifi_fix *f)
{
    for (int i = 0; i < f->n; i++) { free(f->r[i].data); free(f->r[i].bany); }
    free(f->r);
    memset(f, 0, sizeof *f);
}

static char *tok(char **p)
{
    char *s = *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *p = s; return NULL; }
    char *t = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = 0;
    *p = s;
    return t;
}

static int hex_u32(const char *t, uint32_t *v)
{
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t += 2;
    size_t n = strlen(t);
    if (n == 0 || n > 8) return -1;
    uint32_t x = 0;
    for (; *t; t++) {
        if (!isxdigit((unsigned char)*t)) return -1;
        x = x << 4 | (uint32_t)(isdigit((unsigned char)*t) ? *t - '0' : (tolower((unsigned char)*t) - 'a' + 10));
    }
    *v = x;
    return 0;
}

static int dec_u32(const char *t, uint32_t *v)
{
    char *end;
    unsigned long x;
    if (!isdigit((unsigned char)*t)) return -1;
    x = strtoul(t, &end, 10);
    if (*end || x > 0xFFFFFFFFul) return -1;
    *v = (uint32_t)x;
    return 0;
}

static int hexdig(char c) { return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10; }

/* A frame's data token: hex pairs run together, "??" for any byte. */
static int data_tok(wifi_rec *r, const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || n % 2) return -1;
    for (size_t i = 0; i < n; i += 2) {
        if (t[i] == '?' && t[i + 1] == '?') { rec_byte(r, 0, 1); continue; }
        if (!isxdigit((unsigned char)t[i]) || !isxdigit((unsigned char)t[i + 1])) return -1;
        rec_byte(r, (uint8_t)(hexdig(t[i]) << 4 | hexdig(t[i + 1])), 0);
    }
    return 0;
}

static int perr(FILE *err, const char *name, int line, const char *what)
{
    if (err) fprintf(err, "%s:%d: %s\n", name, line, what);
    return -1;
}

/* "-> VALUE" or "-> ?" */
static const char *arrow(wifi_rec *r, char **p)
{
    char *t = tok(p);
    if (!t || strcmp(t, "->")) return "\"->\" and a value expected";
    if (!(t = tok(p))) return "a value expected after \"->\"";
    if (!strcmp(t, "?")) r->any = 1;
    else if (hex_u32(t, &r->val)) return "bad value";
    return tok(p) ? "nothing may follow the value" : NULL;
}

int wifi_fix_parse(wifi_fix *f, const char *text, const char *name, FILE *err)
{
    size_t len = strlen(text);
    char *buf = xrealloc(NULL, len + 1);
    memcpy(buf, text, len + 1);
    int line = 0, rc = 0, last = -1;
    const char *why = NULL;
    for (char *s = buf, *nl; s && *s && !rc; s = nl ? nl + 1 : NULL) {
        nl = strchr(s, '\n');
        if (nl) *nl = 0;
        line++;
        char *c = strchr(s, '#');
        if (c) *c = 0;
        size_t sl = strlen(s);
        if (sl && s[sl - 1] == '\r') s[--sl] = 0;
        char *p = s, *t;
        int cont = s[0] == ' ' || s[0] == '\t';
        if (!s[strspn(s, " \t")]) continue;
        if (cont) {
            if (last < 0 || f->r[last].kind != WIFI_REC_FRAME) { rc = perr(err, name, line, "a continuation line follows no frame record"); break; }
            while ((t = tok(&p)))
                if (data_tok(&f->r[last], t)) { rc = perr(err, name, line, "bad data byte"); break; }
            continue;
        }
        t = tok(&p);
        wifi_rec *r = fix_add(f);
        r->line = line;
        last = f->n - 1;
        if (!strcmp(t, "frame")) {
            r->kind = WIFI_REC_FRAME;
            if (!(t = tok(&p)) || (strcmp(t, "rx") && strcmp(t, "tx"))) { rc = perr(err, name, line, "frame needs rx or tx"); break; }
            r->write = !strcmp(t, "tx");
            while (*p == ' ' || *p == '\t') p++;
            char *q;
            while (*p && (q = strchr(p, '=')) && (!strpbrk(p, " \t") || q < strpbrk(p, " \t"))) {
                char *a = tok(&p);
                if (strlen(r->attr) + strlen(a) + 2 > sizeof r->attr) { rc = perr(err, name, line, "frame attributes too long"); break; }
                if (r->attr[0]) strcat(r->attr, " ");
                strcat(r->attr, a);
                while (*p == ' ' || *p == '\t') p++;
            }
            while (!rc && (t = tok(&p)))
                if (data_tok(r, t)) rc = perr(err, name, line, "bad data byte");
            continue;
        }
        if (!strcmp(t, "delay")) {
            r->kind = WIFI_REC_DELAY;
            if (!(t = tok(&p)) || dec_u32(t, &r->val) || tok(&p)) rc = perr(err, name, line, "delay needs microseconds (decimal)");
            continue;
        }
        if (!strcmp(t, "bbp")) {
            r->kind = WIFI_REC_BBP;
            if (!(t = tok(&p)) || hex_u32(t, &r->addr) || r->addr > 0xFF || !(t = tok(&p)) || hex_u32(t, &r->val) ||
                r->val > 0xFF || tok(&p))
                rc = perr(err, name, line, "bbp needs a register and a value, one byte each");
            continue;
        }
        if (!strcmp(t, "win")) {
            static const char *const W[4] = { "cca", "rx11n", "txbe", "rxfe" };
            uint32_t w = 4, a;
            r->kind = WIFI_REC_WIN;
            if ((t = tok(&p)))
                for (w = 0; w < 4u && strcmp(t, W[w]); w++)
                    ;
            if (w > 3u || !(t = tok(&p)) || hex_u32(t, &a) || a > 0xFF || !(t = tok(&p)) || hex_u32(t, &r->val) ||
                r->val > 0xFF || tok(&p)) {
                rc = perr(err, name, line, "win needs cca, rx11n, txbe or rxfe, an address and a value");
                break;
            }
            r->addr = w << 8 | a;
            continue;
        }
        if (!strcmp(t, "wr32") || !strcmp(t, "rd32") || !strcmp(t, "clk")) {
            r->kind = t[0] == 'w' ? WIFI_REC_WR : t[0] == 'r' ? WIFI_REC_RD : WIFI_REC_CLK;
            if (!(t = tok(&p)) || hex_u32(t, &r->addr)) { rc = perr(err, name, line, "an address (hex) expected"); break; }
            if (r->kind == WIFI_REC_WR) {
                if (!(t = tok(&p)) || hex_u32(t, &r->val) || tok(&p)) rc = perr(err, name, line, "one value (hex) expected");
            } else if ((why = arrow(r, &p))) rc = perr(err, name, line, why);
            continue;
        }
        rc = perr(err, name, line, "unknown record");
    }
    if (!rc)
        for (int i = 0; i < f->n; i++)
            if (f->r[i].kind == WIFI_REC_FRAME && f->r[i].n == 0) { rc = perr(err, name, f->r[i].line, "frame without data"); break; }
    free(buf);
    return rc;
}

static char *read_file(const char *path, FILE *err)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { if (err) fprintf(err, "%s: cannot open\n", path); return NULL; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    rewind(fp);
    char *s = xrealloc(NULL, (size_t)(n < 0 ? 0 : n) + 1);
    size_t got = fread(s, 1, (size_t)(n < 0 ? 0 : n), fp);
    s[got] = 0;
    fclose(fp);
    if (strlen(s) != got) { if (err) fprintf(err, "%s: NUL byte in a text file\n", path); free(s); return NULL; }
    return s;
}

int wifi_fix_load(wifi_fix *f, const char *path, FILE *err)
{
    char *s = read_file(path, err);
    if (!s) return -1;
    int rc = wifi_fix_parse(f, s, path, err);
    free(s);
    return rc;
}

void wifi_rec_write(FILE *o, const wifi_rec *r, int one_line)
{
    switch (r->kind) {
    case WIFI_REC_WR: fprintf(o, "wr32 0x%05x 0x%08x", (unsigned)r->addr, (unsigned)r->val); break;
    case WIFI_REC_RD:
    case WIFI_REC_CLK:
        fprintf(o, r->kind == WIFI_REC_RD ? "rd32 0x%05x" : "clk 0x%04x", (unsigned)r->addr);
        if (r->any) fputs(" -> ?", o);
        else fprintf(o, r->kind == WIFI_REC_RD ? " -> 0x%08x" : " -> 0x%04x", (unsigned)r->val);
        break;
    case WIFI_REC_DELAY: fprintf(o, "delay %u", (unsigned)r->val); break;
    case WIFI_REC_BBP: fprintf(o, "bbp 0x%02x 0x%02x", (unsigned)r->addr, (unsigned)r->val); break;
    case WIFI_REC_WIN: {
        static const char *const W[4] = { "cca", "rx11n", "txbe", "rxfe" };
        fprintf(o, "win %s 0x%02x 0x%02x", W[(r->addr >> 8) & 3u], (unsigned)(r->addr & 0xFFu), (unsigned)r->val);
        break;
    }
    default:
        fprintf(o, "frame %s", r->write ? "tx" : "rx");
        if (r->attr[0]) fprintf(o, " %s", r->attr);
        for (unsigned i = 0; i < r->n; i++) {
            if (one_line && i == 16) { fprintf(o, " .. (%u bytes)", r->n); break; }
            if (!one_line && i && i % 16 == 0) fputs("\n ", o);
            if (r->bany && r->bany[i]) fputs(" ??", o);
            else fprintf(o, " %02x", r->data[i]);
        }
    }
    if (!one_line) fputc('\n', o);
}

void wifi_fix_write(FILE *out, const wifi_fix *f)
{
    for (int i = 0; i < f->n; i++) wifi_rec_write(out, &f->r[i], 0);
}

int wifi_rec_match(const wifi_rec *w, const wifi_rec *g)
{
    if (w->kind != g->kind) return 0;
    switch (w->kind) {
    case WIFI_REC_WR:
    case WIFI_REC_BBP:
    case WIFI_REC_WIN: return w->addr == g->addr && w->val == g->val;
    case WIFI_REC_RD:
    case WIFI_REC_CLK: return w->addr == g->addr && (w->any || w->val == g->val);
    case WIFI_REC_DELAY: return w->val == g->val;
    default:
        if (w->write != g->write || strcmp(w->attr, g->attr) || w->n != g->n) return 0;
        for (unsigned i = 0; i < w->n; i++)
            if (!(w->bany && w->bany[i]) && w->data[i] != g->data[i]) return 0;
        return 1;
    }
}

#define DIFF_WINDOW 32   /* how far ahead a missing or extra run of records is looked for */
#define DIFF_SHOWN 20

static void where(FILE *o, const char *who, const wifi_rec *r, int i)
{
    if (r->line) fprintf(o, "  %s line %d: ", who, r->line);
    else fprintf(o, "  %s #%d: ", who, i + 1);
    wifi_rec_write(o, r, 1);
    fputc('\n', o);
}

int wifi_fix_diff(FILE *out, const wifi_fix *want, const wifi_fix *got)
{
    int i = 0, j = 0, nd = 0;
    while (i < want->n || j < got->n) {
        if (i < want->n && j < got->n && wifi_rec_match(&want->r[i], &got->r[j])) { i++; j++; continue; }
        int show = out && nd < DIFF_SHOWN, k, done = 0;
        nd++;
        for (k = 1; k <= DIFF_WINDOW && !done; k++) {
            if (j < got->n && i + k < want->n && wifi_rec_match(&want->r[i + k], &got->r[j])) {
                if (show) fprintf(out, "missing from the run:\n");
                for (int x = 0; show && x < k; x++) where(out, "want", &want->r[i + x], i + x);
                i += k;
                done = 1;
            } else if (i < want->n && j + k < got->n && wifi_rec_match(&want->r[i], &got->r[j + k])) {
                if (show) fprintf(out, "extra in the run:\n");
                for (int x = 0; show && x < k; x++) where(out, "got", &got->r[j + x], j + x);
                j += k;
                done = 1;
            }
        }
        if (done) continue;
        if (i < want->n && j < got->n) {
            if (show) { fprintf(out, "different:\n"); where(out, "want", &want->r[i], i); where(out, "got ", &got->r[j], j); }
            i++;
            j++;
        } else if (i < want->n) {
            if (show) { fprintf(out, "missing from the run:\n"); where(out, "want", &want->r[i], i); }
            i++;
        } else {
            if (show) { fprintf(out, "extra in the run:\n"); where(out, "got", &got->r[j], j); }
            j++;
        }
    }
    if (out && nd > DIFF_SHOWN) fprintf(out, "... %d differences in all\n", nd);
    return nd;
}

/* ---- the model ---- */

typedef struct { uint32_t addr; uint32_t *v; unsigned n, pos; int stuck; } mqueue;
#define AP_MAX 8
typedef struct {
    uint8_t bssid[6], ch, sec, hidden;
    char ssid[33];
    int8_t rssi;
    int auth, assoc;            /* how it answers: 0 accept, 1 refuse, 2 silent */
    int kick_ms, gone_ms;       /* after association: a deauthentication / no more beacons, that many ms later (0: never) */
    int authed, joined;
    uint64_t next_beacon, joined_at;
    uint16_t seq;
    uint8_t sta[6];
    /* WPA2-PSK (psk=...): the authenticator, how it behaves, where it is (sdio_model.h "ap") */
    uint8_t rsn[64], rsn_len;   /* its RSN element (the default CCMP / CCMP / PSK, or rsn=HEX) */
    int psk, eapol, rekey_ms, bcast_ms;   /* eapol: 0 normal, 1 silent, 2 msg 3 with a bad MIC */
    uint8_t pmk[32];
    struct wpa_auth wa;
    int tries, done, rekeyed;
    uint64_t sent_at, done_at, next_bcast;
    uint32_t refused;           /* station frames the AP dropped: bad MIC, wrong counter, not encrypted as it must be */
    uint8_t bc_gtk[16], bc_idx; /* the GTK its group addressed frames go out under (the new one once group msg 2 came) */
    int group_done;             /* group key handshakes finished (group msg 2 taken) */
    /* stage 9 (net=IP, sdio_model.h "ap ... net="): a router behind the AP, DHCP server and gateway at gw, and a host
     * that pings the station */
    int net, dhcp, noping, pingin_ms, ping_delay_ms;   /* dhcp: 0 normal, 1 silent, 2 NAK every REQUEST */
    int clear_ms, ivleft;       /* an unprotected broadcast every clear_ms; decrypted frames keep the CCMP header */
    int others, dup, arpdrop;   /* an OFFER for another client first; each echo reply twice; the first ARP request ignored */
    uint64_t next_clear;
    uint8_t gw[4], lease_ip[4];
    uint32_t gw_mac_seq;
    int host_arp, acked;
    uint64_t next_pingin, acked_at;
    uint16_t pingin_seq;
    sdio_model_net ns;          /* what it saw (sdio_model_ap_net) */
    struct { uint64_t at; uint8_t p[128]; unsigned n; } held[4];   /* echo replies delayed by ping_delay_ms */
} model_ap;
enum { POLL_TUNE, POLL_MEASURE, POLL_ANL, POLL_STUCK, N_POLL };

struct sdio_model {
    uint32_t ana[FM1_WLA_CON_N], wl[40], core[(FM1_WIFI_CORE_LAST - FM1_WIFI_CORE_BASE) / 4u + 1u];
    mqueue *q;
    int nq;
    uint8_t bbp[256], win[4][256];
    uint32_t clk;
    int bad, raw_off, trims, in_trim;
    int hang_tune, hang_measure, hang_anl, hang_after, measures;
    unsigned poll[N_POLL], poll_max;
    wifi_fix log, wlog;
    void (*on_wr)(void *ctx, uint32_t addr, uint32_t v);
    void *on_wr_ctx;
    uint64_t us_cca, us_busy;   /* microseconds of delay since RX_STA_CNT1 / CH_BUSY_STA were last read */
    /* stages 4-6: the frame FIFOs and the access points around (sdio_model.h "ap") */
    uint64_t now_us;            /* the model's clock: the library's delays added up */
    model_ap ap[AP_MAX];
    int nap;
    uint8_t *fifo;              /* the RX FIFO: frames in the core's format, back to back */
    size_t fifo_n, fifo_cap;
    int rx_done, tx_done, hang_rxdma, hang_txdma, rx_bad;
    uint32_t tx_ok, rx_dropped;
    void *bus[16];              /* fm1wifi_port_bus's buffers: number k is 0x10000000 + k << 20 */
    int nbus;
    wifi_fix frames;            /* every frame sent (tx) and queued for the library (rx), with its channel */
};

static sdio_model *cur;
static uint32_t sys_base, sys_last;   /* sys_now (below) */
static uint64_t sys_from_us;
static void rx_dma(sdio_model *m, uint32_t len);
static void tx_dma(sdio_model *m, uint32_t len);
static void air(sdio_model *m);

sdio_model *sdio_model_new(void)
{
    sdio_model *m = xrealloc(NULL, sizeof *m);
    memset(m, 0, sizeof *m);
    return m;
}

void sdio_model_free(sdio_model *m)
{
    if (!m) return;
    if (cur == m) cur = NULL;
    for (int i = 0; i < m->nq; i++) free(m->q[i].v);
    free(m->q);
    wifi_fix_free(&m->log);
    wifi_fix_free(&m->wlog);
    wifi_fix_free(&m->frames);
    free(m->fifo);
    free(m);
}

void sdio_model_use(sdio_model *m) { sys_now(); cur = m; sys_base = sys_last; sys_from_us = m ? m->now_us : 0; }
void sdio_model_on_wr(sdio_model *m, void (*fn)(void *ctx, uint32_t addr, uint32_t v), void *ctx) { m->on_wr = fn; m->on_wr_ctx = ctx; }
const wifi_fix *sdio_model_log(const sdio_model *m) { return &m->log; }
const wifi_fix *sdio_model_wlog(const sdio_model *m) { return &m->wlog; }
void sdio_model_wlog_clear(sdio_model *m) { wifi_fix_free(&m->wlog); }
void sdio_model_raw_log(sdio_model *m, int on) { m->raw_off = !on; }
void sdio_model_zero(sdio_model *m)
{
    memset(m->ana, 0, sizeof m->ana);
    memset(m->wl, 0, sizeof m->wl);
    memset(m->core, 0, sizeof m->core);
    memset(m->bbp, 0, sizeof m->bbp);
    memset(m->win, 0, sizeof m->win);
}
uint32_t sdio_model_clk(const sdio_model *m) { return m->clk; }
const wifi_fix *sdio_model_frames(const sdio_model *m) { return &m->frames; }
uint64_t sdio_model_now_us(const sdio_model *m) { return m->now_us; }
/* lwIP's clock (fm1wifi_port.h) on the host: the model's, in ms, carried on from the last model's when another one is
 * put in use (a new model's clock starts at 0, lwIP's timers must never see time go back) */
uint32_t sys_now(void)
{
    if (cur) sys_last = sys_base + (uint32_t)((cur->now_us - sys_from_us) / 1000u);
    return sys_last;
}
static int ap_line(sdio_model *m, char *p);
uint8_t sdio_model_bbp(const sdio_model *m, uint8_t reg) { return m->bbp[reg]; }
uint8_t sdio_model_win(const sdio_model *m, unsigned w, uint8_t a) { return m->win[w & 3u][a]; }
int sdio_model_bad(const sdio_model *m) { return m->bad; }
int sdio_model_trims(const sdio_model *m) { return m->trims; }
int sdio_model_in_trim(const sdio_model *m) { return m->in_trim; }
unsigned sdio_model_polls(const sdio_model *m) { return m->poll_max; }

static int wifi_addr(uint32_t a)   /* fm1wifi_port.h's blocks, as the felucca-bsp port has them */
{
    return !(a & 3u) && ((a >= FM1_WLA_CON(0) && a <= FM1_WLA_CON(FM1_WLA_CON_N - 1u)) ||
                         (a >= FM1_JL_WL_BASE && a <= FM1_WL_SD_DPTR) ||
                         (a >= FM1_WIFI_CORE_BASE && a <= FM1_WIFI_CORE_LAST));
}

static uint32_t *reg_at(sdio_model *m, uint32_t a)   /* the register file: NULL outside the WiFi blocks */
{
    if (!wifi_addr(a)) return NULL;
    if (a < FM1_JL_WL_BASE) return &m->ana[(a - FM1_WLA_CON(0)) / 4u];
    if (a < FM1_WIFI_CORE_BASE) return &m->wl[(a - FM1_JL_WL_BASE) / 4u];
    return &m->core[(a - FM1_WIFI_CORE_BASE) / 4u];
}

uint32_t sdio_model_peek(const sdio_model *m, uint32_t addr)
{
    uint32_t *r = reg_at((sdio_model *)m, addr);
    return r ? *r : 0;
}

static mqueue *queue_at(sdio_model *m, uint32_t addr, int make)
{
    for (int i = 0; i < m->nq; i++)
        if (m->q[i].addr == addr) return &m->q[i];
    if (!make) return NULL;
    m->q = xrealloc(m->q, (size_t)(m->nq + 1) * sizeof *m->q);
    mqueue *q = &m->q[m->nq++];
    memset(q, 0, sizeof *q);
    q->addr = addr;
    return q;
}

/* The script: reg, next, stuck, bbp, clk, hang (sdio_model.h). */
int sdio_model_script(sdio_model *m, const char *text, const char *name, FILE *err)
{
    size_t len = strlen(text);
    char *buf = xrealloc(NULL, len + 1);
    memcpy(buf, text, len + 1);
    int line = 0, rc = 0;
    for (char *s = buf, *nl; s && *s && !rc; s = nl ? nl + 1 : NULL) {
        nl = strchr(s, '\n');
        if (nl) *nl = 0;
        line++;
        char *c = strchr(s, '#');
        if (c) *c = 0;
        char *p = s, *t;
        uint32_t a, v;
        if (!s[strspn(s, " \t")]) continue;
        t = tok(&p);
        if (!strcmp(t, "clk")) {
            if (!(t = tok(&p)) || hex_u32(t, &v) || (v & ~FM1_CLK_CON1_WIFI) || tok(&p)) { rc = perr(err, name, line, "clk needs bits 15-14 only"); break; }
            m->clk = v;
            continue;
        }
        if (!strcmp(t, "bbp")) {
            if (!(t = tok(&p)) || hex_u32(t, &a) || a > 0xFF || !(t = tok(&p)) || hex_u32(t, &v) || v > 0xFF || tok(&p)) {
                rc = perr(err, name, line, "bbp needs a register and a value, one byte each");
                break;
            }
            m->bbp[a] = (uint8_t)v;
            continue;
        }
        if (!strcmp(t, "hang")) {
            t = tok(&p);
            if (t && !strcmp(t, "tune")) m->hang_tune = 1;
            else if (t && !strcmp(t, "measure")) {
                m->hang_measure = 1;
                t = tok(&p);
                if (t && !strcmp(t, "after")) {        /* hang measure after N: the first N measurements answer */
                    t = tok(&p);
                    if (!t) { rc = perr(err, name, line, "a number after hang measure after"); break; }
                    m->hang_after = atoi(t);
                } else if (t) { rc = perr(err, name, line, "hang measure, or hang measure after N"); break; }
                continue;
            }
            else if (t && !strcmp(t, "anl")) m->hang_anl = 1;
            else if (t && !strcmp(t, "rxdma")) m->hang_rxdma = 1;
            else if (t && !strcmp(t, "txdma")) m->hang_txdma = 1;
            else { rc = perr(err, name, line, "hang needs tune, measure, anl, rxdma or txdma"); break; }
            if (tok(&p)) { rc = perr(err, name, line, "one word after hang"); break; }
            continue;
        }
        if (!strcmp(t, "ap")) {
            if (m->nap >= AP_MAX || ap_line(m, p)) { rc = perr(err, name, line, "ap: ch=1-13 bssid=xx:..:xx ssid=NAME [sec=open|wep|wpa|wpa2] [rssi=-N] [hidden] [auth=accept|refuse|silent] [assoc=...] [kick=MS] [gone=MS] [psk=PASS (wpa2) eapol=normal|silent|badmic rekey=MS bcast=MS rsn=HEX [net=A.B.C.D dhcp=normal|silent|nak ping=on|off pingin=MS delay=MS]], 8 at most"); break; }
            continue;
        }
        if (!strcmp(t, "rxbad")) { m->rx_bad = 1; continue; }   /* the first frame's length word reads 0 */
        if (strcmp(t, "reg") && strcmp(t, "next") && strcmp(t, "stuck")) { rc = perr(err, name, line, "unknown script line"); break; }
        int kind = t[0] == 'r' ? 0 : t[0] == 'n' ? 1 : 2;
        if (!(t = tok(&p)) || hex_u32(t, &a) || (a & 3u)) { rc = perr(err, name, line, "a word-aligned address expected"); break; }
        if (!kind && !reg_at(m, a)) { rc = perr(err, name, line, "reg: an address outside the WiFi blocks"); break; }
        mqueue *q = kind ? queue_at(m, a, 1) : NULL;
        int n = 0;
        while ((t = tok(&p))) {
            if (hex_u32(t, &v)) { rc = perr(err, name, line, "bad value"); break; }
            n++;
            if (!kind) { *reg_at(m, a) = v; continue; }
            if (kind == 2) { q->stuck = 1; q->n = q->pos = 0; }
            q->v = xrealloc(q->v, (q->n + 1) * sizeof *q->v);
            q->v[q->n++] = v;
        }
        if (!rc && (n == 0 || ((kind == 0 || kind == 2) && n != 1))) rc = perr(err, name, line, kind == 1 ? "next needs values" : "one value expected");
    }
    free(buf);
    return rc;
}

int sdio_model_load(sdio_model *m, const char *path, FILE *err)
{
    char *s = read_file(path, err);
    if (!s) return -1;
    int rc = sdio_model_script(m, s, path, err);
    free(s);
    return rc;
}

static sdio_model *model(void)
{
    if (!cur) { fputs("sdio_model: a port call with no model in use (sdio_model_use)\n", stderr); exit(2); }
    return cur;
}

static void log_rec(sdio_model *m, int kind, uint32_t addr, uint32_t val)
{
    if (m->raw_off) return;
    wifi_rec *r = fix_add(&m->log);
    r->kind = kind;
    r->addr = addr;
    r->val = val;
}

static void wlog_rec(sdio_model *m, int kind, uint32_t addr, uint32_t val)
{
    wifi_rec *r = fix_add(&m->wlog);
    r->kind = kind;
    r->addr = addr;
    r->val = val;
}

static void poll(sdio_model *m, int kind, int busy)   /* a wait's answer: count busy runs, stop an endless one */
{
    if (!busy) { m->poll[kind] = 0; return; }
    if (++m->poll[kind] > m->poll_max) m->poll_max = m->poll[kind];
    if (m->poll[kind] > SDIO_MODEL_POLL_MAX) {
        fprintf(stderr, "sdio_model: unbounded wait: %u busy answers in a row\n", m->poll[kind]);
        exit(3);
    }
}

/* ---- stage 3's answers (sdio_model.h; tools/wifi_ir.py's Model has the same) */

static uint32_t wla(const sdio_model *m, unsigned n) { return m->ana[n]; }
static int32_t sx(uint32_t v, unsigned bits) { v &= (1u << bits) - 1u; return v >> (bits - 1u) ? (int32_t)v - (1 << bits) : (int32_t)v; }

static uint32_t status(sdio_model *m)          /* WLA_CON30, selector WLA_CON26 bits 31-28 */
{
    uint32_t w26 = wla(m, 26), v, cap, fbds, lo, res;
    if (w26 >> 28 != 1u) return 0;
    v = m->hang_tune ? 0 : (w26 & 1u) << 5;
    poll(m, POLL_TUNE, (w26 & 1u) && !v);
    v |= ((((w26 >> 1) & 0x1FFu) * 5u / 9u) & 0xFFu) << 8;
    cap = (wla(m, 14) >> 19) & 0x7Fu;
    fbds = (wla(m, 15) >> 5) & 0xFFu;
    lo = 250u - 2u * cap;
    res = fbds < lo ? 2u : fbds < lo + 5u ? 0u : 1u;
    return v | (res & 1u) << 18 | (res >> 1) << 17;
}

static void measure(const sdio_model *m, int32_t *i, int32_t *q)   /* the measurement registers, 24-bit units */
{
    const uint8_t *t = m->win[2], *r = m->win[3];
    if (wla(m, 11) >> 14 & 1u) {                              /* the TX calibration mode */
        int32_t lo_i = (int32_t)(wla(m, 7) >> 8 & 0x7Fu), lo_q = (int32_t)(wla(m, 8) & 0x7Fu);
        int32_t di = sx((uint32_t)t[8] << 2 | (t[98] & 3u), 10), dq = sx((uint32_t)t[9] << 2 | (t[98] >> 2 & 3u), 10);
        int32_t gi = (int32_t)((uint32_t)t[4] << 2 | (t[97] & 3u)), gq = (int32_t)((uint32_t)t[6] << 2 | (t[97] >> 4 & 3u));
        int32_t pi = sx((uint32_t)t[5] << 2 | (t[97] >> 2 & 3u), 10), pq = sx((uint32_t)t[7] << 2 | (t[97] >> 6 & 3u), 10);
        *i = 256 * (40 * (lo_i - 41) + 24 * (di - 23) + 2 * (gi - gq - 37));
        *q = 256 * (40 * (lo_q - 52) + 24 * (dq + 17) + 3 * (pq - pi + 9));
    } else if (wla(m, 11) >> 11 & 1u) {                       /* the RX calibration mode */
        *i = 256 * (5 * ((int32_t)r[9] - (int32_t)r[11] + 20));
        *q = 256 * (4 * (sx(r[10], 8) - sx(r[12], 8) - 7));
    } else {                                                  /* the analog DC against its range's target */
        uint32_t w2 = wla(m, 2);
        int32_t di = (int32_t)(w2 >> 7 & 0x1FFu), dq = (int32_t)(w2 >> 17 & 0x1FFu);
        int32_t rng = (int32_t)(wla(m, 4) >> 8 & 0xFu), g = (int32_t)(wla(m, 3) >> 6 & 3u);
        if (!(w2 >> 16 & 1u)) di = -di;
        if (!(w2 >> 26 & 1u)) dq = -dq;
        *i = 600 * (di - (123 - 9 * rng - 20 * g));
        *q = 600 * (dq - (-77 + 5 * rng + 11 * g));
    }
}

/* The ADC peak, a made-up level from the VGA fields and the channel, but only with the PHY in its loopback (BBP 25 =
 * 0x18: the trim, where adc_pp_tune reads it); in the receive state 0, as the FM-1 read on every channel on its first
 * clean run (docs/WIFI_STAGE3.md "ADC peak 0 on every channel"). Which of the two the hardware does is not known. */
static uint8_t adc_peak(const sdio_model *m)
{
    if (m->bbp[25] != 0x18u) return 0;
    uint32_t w4 = wla(m, 4), w5 = wla(m, 5), ch = m->wl[(FM1_WF_CON(7) - FM1_JL_WL_BASE) / 4u] & 0xFFu;
    uint32_t g = (w5 >> 15 & 7u) + 8u * (w5 >> 5 & 7u) + 2u * (w5 >> 12 & 7u) + (w4 >> 22 & 7u) + 3u * (w4 >> 25 & 3u);
    uint32_t p = 6u + 3u * g + (ch * 7u) % 11u;
    return (uint8_t)(p > 255u ? 255u : p);
}

/* The channel survey's counters (fm1wifi_channel_survey): made-up access points on channels 1, 6 and 11 (weights 4, 10
 * and 3, each reaching two channels either side, less with distance) give, per millisecond of delay with the MAC's
 * receiver on (MAC_SYS_CTRL bit 3), 2 + the sum of weight * (3 - distance) false CCAs and 20 + 10 times that sum per
 * mille of busy time. Both clear when read, as rt2800's do. None of it is measured. */
static uint32_t survey_level(const sdio_model *m)
{
    static const uint32_t AP[3][2] = { { 1, 4 }, { 6, 10 }, { 11, 3 } };
    uint32_t ch = (m->wl[(FM1_WF_CON(7) - FM1_JL_WL_BASE) / 4u] & 0xFFu) - 101u, sum = 0, i;
    for (i = 0; i < 3u; i++) {
        uint32_t d = ch > AP[i][0] ? ch - AP[i][0] : AP[i][0] - ch;
        if (d < 3u) sum += AP[i][1] * (3u - d);
    }
    return sum;
}

static uint32_t survey_read(sdio_model *m, uint32_t addr)
{
    int rx = m->core[MAC_SYS_CTRL / 4u] & MAC_SYS_CTRL_ENABLE_RX;
    uint64_t v;
    if (addr == RX_STA_CNT1) {
        v = rx ? (2u + survey_level(m)) * m->us_cca / 1000u : 0u;
        m->us_cca = 0;
        return v > 0xFFFFu ? 0xFFFFu : (uint32_t)v;
    }
    v = rx ? (20u + 10u * survey_level(m)) * m->us_busy / 1000u : 0u;
    m->us_busy = 0;
    return (uint32_t)v;
}

static uint8_t win_read(sdio_model *m, unsigned w, uint8_t a)
{
    if (w == 3u) {
        if (a == 0x8D) {
            int busy = m->hang_measure && m->measures >= m->hang_after;
            poll(m, POLL_MEASURE, busy);
            if (!busy) m->measures++;
            return busy ? 3 : 0;
        }
        if (a == 0x80) return adc_peak(m);
        if (a >= 0x81 && a <= 0x8C) {
            int32_t i, q, v;
            unsigned k = (a - 0x81u) / 3u;
            measure(m, &i, &q);
            v = k == 0u ? i : k == 2u ? 0 : q;
            return (uint8_t)(((uint32_t)v & 0xFFFFFFu) >> (8u * (2u - (a - 0x81u) % 3u)));
        }
    }
    return m->win[w][a];
}

static void bbp_write(sdio_model *m, uint8_t r, uint8_t v)
{
    m->bbp[r] = v;
    if (r >= FM1_BBP_CCA_STROBE && r <= FM1_BBP_RXFE_STROBE + 3u) {
        unsigned w = (r - FM1_BBP_CCA_STROBE) / 4u, k = (r - FM1_BBP_CCA_STROBE) % 4u;
        uint8_t s = (uint8_t)(FM1_BBP_CCA_STROBE + 4u * w), a = m->bbp[s + 1u];
        if (k == 1u || k == 2u) return;                      /* the window's address and data registers */
        if (k == 0u && v == 1u) {
            m->win[w][a] = m->bbp[s + 2u];
            wlog_rec(m, WIFI_REC_WIN, w << 8 | a, m->win[w][a]);
            return;
        }
        if (k == 0u && v == 2u) { m->bbp[s + 3u] = win_read(m, w, a); return; }
    }
    wlog_rec(m, WIFI_REC_BBP, r, v);
}

/* ---- stages 4-6: the frame FIFOs and the access points (sdio_model.h "ap") */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32le(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static void put32be(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32le(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int hex_mac(const char *t, uint8_t *a)
{
    unsigned v[6];
    if (sscanf(t, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) { if (v[i] > 255u) return -1; a[i] = (uint8_t)v[i]; }
    return 0;
}

static int answer(const char *v)
{
    return !strcmp(v, "accept") ? 0 : !strcmp(v, "refuse") ? 1 : !strcmp(v, "silent") ? 2 : -1;
}

static int ap_line(sdio_model *m, char *p)
{
    model_ap *a = &m->ap[m->nap];
    char *t, pass[64] = "";
    int have = 0;
    memset(a, 0, sizeof *a);
    a->rssi = -50;
    while ((t = tok(&p))) {
        char *v = strchr(t, '=');
        if (v) *v++ = 0;
        if (!strcmp(t, "hidden") && !v) { a->hidden = 1; continue; }
        if (!v) return -1;
        if (!strcmp(t, "ch")) { a->ch = (uint8_t)atoi(v); if (a->ch < 1 || a->ch > 13) return -1; have |= 1; }
        else if (!strcmp(t, "bssid")) { if (hex_mac(v, a->bssid)) return -1; have |= 2; }
        else if (!strcmp(t, "ssid")) { if (strlen(v) > 32) return -1; strcpy(a->ssid, v); have |= 4; }
        else if (!strcmp(t, "sec")) {
            a->sec = !strcmp(v, "open") ? 0 : !strcmp(v, "wep") ? 1 : !strcmp(v, "wpa") ? 2 : !strcmp(v, "wpa2") ? 3 : 9;
            if (a->sec > 3) return -1;
        }
        else if (!strcmp(t, "rssi")) a->rssi = (int8_t)atoi(v);
        else if (!strcmp(t, "auth")) { if ((a->auth = answer(v)) < 0) return -1; }
        else if (!strcmp(t, "assoc")) { if ((a->assoc = answer(v)) < 0) return -1; }
        else if (!strcmp(t, "kick")) a->kick_ms = atoi(v);
        else if (!strcmp(t, "psk")) { if (strlen(v) > 63) return -1; strcpy(pass, v); a->psk = 1; }
        else if (!strcmp(t, "eapol")) { a->eapol = !strcmp(v, "silent") ? 1 : !strcmp(v, "badmic") ? 2 : !strcmp(v, "normal") ? 0 : -1; if (a->eapol < 0) return -1; }
        else if (!strcmp(t, "rekey")) a->rekey_ms = atoi(v);
        else if (!strcmp(t, "bcast")) a->bcast_ms = atoi(v);
        else if (!strcmp(t, "rsn")) {
            unsigned b, k = 0;
            while (v[2 * k] && k < sizeof a->rsn && sscanf(v + 2 * k, "%2x", &b) == 1) a->rsn[k++] = (uint8_t)b;
            if (k < 4 || a->rsn[0] != 48 || a->rsn[1] + 2u != k) return -1;
            a->rsn_len = (uint8_t)k;
        }
        else if (!strcmp(t, "gone")) a->gone_ms = atoi(v);
        else if (!strcmp(t, "net")) {
            unsigned q[4];
            if (sscanf(v, "%u.%u.%u.%u", &q[0], &q[1], &q[2], &q[3]) != 4) return -1;
            for (int k = 0; k < 4; k++) { if (q[k] > 255) return -1; a->gw[k] = (uint8_t)q[k]; }
            memcpy(a->lease_ip, a->gw, 3); a->lease_ip[3] = 23;
            a->net = 1;
        }
        else if (!strcmp(t, "dhcp")) { a->dhcp = !strcmp(v, "normal") ? 0 : !strcmp(v, "silent") ? 1 : !strcmp(v, "nak") ? 2 : -1; if (a->dhcp < 0) return -1; }
        else if (!strcmp(t, "ping")) { a->noping = !strcmp(v, "off") ? 1 : !strcmp(v, "on") ? 0 : -1; if (a->noping < 0) return -1; }
        else if (!strcmp(t, "pingin")) a->pingin_ms = atoi(v);
        else if (!strcmp(t, "delay")) a->ping_delay_ms = atoi(v);
        else if (!strcmp(t, "clear")) a->clear_ms = atoi(v);
        else if (!strcmp(t, "ivleft")) { a->ivleft = !strcmp(v, "1"); }
        else if (!strcmp(t, "others")) a->others = !strcmp(v, "1");
        else if (!strcmp(t, "dup")) a->dup = !strcmp(v, "1");
        else if (!strcmp(t, "arpdrop")) a->arpdrop = atoi(v);
        else return -1;
    }
    if (have != 7 || (a->psk && a->sec != 3) || ((a->net || a->dhcp || a->noping || a->pingin_ms || a->clear_ms || a->ivleft || a->others || a->dup || a->arpdrop) && !(a->net && a->psk))) return -1;
    if (a->psk) {                                                /* the PMK from the passphrase and the SSID (J.4) */
        struct wpa_pmk_job j;
        if (wpa_pmk_begin(&j, pass, strlen(pass), (const uint8_t *)a->ssid, strlen(a->ssid))) return -1;
        while (!wpa_pmk_step(&j, 4096)) {}
        if (wpa_pmk_take(&j, a->pmk)) return -1;
    }
    if (a->sec == 3 && !a->rsn_len) {
        static const uint8_t rsn[] = { 48, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0 };
        memcpy(a->rsn, rsn, sizeof rsn);
        a->rsn_len = sizeof rsn;
    }
    a->next_beacon = (uint64_t)(m->nap + 1) * 7000u;            /* each its own phase */
    m->nap++;
    return 0;
}

static unsigned channel_now(const sdio_model *m)
{
    uint32_t v = (m->wl[(FM1_WF_CON(7) - FM1_JL_WL_BASE) / 4u] & 0xFFu) - 101u;
    return v >= 1u && v <= 13u ? v : 0u;
}

static void frame_rec(sdio_model *m, int tx, unsigned ch, const uint8_t *f, unsigned n)
{
    wifi_rec *r = fix_add(&m->frames);
    r->kind = WIFI_REC_FRAME;
    r->write = tx;
    snprintf(r->attr, sizeof r->attr, "ch=%u", ch);
    for (unsigned i = 0; i < n; i++) rec_byte(r, f[i], 0);
}

/* A frame on the air to the library: kept when the receiver is on (MAC_SYS_CTRL RX, a filter written: the link is
 * up) and the filter lets it through, then into the FIFO as length word (big-endian), RXWI, frame, pad, trailer. */
static void deliver_x(sdio_model *m, const uint8_t *f, unsigned n, int8_t rssi, uint32_t info, uint32_t wcid)
{
    uint32_t filt = m->core[RX_FILTER_CFG / 4u], N = 16u + ((n + 3u) & ~3u);
    const uint32_t *ad = &m->core[MAC_ADDR_DW0 / 4u];
    uint8_t me[6] = { (uint8_t)ad[0], (uint8_t)(ad[0] >> 8), (uint8_t)(ad[0] >> 16), (uint8_t)(ad[0] >> 24),
                      (uint8_t)ad[1], (uint8_t)(ad[1] >> 8) };
    if (!(m->core[MAC_SYS_CTRL / 4u] & MAC_SYS_CTRL_ENABLE_RX) || !filt) return;
    if (f[4] & 1u) info |= f[4] == 0xFF ? 1u << 6 : 1u << 5;                 /* broadcast / multicast */
    else if (!memcmp(f + 4, me, 6)) info |= 1u << 4;                          /* unicast to me */
    else if (filt & RX_FILTER_CFG_DROP_NOT_TO_ME) return;
    if (m->fifo_n + N + 8u > 65536u) { m->rx_dropped++; return; }
    if (m->fifo_n + N + 8u > m->fifo_cap) { m->fifo_cap = 65536u; m->fifo = xrealloc(m->fifo, m->fifo_cap); }
    uint8_t *p = m->fifo + m->fifo_n;
    memset(p, 0, N + 8u);
    put32be(p, m->rx_bad ? 0u : N);
    m->rx_bad = 0;
    put32le(p + 4, (uint32_t)n << 16 | (wcid & 0xFFu));                     /* RXWI: MPDU byte count, WCID */
    put32le(p + 12, (uint8_t)rssi);                                          /* RSSI0 */
    memcpy(p + 20, f, n);
    put32be(p + 4 + N, info);
    m->fifo_n += N + 8u;
    frame_rec(m, 0, channel_now(m), f, n);
}

static void deliver(sdio_model *m, const uint8_t *f, unsigned n, int8_t rssi) { deliver_x(m, f, n, rssi, 0, 0xFFu); }

static unsigned mgmt_hdr(uint8_t *f, uint8_t kind, const uint8_t *da, const uint8_t *sa, const uint8_t *bssid, uint16_t *seq)
{
    memset(f, 0, 24);
    f[0] = kind;
    memcpy(f + 4, da, 6); memcpy(f + 10, sa, 6); memcpy(f + 16, bssid, 6);
    put16(f + 22, (uint32_t)((*seq)++ & 0xFFFu) << 4);
    return 24;
}

static unsigned ap_body(const sdio_model *m, const model_ap *a, uint8_t *f, int show_ssid)
{
    static const uint8_t rates[] = { 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
    static const uint8_t wpa[] = { 221, 22, 0x00, 0x50, 0xF2, 1, 1, 0, 0x00, 0x50, 0xF2, 2, 1, 0, 0x00, 0x50, 0xF2, 2, 1, 0, 0x00, 0x50, 0xF2, 2 };
    unsigned n = 0, k = show_ssid ? (unsigned)strlen(a->ssid) : 0;
    put32le(f, (uint32_t)m->now_us); put32le(f + 4, (uint32_t)(m->now_us >> 32));   /* timestamp */
    put16(f + 8, 100);                                                       /* beacon interval, TU */
    put16(f + 10, 0x0001u | 0x0400u | (a->sec ? 0x0010u : 0u));             /* ESS, short slot, privacy */
    n = 12;
    f[n++] = 0; f[n++] = (uint8_t)k; memcpy(f + n, a->ssid, k); n += k;
    memcpy(f + n, rates, sizeof rates); n += sizeof rates;
    f[n++] = 3; f[n++] = 1; f[n++] = a->ch;
    if (a->sec == 3) { memcpy(f + n, a->rsn, a->rsn_len); n += a->rsn_len; }
    if (a->sec == 2) { memcpy(f + n, wpa, sizeof wpa); n += sizeof wpa; }
    return n;
}

static void ap_send(sdio_model *m, model_ap *a, const uint8_t *f, unsigned n)
{
    if (channel_now(m) == a->ch) deliver(m, f, n, a->rssi);
}

static void ap_mgmt(sdio_model *m, model_ap *a, uint8_t kind, const uint8_t *body, unsigned nb)
{
    uint8_t f[64];
    unsigned n = mgmt_hdr(f, kind, a->sta, a->bssid, a->bssid, &a->seq);
    memcpy(f + n, body, nb);
    ap_send(m, a, f, n + nb);
}

/* ---- WPA2: the MAC's key tables as the hardware would use them, and the AP's side of the handshakes */

static uint32_t core_at(const sdio_model *m, uint32_t a) { return m->core[a / 4u]; }

static int key_is(const sdio_model *m, uint32_t at, const uint8_t *key)   /* a 32-byte entry: the key, 16 bytes of 0 */
{
    for (unsigned k = 0; k < 8u; k++)
        if (core_at(m, at + 4u * k) != (k < 4u ? rd32le(key + 4u * k) : 0u)) return 0;
    return 1;
}

static int wcid_of(const sdio_model *m, const uint8_t *a)   /* the WCID whose MAC_WCID_ENTRY holds address a, or -1 */
{
    for (int w = 0; w < 256; w++)
        if (core_at(m, MAC_WCID_ENTRY(w)) == rd32le(a) && (core_at(m, MAC_WCID_ENTRY(w) + 4u) & 0xFFFFu) == (a[4] | (uint32_t)a[5] << 8u))
            return w;
    return -1;
}

/* Would the MAC decrypt a protected frame from a: unicast, the AP's WCID (looked up by its address) pairwise AES
 * with the TK; group, the shared key with the frame's key ID AES in SHARED_KEY_MODE_ENTRY(0), WCID 0 shared AES */
static int rx_key_ok(const sdio_model *m, const model_ap *a, int group, unsigned idx, const uint8_t *gtk, int *wcid)
{
    *wcid = wcid_of(m, a->bssid);
    if (group)
        return (core_at(m, SHARED_KEY_MODE_ENTRY(0)) >> 4u * idx & 0xFu) == FM1WIFI_CIPHER_AES &&
               key_is(m, SHARED_KEY_ENTRY(idx), gtk) && (core_at(m, MAC_WCID_ATTR_ENTRY(0)) & 0xFu) == FM1WIFI_CIPHER_AES << 1;
    return *wcid >= 0 && (core_at(m, MAC_WCID_ATTR_ENTRY(*wcid)) & 0xFu) == (MAC_WCID_ATTRIBUTE_KEYTAB | FM1WIFI_CIPHER_AES << 1) &&
           key_is(m, PAIRWISE_KEY_ENTRY(*wcid), a->wa.ptk + 32);
}

/* Would the MAC encrypt a frame the library sent with JieLi's WIV and the TXWI's WCID under the TK a agreed */
static int tx_key_ok(const sdio_model *m, const model_ap *a, unsigned wiv, unsigned w)
{
    return !wiv && w < 256u && (core_at(m, MAC_WCID_ATTR_ENTRY(w)) & 0xFu) == (MAC_WCID_ATTRIBUTE_KEYTAB | FM1WIFI_CIPHER_AES << 1) &&
           (core_at(m, MAC_IVEIV_ENTRY(w)) >> 24 & FM1WIFI_IV3_EXT_IV) && key_is(m, PAIRWISE_KEY_ENTRY(w), a->wa.ptk + 32);
}

/* A data frame from a to da (From DS), RFC 1042 LLC/SNAP with ethertype type, then p. Protected, it arrives as the MAC
 * hands it over: decrypted (RXD DECRYPTED, IV and MIC taken off) when the key tables hold the key, else as it came
 * over the air (the CCMP header, the body encrypted, the MIC) with a key error */
static void ap_data_from(sdio_model *m, model_ap *a, const uint8_t *da, const uint8_t *src, uint32_t type, const uint8_t *p,
                         unsigned n, int protect, unsigned idx, const uint8_t *gtk)
{
    static const uint8_t snap[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
    uint8_t f[640];
    unsigned h = mgmt_hdr(f, 0x08, da, a->bssid, src, &a->seq);
    int w = -1;
    f[1] = (uint8_t)(0x02u | (protect ? 0x40u : 0u));                      /* From DS, Protected */
    if (channel_now(m) != a->ch) return;
    if (protect && !rx_key_ok(m, a, da[0] & 1u, idx, gtk, &w)) {
        uint8_t ccmp[8] = { 1, 0, 0, (uint8_t)(0x20u | idx << 6), 0, 0, 0, 0 };
        memcpy(f + h, ccmp, 8);
        for (unsigned i = 0; i < 8u + n; i++) f[h + 8 + i] = (uint8_t)((i < 6 ? snap[i] : i == 6 ? type >> 8 : i == 7 ? type : p[i - 8]) ^ 0xA5u);
        memset(f + h + 16 + n, 0x3C, 8);                                    /* MIC */
        deliver_x(m, f, h + 24 + n, a->rssi, 3u << 9, w < 0 ? 0xFFu : (uint32_t)w);   /* cipher error 3: key */
        return;
    }
    if (protect && a->ivleft) {                                           /* the CCMP header not taken off */
        uint8_t ccmp[8] = { 1, 0, 0, (uint8_t)(0x20u | idx << 6), 0, 0, 0, 0 };
        memcpy(f + h, ccmp, 8);
        h += 8;
    }
    memcpy(f + h, snap, 6);
    f[h + 6] = (uint8_t)(type >> 8); f[h + 7] = (uint8_t)type;
    memcpy(f + h + 8, p, n);
    if (!protect) deliver(m, f, h + 8 + n, a->rssi);
    else deliver_x(m, f, h + 8 + n, a->rssi, 1u << 16, w < 0 ? 0xFFu : (uint32_t)w);
}

static void ap_data(sdio_model *m, model_ap *a, const uint8_t *da, uint32_t type, const uint8_t *p, unsigned n, int protect,
                    unsigned idx, const uint8_t *gtk)
{
    ap_data_from(m, a, da, a->bssid, type, p, n, protect, idx, gtk);
}

/* ---- stage 9: the network behind a WPA2 AP (net=): a router at gw, the DHCP server (RFC 2131) and gateway (ARP, ICMP
 * echo), and a host at .50 that pings the station once it has its address. Everything to the station goes protected:
 * unicast under the PTK, broadcast under the GTK (the DHCP OFFER, the host's ARP request). */

static const uint8_t GW_MAC[6] = { 0x02, 0x47, 0x57, 0x00, 0x00, 0x01 }, HOST_MAC[6] = { 0x02, 0x48, 0x4F, 0x53, 0x54, 0x50 };

static unsigned be16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void wbe16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned csum(const uint8_t *p, unsigned n, uint32_t s)   /* RFC 1071, folded and inverted */
{
    for (unsigned i = 0; i + 1 < n; i += 2) s += be16(p + i);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return ~s & 0xFFFF;
}
static void ip_head(uint8_t *p, const uint8_t *src, const uint8_t *dst, unsigned proto, unsigned payload)
{
    static unsigned id = 0x5000;
    memset(p, 0, 20);
    p[0] = 0x45; wbe16(p + 2, 20 + payload); wbe16(p + 4, id++); p[8] = 64; p[9] = (uint8_t)proto;
    memcpy(p + 12, src, 4); memcpy(p + 16, dst, 4);
    wbe16(p + 10, csum(p, 20, 0));
}

static void net_send(sdio_model *m, model_ap *a, const uint8_t *da, const uint8_t *src, unsigned type, const uint8_t *p, unsigned n)
{
    int group = da[0] & 1u;
    ap_data_from(m, a, da, src, type, p, n, 1, group ? a->bc_idx : 0, group ? a->bc_gtk : 0);
    a->ns.to_sta++;
}

/* a DHCP reply of the given type to the client's request d (dn bytes) */
static void dhcp_reply_to(sdio_model *m, model_ap *a, const uint8_t *d, unsigned type, const uint8_t *chaddr, const uint8_t *yi);
static void dhcp_reply(sdio_model *m, model_ap *a, const uint8_t *d, unsigned type) { dhcp_reply_to(m, a, d, type, d + 28, a->lease_ip); }

static void dhcp_reply_to(sdio_model *m, model_ap *a, const uint8_t *d, unsigned type, const uint8_t *chaddr, const uint8_t *yi)
{
    static const uint8_t bc_ip[4] = { 255, 255, 255, 255 }, bc_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t p[400] = { 0 }, *u = p + 20, *r = u + 8, *o;
    int bcast = type == 2;                                             /* the OFFER broadcast, the ACK / NAK unicast */
    r[0] = 2; r[1] = 1; r[2] = 6;
    memcpy(r + 4, d + 4, 4);                                           /* xid */
    if (type != 6) memcpy(r + 16, yi, 4);
    memcpy(r + 28, chaddr, 6);                                         /* chaddr */
    r[236] = 99; r[237] = 130; r[238] = 83; r[239] = 99;
    o = r + 240;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 54; *o++ = 4; memcpy(o, a->gw, 4); o += 4;
    if (type != 6) {
        static const uint8_t mask[4] = { 255, 255, 255, 0 }, dns2[4] = { 1, 1, 1, 1 };
        *o++ = 51; *o++ = 4; *o++ = 0; *o++ = 1; *o++ = 0x51; *o++ = 0x80;   /* 86400 s */
        *o++ = 1; *o++ = 4; memcpy(o, mask, 4); o += 4;
        *o++ = 3; *o++ = 4; memcpy(o, a->gw, 4); o += 4;
        *o++ = 6; *o++ = 8; memcpy(o, a->gw, 4); memcpy(o + 4, dns2, 4); o += 8;
    }
    *o++ = 255;
    unsigned dn = (unsigned)(o - r), ul = 8 + dn;
    wbe16(u, 67); wbe16(u + 2, 68); wbe16(u + 4, ul); wbe16(u + 6, 0);
    ip_head(p, a->gw, bcast || type == 6 ? bc_ip : yi, 17, ul);
    {   uint8_t ph[12];
        memcpy(ph, p + 12, 8); ph[8] = 0; ph[9] = 17; wbe16(ph + 10, ul);
        uint32_t s = 0;
        for (int i = 0; i < 12; i += 2) s += be16(ph + i);
        unsigned c = csum(u, ul, s);
        wbe16(u + 6, c ? c : 0xFFFF); }
    net_send(m, a, bcast || type == 6 ? bc_mac : chaddr, GW_MAC, 0x0800, p, 20 + ul);
}

static void arp_send(sdio_model *m, model_ap *a, unsigned op, const uint8_t *smac, const uint8_t *sip, const uint8_t *tmac,
                     const uint8_t *tip, const uint8_t *da)
{
    uint8_t p[28] = { 0, 1, 8, 0, 6, 4, 0, (uint8_t)op };
    memcpy(p + 8, smac, 6); memcpy(p + 14, sip, 4); memcpy(p + 18, tmac, 6); memcpy(p + 24, tip, 4);
    net_send(m, a, da, smac, 0x0806, p, 28);
}

static void echo_send(sdio_model *m, model_ap *a, unsigned type, const uint8_t *src, const uint8_t *dst, const uint8_t *smac,
                      unsigned id, unsigned seq, const uint8_t *data, unsigned dl)
{
    uint8_t p[600];
    if (dl > 500) return;
    uint8_t *c = p + 20;
    c[0] = (uint8_t)type; c[1] = 0; c[2] = c[3] = 0; wbe16(c + 4, id); wbe16(c + 6, seq);
    memcpy(c + 8, data, dl);
    wbe16(c + 2, csum(c, 8 + dl, 0));
    ip_head(p, src, dst, 1, 8 + dl);
    net_send(m, a, a->sta, smac, 0x0800, p, 28 + dl);
}

/* the station's IPv4 or ARP packet p (n bytes) to the network */
static void ap_net(sdio_model *m, model_ap *a, unsigned type, const uint8_t *p, unsigned n)
{
    static const uint8_t host_ip_tail = 50;
    uint8_t host_ip[4];
    memcpy(host_ip, a->gw, 3); host_ip[3] = host_ip_tail;
    if (type == 0x0806 && n >= 28 && be16(p) == 1 && be16(p + 2) == 0x0800) {
        unsigned op = be16(p + 6);
        if (op == 1) {
            a->ns.arp_req++;
            if (!memcmp(p + 24, a->gw, 4) && a->arpdrop-- <= 0) { a->ns.arp_gw++; arp_send(m, a, 2, GW_MAC, a->gw, p + 8, p + 14, a->sta); }
        } else if (op == 2 && !memcmp(p + 24, host_ip, 4) && !memcmp(p + 18, HOST_MAC, 6)) {
            a->ns.arp_reply++;
            a->host_arp = 1;
        }
        return;
    }
    if (type != 0x0800 || n < 28 || p[0] != 0x45 || csum(p, 20, 0) != 0 || be16(p + 2) > n) return;
    n = be16(p + 2);
    if (p[9] == 17 && be16(p + 20) == 68 && be16(p + 22) == 67 && n >= 28 + 244) {   /* DHCP, client to server */
        const uint8_t *d = p + 28, *o = d + 240, *end = p + n;
        unsigned mt = 0;
        while (o + 2 <= end && *o != 255) { if (*o == 0) { o++; continue; } if (*o == 53 && o[1] == 1) mt = o[2]; o += 2 + o[1]; }
        if (mt == 1) a->ns.discover++; else if (mt == 3) a->ns.request++; else if (mt == 7) a->ns.release++;
        if (a->dhcp == 1) return;
        if (mt == 1 && a->others) {                                     /* another client's OFFER (same xid), broadcast */
            static const uint8_t other[6] = { 0x02, 0x99, 0x99, 0x99, 0x99, 0x99 };
            uint8_t yi[4];
            memcpy(yi, a->gw, 3); yi[3] = 99;
            dhcp_reply_to(m, a, d, 2, other, yi);
        }
        if (mt == 1) dhcp_reply(m, a, d, 2);
        else if (mt == 3) {
            dhcp_reply(m, a, d, a->dhcp == 2 ? 6 : 5);
            if (a->dhcp != 2 && !a->acked) { a->acked = 1; a->acked_at = m->now_us; a->next_pingin = m->now_us + (uint64_t)a->pingin_ms * 1000u; }
        }
        return;
    }
    if (p[9] == 1 && n >= 28) {
        const uint8_t *c = p + 20;
        if (csum(c, n - 20, 0) != 0) { a->ns.bad_icmp++; return; }
        if (c[0] == 8 && !memcmp(p + 16, a->gw, 4)) {                  /* to the gateway */
            a->ns.ping_req++;
            if (a->noping) return;
            if (a->ping_delay_ms) {
                for (int k = 0; k < 4; k++)
                    if (!a->held[k].n && n <= sizeof a->held[k].p) {
                        memcpy(a->held[k].p, p, n); a->held[k].n = n;
                        a->held[k].at = m->now_us + (uint64_t)a->ping_delay_ms * 1000u;
                        break;
                    }
                return;
            }
            echo_send(m, a, 0, a->gw, p + 12, GW_MAC, be16(c + 4), be16(c + 6), c + 8, n - 28);
            if (a->dup) echo_send(m, a, 0, a->gw, p + 12, GW_MAC, be16(c + 4), be16(c + 6), c + 8, n - 28);
            a->ns.ping_reply++;
        } else if (c[0] == 0 && !memcmp(p + 16, host_ip, 4) && be16(c + 4) == 0x4854) a->ns.pingin_got++;
    }
}

/* due by the clock: the host's ARP request and pings, delayed echo replies */
static void net_air(sdio_model *m, model_ap *a)
{
    uint8_t host_ip[4], zero[6] = { 0 };
    static const uint8_t bc_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    memcpy(host_ip, a->gw, 3); host_ip[3] = 50;
    for (int k = 0; k < 4; k++)
        if (a->held[k].n && m->now_us >= a->held[k].at) {
            const uint8_t *p = a->held[k].p, *c = p + 20;
            echo_send(m, a, 0, a->gw, p + 12, GW_MAC, be16(c + 4), be16(c + 6), c + 8, a->held[k].n - 28);
            a->ns.ping_reply++;
            a->held[k].n = 0;
        }
    if (a->clear_ms && m->now_us >= a->next_clear) {                  /* a broadcast ARP request in the clear */
        if (a->next_clear) {
            uint8_t p[28] = { 0, 1, 8, 0, 6, 4, 0, 1 };
            memcpy(p + 8, GW_MAC, 6); memcpy(p + 14, a->gw, 4); memcpy(p + 24, a->lease_ip, 4);
            ap_data_from(m, a, bc_mac, GW_MAC, 0x0806, p, 28, 0, 0, 0);
            a->ns.clear_sent++;
        }
        a->next_clear = m->now_us + (uint64_t)a->clear_ms * 1000u;
    }
    if (!a->pingin_ms || !a->acked || m->now_us < a->next_pingin) return;
    a->next_pingin += (uint64_t)a->pingin_ms * 1000u;
    if (!a->host_arp) { arp_send(m, a, 1, HOST_MAC, host_ip, zero, a->lease_ip, bc_mac); a->ns.host_arp_req++; return; }
    {   uint8_t data[32];
        for (int i = 0; i < 32; i++) data[i] = (uint8_t)('a' + i % 23);
        echo_send(m, a, 8, host_ip, a->lease_ip, HOST_MAC, 0x4854, a->pingin_seq++, data, sizeof data);
        a->ns.pingin_sent++; }
}

void sdio_model_ap_net(const sdio_model *m, int i, sdio_model_net *ns) { *ns = m->ap[i].ns; }

static void ap_eapol(sdio_model *m, model_ap *a, const uint8_t *pdu, size_t n, int protect)
{
    ap_data(m, a, a->sta, 0x888Eu, pdu, (unsigned)n, protect, 0, 0);
    a->sent_at = m->now_us;
}

static void ap_send_msg(sdio_model *m, model_ap *a, int which)   /* 1 msg 1, 3 msg 3 (a bad MIC when told so) */
{
    uint8_t f[256];
    size_t n = which == 1 ? wpa_auth_msg1(&a->wa, f) : wpa_auth_msg3(&a->wa, f);
    if (which == 3 && a->eapol == 2) f[F_MIC] ^= 0x01;
    ap_eapol(m, a, f, n, 0);
}

/* the association taken: the 4-way handshake starts with the RSNE the station sent */
static void ap_wpa_start(sdio_model *m, model_ap *a, const uint8_t *f, unsigned n)
{
    const uint8_t *e = f + 28, *rsne = 0;
    uint8_t gtk[16];
    for (; e + 2 <= f + n && e + 2 + e[1] <= f + n; e += 2 + e[1])
        if (e[0] == 48) { rsne = e; break; }
    for (int i = 0; i < 16; i++) gtk[i] = (uint8_t)(0x60 + i);
    wpa_auth_init(&a->wa, a->pmk, a->bssid, a->sta, a->rsn, a->rsn_len, rsne ? rsne : f, rsne ? 2u + rsne[1] : 0u, gtk, 1,
                  (uint8_t)(m->now_us >> 10));
    a->done = a->rekeyed = 0;
    a->tries = 1;
    if (a->eapol != 1) ap_send_msg(m, a, 1);
}

/* A data frame the library sent to a (To DS): EAPOL to the authenticator. Protected, the MAC must have encrypted it
 * with the TK (WIV 0, the TXWI's WCID the AP's entry); after the 4-way handshake nothing is taken in the clear. */
static void ap_hear_data(sdio_model *m, model_ap *a, const uint8_t *f, unsigned n, unsigned wiv, unsigned wcid)
{
    unsigned prot = f[1] & 0x40u, h = (f[0] & 0x80u) ? 26u : 24u;
    int r;
    if (!a->psk || !a->joined || (f[1] & 3u) != 1u || memcmp(f + 10, a->sta, 6)) return;
    if (prot ? !tx_key_ok(m, a, wiv, wcid) : a->done) { a->refused++; return; }
    if (n < h + 8u || f[h] != 0xAA || f[h + 1] != 0xAA || f[h + 2] != 0x03) return;
    if (f[h + 6] != 0x88 || f[h + 7] != 0x8E) {
        if (a->net && a->done && prot) { a->ns.from_sta++; ap_net(m, a, (unsigned)f[h + 6] << 8 | f[h + 7], f + h + 8, n - h - 8); }
        return;
    }
    r = wpa_auth_take(&a->wa, f + h + 8, n - h - 8);
    if (r == 2) { a->tries = 1; ap_send_msg(m, a, 3); }
    else if (r == 4) {
        a->done = 1;
        a->done_at = m->now_us;
        a->next_bcast = m->now_us + (uint64_t)a->bcast_ms * 1000u;
        memcpy(a->bc_gtk, a->wa.gtk, 16);
        a->bc_idx = a->wa.gtk_idx;
    } else if (r == 6) { memcpy(a->bc_gtk, a->wa.gtk, 16); a->bc_idx = a->wa.gtk_idx; a->group_done++; }
    else a->refused++;
}

void sdio_model_ap_wpa(const sdio_model *m, int i, int *done, int *group_done, uint32_t *refused, uint8_t ptk[48])
{
    const model_ap *a = &m->ap[i];
    *done = a->joined && a->done;
    *group_done = a->group_done;
    *refused = a->refused;
    if (ptk) memcpy(ptk, a->wa.ptk, 48);
}

/* Beacons, a planned deauthentication: whatever is due by the model's clock. */
static void air(sdio_model *m)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    for (int i = 0; i < m->nap; i++) {
        model_ap *a = &m->ap[i];
        int gone = a->joined && a->gone_ms && m->now_us >= a->joined_at + (uint64_t)a->gone_ms * 1000u;
        if (a->joined && a->kick_ms && m->now_us >= a->joined_at + (uint64_t)a->kick_ms * 1000u) {
            uint8_t b[2] = { 2, 0 };                                         /* reason 2: no longer valid */
            ap_mgmt(m, a, 0xC0, b, 2);
            a->joined = a->authed = 0;
        }
        if (a->psk && a->joined && !a->done && a->eapol != 1 && m->now_us - a->sent_at >= 1000000u) {
            if (a->tries < 4) { a->tries++; ap_send_msg(m, a, a->wa.stage == WPA_AUTH_MSG3 ? 3 : 1); }
            else { uint8_t b[2] = { 15, 0 }; ap_mgmt(m, a, 0xC0, b, 2); a->joined = a->authed = 0; }   /* 15: 4-way timeout */
        }
        if (a->psk && a->joined && a->done && a->rekey_ms && !a->rekeyed && m->now_us >= a->done_at + (uint64_t)a->rekey_ms * 1000u) {
            uint8_t g[256];
            size_t gn;
            for (int k = 0; k < 16; k++) a->wa.gtk[k] = (uint8_t)(a->wa.gtk[k] + 0x11);
            a->wa.gtk_idx = a->wa.gtk_idx == 1 ? 2 : 1;
            gn = wpa_auth_group1(&a->wa, g);
            a->rekeyed = 1;
            ap_eapol(m, a, g, gn, 1);
        }
        if (a->net && a->joined && a->done) net_air(m, a);
        if (a->psk && a->joined && a->done && a->bcast_ms && m->now_us >= a->next_bcast) {
            static const uint8_t arp[28] = { 0, 1, 8, 0, 6, 4, 0, 1, 2, 0, 0, 0, 0, 1, 192, 168, 1, 1, 0, 0, 0, 0, 0, 0, 192, 168, 1, 77 };
            ap_data(m, a, bcast, 0x0806u, arp, sizeof arp, 1, a->bc_idx, a->bc_gtk);
            a->next_bcast += (uint64_t)a->bcast_ms * 1000u;
        }
        while (a->next_beacon <= m->now_us) {
            if (!gone) {
                uint8_t f[160];
                unsigned n = mgmt_hdr(f, 0x80, bcast, a->bssid, a->bssid, &a->seq);
                n += ap_body(m, a, f + n, !a->hidden);
                ap_send(m, a, f, n);
            }
            a->next_beacon += 102400u;                                       /* 100 TU */
        }
    }
}

/* A frame the library sent, on the current channel: the access points there answer as IEEE 802.11 says. */
static void ap_hear(sdio_model *m, const uint8_t *f, unsigned n, unsigned wiv, unsigned wcid)
{
    unsigned ch = channel_now(m);
    uint8_t kind = f[0] & 0xFC;
    if (n < 24) return;
    for (int i = 0; i < m->nap; i++) {
        model_ap *a = &m->ap[i];
        uint8_t r[160];
        if (a->ch != ch) continue;
        if (kind == 0x40) {                                                  /* probe request: SSID element first */
            unsigned k = n >= 26 && f[24] == 0 ? f[25] : 0;
            if (k ? k != strlen(a->ssid) || memcmp(f + 26, a->ssid, k) : a->hidden) continue;
            memcpy(a->sta, f + 10, 6);
            unsigned m2 = mgmt_hdr(r, 0x50, a->sta, a->bssid, a->bssid, &a->seq);
            m2 += ap_body(m, a, r + m2, 1);
            ap_send(m, a, r, m2);
            continue;
        }
        if (memcmp(f + 4, a->bssid, 6)) continue;
        if ((kind & 0x0C) == 0x08) { ap_hear_data(m, a, f, n, wiv, wcid); continue; }   /* data */
        memcpy(a->sta, f + 10, 6);
        if (kind == 0xB0 && a->auth != 2) {                                  /* authentication */
            uint8_t b[6] = { 0, 0, 2, 0, (uint8_t)(a->auth ? 1 : 0), 0 };
            a->authed = !a->auth;
            ap_mgmt(m, a, 0xB0, b, 6);
        } else if (kind == 0x00 && a->assoc != 2) {                          /* association request */
            if (!a->authed) { uint8_t b[2] = { 6, 0 }; ap_mgmt(m, a, 0xC0, b, 2); continue; }
            uint8_t b[16] = { 0x01, 0x04, (uint8_t)(a->assoc ? 17 : 0), 0, 1, 0xC0, 1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
            ap_mgmt(m, a, 0x10, b, 16);
            if (!a->assoc) { a->joined = 1; a->joined_at = m->now_us; if (a->psk) ap_wpa_start(m, a, f, n); }
        } else if (kind == 0xC0) {                                           /* the station leaves */
            a->joined = a->authed = 0;
            a->acked = a->host_arp = 0;
        }
    }
}

/* A DMA address the port handed out, or 0 (a length written with any other address in place: no transfer, the done
 * bit stays clear, as a DMA to nowhere would leave it) */
static uint8_t *bus_ptr(sdio_model *m, uint32_t h)
{
    uint32_t k = (h - 0x10000000u) >> 20;
    if (h < 0x10000000u || k >= (uint32_t)m->nbus) return NULL;
    return (uint8_t *)m->bus[k] + (h & 0xFFFFFu);
}

static void rx_dma(sdio_model *m, uint32_t len)
{
    uint8_t *d = bus_ptr(m, m->core[(FM1_WL30_HOST_18 - FM1_WIFI_CORE_BASE) / 4u]);
    size_t k = len < m->fifo_n ? len : m->fifo_n;
    if (m->hang_rxdma || !d || len > 0xFFFFu) return;
    memcpy(d, m->fifo, k);
    memset(d + k, 0, len - k);
    memmove(m->fifo, m->fifo + k, m->fifo_n - k);
    m->fifo_n -= k;
    m->rx_done = 1;
}

static void tx_dma(sdio_model *m, uint32_t len)
{
    const uint8_t *s = bus_ptr(m, m->core[(FM1_WL30_HOST_10 - FM1_WIFI_CORE_BASE) / 4u]);
    uint32_t n;
    if (m->hang_txdma || !s) return;
    n = (rd32le(s + 8) >> 16) & 0xFFFu;                                      /* TXWI word 1: MPDU byte count */
    if (len < 26u || n + 26u != len) { fprintf(stderr, "sdio_model: TX DMA of %u bytes for a %u-byte frame\n", len, n); exit(2); }
    frame_rec(m, 1, channel_now(m), s + 20, n);
    m->tx_done = 1;
    m->tx_ok++;
    ap_hear(m, s + 20, n, s[0] & 1u, (rd32le(s + 8) >> 8) & 0xFFu);   /* JieLi's WIV, the TXWI's WCID */
}

/* ---- the port */

uint32_t fm1wifi_port_rd32(uint32_t addr)
{
    sdio_model *m = model();
    uint32_t v = 0, *r = reg_at(m, addr);
    if (!r) m->bad++;
    else {
        mqueue *q = queue_at(m, addr, 0);
        if (q && q->stuck) { v = q->v[0]; poll(m, POLL_STUCK, 1); }
        else if (q && q->pos < q->n) v = q->v[q->pos++];
        else if (addr == FM1_WLA_CON(30)) v = status(m);
        else if (addr == FM1_WIFI_CORE(RX_STA_CNT1) || addr == FM1_WIFI_CORE(CH_BUSY_STA)) v = survey_read(m, addr - FM1_WIFI_CORE_BASE);
        else if (addr == FM1_WL30_HOST_04)
            v = (*r & ~(FM1_WL30_04_RX_READY | FM1_WL30_04_TX_DONE | FM1_WL30_04_RX_DONE)) |
                (m->fifo_n ? FM1_WL30_04_RX_READY : 0u) | (m->rx_done ? FM1_WL30_04_RX_DONE : 0u) |
                (m->tx_done ? FM1_WL30_04_TX_DONE : 0u);
        else if (addr == FM1_WIFI_CORE(TX_STA_CNT1)) { v = m->tx_ok > 0xFFFFu ? 0xFFFFu : m->tx_ok; m->tx_ok = 0; }
        else if (addr == FM1_WIFI_CORE(RX_STA_CNT2)) { v = (m->rx_dropped > 0xFFFFu ? 0xFFFFu : m->rx_dropped) << 16; m->rx_dropped = 0; }
        else if (addr == FM1_WL_ANL_CON(0)) {
            v = (*r & ~0x700000u) | (m->hang_anl ? 0x100000u : 0u);
            poll(m, POLL_ANL, m->hang_anl);
        } else v = *r;
    }
    log_rec(m, WIFI_REC_RD, addr, v);
    return v;
}

/* BBP_CSR_CFG with BUSY set starts a transfer: the model does it at once, leaving the value (a read) in bits
 * 7-0 and BUSY clear (unless a next or stuck line answers the polls). */
void fm1wifi_port_wr32(uint32_t addr, uint32_t v)
{
    sdio_model *m = model();
    uint32_t *r = reg_at(m, addr);
    if (m->on_wr) m->on_wr(m->on_wr_ctx, addr, v);
    log_rec(m, WIFI_REC_WR, addr, v);
    m->poll[POLL_STUCK] = 0;                                  /* a stuck register's polls: a run without writes */
    if (!r) { m->bad++; return; }
    if (addr == FM1_WIFI_CORE(BBP_CSR_CFG)) {
        if (v & BBP_CSR_CFG_BUSY) {
            uint8_t reg = (uint8_t)((v & BBP_CSR_CFG_REGNUM) >> 8);
            if (v & BBP_CSR_CFG_READ_CONTROL) v = (v & ~BBP_CSR_CFG_VALUE) | m->bbp[reg];
            else bbp_write(m, reg, (uint8_t)(v & BBP_CSR_CFG_VALUE));
            v &= ~BBP_CSR_CFG_BUSY;
        }
    } else {
        wlog_rec(m, WIFI_REC_WR, addr, v);
    }
    if (addr == FM1_WL30_HOST_04) {                           /* the done bits are the model's; 10 and 11 clear them */
        if (v & FM1_WL30_04_TX_CLR) m->tx_done = 0;
        if (v & FM1_WL30_04_RX_CLR) m->rx_done = 0;
        v &= ~(FM1_WL30_04_RX_READY | FM1_WL30_04_TX_DONE | FM1_WL30_04_RX_DONE);
    }
    *r = v;
    if (addr == FM1_WL30_HOST_1C) rx_dma(m, v);
    if (addr == FM1_WL30_HOST_14) tx_dma(m, v);
}

uint32_t fm1wifi_port_clk(uint32_t bits)
{
    sdio_model *m = model();
    uint32_t old = m->clk;
    m->clk = bits & FM1_CLK_CON1_WIFI;
    log_rec(m, WIFI_REC_CLK, bits, old);
    return old;
}

void fm1wifi_port_delay_us(uint32_t us)
{
    sdio_model *m = model();
    log_rec(m, WIFI_REC_DELAY, 0, us);
    wlog_rec(m, WIFI_REC_DELAY, 0, us);
    m->us_cca += us;
    m->us_busy += us;
    m->now_us += us;
    air(m);
}

uint32_t fm1wifi_port_bus(const void *p)
{
    sdio_model *m = model();
    int k;
    for (k = 0; k < m->nbus && m->bus[k] != p; k++) {}
    if (k == m->nbus) {
        if (m->nbus == 16) { fputs("sdio_model: more than 16 DMA buffers\n", stderr); exit(2); }
        m->bus[m->nbus++] = (void *)p;
    }
    return 0x10000000u + ((uint32_t)k << 20);
}

void fm1wifi_port_trim(int on)
{
    sdio_model *m = model();
    if (on) { m->trims++; m->in_trim = 1; }
    else if (!m->in_trim) m->trims = -1;
    else m->in_trim = 0;
}
