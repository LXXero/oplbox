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
    case WIFI_REC_WR: return w->addr == g->addr && w->val == g->val;
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

typedef struct { uint32_t addr, val; } mreg;
typedef struct { uint32_t addr; uint32_t *v; unsigned n, pos; int stuck; } mqueue;

struct sdio_model {
    mreg *reg;
    int nreg;
    mqueue *q;
    int nq;
    uint8_t bbp[256];
    uint32_t clk;
    int bad;
    wifi_fix log;
};

static sdio_model *cur;

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
    free(m->reg);
    wifi_fix_free(&m->log);
    free(m);
}

void sdio_model_use(sdio_model *m) { cur = m; }
const wifi_fix *sdio_model_log(const sdio_model *m) { return &m->log; }
uint32_t sdio_model_clk(const sdio_model *m) { return m->clk; }
uint8_t sdio_model_bbp(const sdio_model *m, uint8_t reg) { return m->bbp[reg]; }
int sdio_model_bad(const sdio_model *m) { return m->bad; }

static mreg *reg_at(sdio_model *m, uint32_t addr, int make)
{
    for (int i = 0; i < m->nreg; i++)
        if (m->reg[i].addr == addr) return &m->reg[i];
    if (!make) return NULL;
    m->reg = xrealloc(m->reg, (size_t)(m->nreg + 1) * sizeof *m->reg);
    m->reg[m->nreg].addr = addr;
    m->reg[m->nreg].val = 0;
    return &m->reg[m->nreg++];
}

uint32_t sdio_model_peek(const sdio_model *m, uint32_t addr)
{
    mreg *r = reg_at((sdio_model *)m, addr, 0);
    return r ? r->val : 0;
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

/* The script: reg, next, stuck, bbp, clk (sdio_model.h). */
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
        if (strcmp(t, "reg") && strcmp(t, "next") && strcmp(t, "stuck")) { rc = perr(err, name, line, "unknown script line"); break; }
        int kind = t[0] == 'r' ? 0 : t[0] == 'n' ? 1 : 2;
        if (!(t = tok(&p)) || hex_u32(t, &a) || (a & 3u)) { rc = perr(err, name, line, "a word-aligned address expected"); break; }
        mqueue *q = kind ? queue_at(m, a, 1) : NULL;
        int n = 0;
        while ((t = tok(&p))) {
            if (hex_u32(t, &v)) { rc = perr(err, name, line, "bad value"); break; }
            n++;
            if (!kind) { reg_at(m, a, 1)->val = v; continue; }
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

static int wifi_addr(uint32_t a)   /* fm1wifi_port.h's blocks, as the felucca-bsp port has them */
{
    return !(a & 3u) && ((a >= FM1_WLA_CON(0) && a <= FM1_WLA_CON(FM1_WLA_CON_N - 1u)) ||
                         (a >= FM1_JL_WL_BASE && a <= FM1_WL_SD_DPTR) ||
                         (a >= FM1_WIFI_CORE_BASE && a <= FM1_WIFI_CORE_LAST));
}

static void log_rec(sdio_model *m, int kind, uint32_t addr, uint32_t val)
{
    wifi_rec *r = fix_add(&m->log);
    r->kind = kind;
    r->addr = addr;
    r->val = val;
}

uint32_t fm1wifi_port_rd32(uint32_t addr)
{
    sdio_model *m = model();
    uint32_t v = 0;
    if (!wifi_addr(addr)) m->bad++;
    else {
        mqueue *q = queue_at(m, addr, 0);
        if (q && q->stuck) v = q->v[0];
        else if (q && q->pos < q->n) v = q->v[q->pos++];
        else v = sdio_model_peek(m, addr);
    }
    log_rec(m, WIFI_REC_RD, addr, v);
    return v;
}

/* BBP_CSR_CFG with BUSY set starts a transfer: the model does it at once, leaving the value (a read) in bits
 * 7-0 and BUSY clear (unless a next or stuck line answers the polls). */
void fm1wifi_port_wr32(uint32_t addr, uint32_t v)
{
    sdio_model *m = model();
    log_rec(m, WIFI_REC_WR, addr, v);
    if (!wifi_addr(addr)) { m->bad++; return; }
    if (addr == FM1_WIFI_CORE(BBP_CSR_CFG) && (v & BBP_CSR_CFG_BUSY)) {
        uint8_t reg = (uint8_t)((v & BBP_CSR_CFG_REGNUM) >> 8);
        if (v & BBP_CSR_CFG_READ_CONTROL) v = (v & ~BBP_CSR_CFG_VALUE) | m->bbp[reg];
        else m->bbp[reg] = (uint8_t)(v & BBP_CSR_CFG_VALUE);
        v &= ~BBP_CSR_CFG_BUSY;
    }
    reg_at(m, addr, 1)->val = v;
}

uint32_t fm1wifi_port_clk(uint32_t bits)
{
    sdio_model *m = model();
    uint32_t old = m->clk;
    m->clk = bits & FM1_CLK_CON1_WIFI;
    log_rec(m, WIFI_REC_CLK, bits, old);
    return old;
}

void fm1wifi_port_delay_us(uint32_t us) { log_rec(model(), WIFI_REC_DELAY, 0, us); }
