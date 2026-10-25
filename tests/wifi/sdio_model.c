/* SPDX-License-Identifier: GPL-3.0-only
 * Host register model of the internal SDIO host, the fixture format and its diff (see sdio_model.h).
 * Own code, written from docs/WIFI.md and the SDIO Simplified Specification. Host only.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "fm1_wifi_regs.h"
#include "sdio_model.h"

#define CMD53_MAX 2048u   /* the model's limit for one CMD53, enough for a 802.11 frame */

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fputs("sdio_model: out of memory\n", stderr); exit(2); }
    return q;
}

/* ---- the fixture format ---- */

static const char *const status_names[] = { "ok", "enotyet", "erange", "ebus", "etimeout" };

const char *wifi_status_name(int s) { return s <= 0 && s >= -4 ? status_names[-s] : "?"; }

static int status_parse(const char *t, int *s)
{
    for (int i = 0; i < 5; i++)
        if (!strcmp(t, status_names[i])) { *s = -i; return 0; }
    return -1;
}

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
        r->any = xrealloc(r->any, r->n + 64);
    }
    r->data[r->n] = b;
    r->any[r->n++] = any;
}

void wifi_fix_free(wifi_fix *f)
{
    for (int i = 0; i < f->n; i++) { free(f->r[i].data); free(f->r[i].any); }
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

static int hexdig(char c) { return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10; }

/* A data token: hex pairs run together, "??" for any byte. */
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

static int fn_tok(const char *t, unsigned *fn)
{
    if (t[0] != 'f' || t[1] < '0' || t[1] > '7' || t[2]) return -1;
    *fn = (unsigned)(t[1] - '0');
    return 0;
}

/* The rest of a CMD53 / CMD52 / frame line: data, then optionally "!" and the error. */
static const char *data_tail(wifi_rec *r, char **p, int *has_status)
{
    char *t;
    while ((t = tok(p))) {
        if (*has_status) return "nothing may follow the error";
        if (!strcmp(t, "!")) {
            if (!(t = tok(p))) return "\"!\" without an error";
            if (status_parse(t, &r->status)) return "unknown error name";
            *has_status = 1;
            continue;
        }
        if (data_tok(r, t)) return "bad data byte";
    }
    return NULL;
}

static int perr(FILE *err, const char *name, int line, const char *what)
{
    if (err) fprintf(err, "%s:%d: %s\n", name, line, what);
    return -1;
}

static const char *rec_check(const wifi_rec *r)
{
    int failed = r->status != FM1_WIFI_OK;
    if (failed && !r->write && r->kind != WIFI_REC_FRAME && r->n) return "a failed read has no data";
    switch (r->kind) {
    case WIFI_REC_CMD52:
        if (!failed && r->n != 1) return "CMD52 carries one byte";
        if (r->n > 1) return "CMD52 carries one byte";
        break;
    case WIFI_REC_CMD53:
        if (!failed && r->n == 0) return "CMD53 without data";
        if (r->n > CMD53_MAX) return "CMD53 longer than the model's 2048 bytes";
        break;
    case WIFI_REC_FRAME:
        if (r->n == 0) return "frame without data";
        break;
    }
    return NULL;
}

int wifi_fix_parse(wifi_fix *f, const char *text, const char *name, FILE *err)
{
    size_t len = strlen(text);
    char *buf = xrealloc(NULL, len + 1);
    memcpy(buf, text, len + 1);
    int line = 0, rc = 0, last = -1, last_status = 0;
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
            if (last < 0 || (f->r[last].kind != WIFI_REC_CMD53 && f->r[last].kind != WIFI_REC_FRAME)) {
                rc = perr(err, name, line, "a continuation line follows no CMD53 or frame record");
                break;
            }
            if ((why = data_tail(&f->r[last], &p, &last_status))) rc = perr(err, name, line, why);
            continue;
        }
        t = tok(&p);
        if (last >= 0 && (why = rec_check(&f->r[last]))) { rc = perr(err, name, f->r[last].line, why); break; }
        wifi_rec *r = fix_add(f);
        r->line = line;
        last = f->n - 1;
        last_status = 0;
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
            if (rc) break;
            if ((why = data_tail(r, &p, &last_status))) rc = perr(err, name, line, why);
            continue;
        }
        char *end = NULL;
        unsigned long idx = 0;
        if (!strncmp(t, "cmd", 3) && isdigit((unsigned char)t[3])) idx = strtoul(t + 3, &end, 10);
        if (!end || *end || idx > 63) {
            rc = perr(err, name, line, "unknown record");
            break;
        }
        r->idx = (unsigned)idx;
        if (idx == 52 || idx == 53) {
            r->kind = idx == 52 ? WIFI_REC_CMD52 : WIFI_REC_CMD53;
            if (!(t = tok(&p)) || (strcmp(t, "r") && strcmp(t, "w"))) { rc = perr(err, name, line, "r or w expected"); break; }
            r->write = t[0] == 'w';
            if (!(t = tok(&p)) || fn_tok(t, &r->fn)) { rc = perr(err, name, line, "function f0-f7 expected"); break; }
            if (!(t = tok(&p)) || hex_u32(t, &r->addr) || r->addr > FM1_SDIO_ADDR_MAX) {
                rc = perr(err, name, line, "17-bit address expected");
                break;
            }
            if ((why = data_tail(r, &p, &last_status))) rc = perr(err, name, line, why);
            continue;
        }
        r->kind = WIFI_REC_CMD;
        if (!(t = tok(&p)) || hex_u32(t, &r->arg)) { rc = perr(err, name, line, "argument expected"); break; }
        if (!(t = tok(&p))) { rc = perr(err, name, line, "\"->\" and the response, or \"!\" and the error, expected"); break; }
        if (!strcmp(t, "->")) {
            if (!(t = tok(&p))) { rc = perr(err, name, line, "response expected"); break; }
            if (!strcmp(t, "?")) r->resp_any = 1;
            else if (hex_u32(t, &r->resp)) { rc = perr(err, name, line, "bad response"); break; }
        } else if (!strcmp(t, "!")) {
            if (!(t = tok(&p)) || status_parse(t, &r->status) || r->status == FM1_WIFI_OK) {
                rc = perr(err, name, line, "error name expected");
                break;
            }
        } else {
            rc = perr(err, name, line, "\"->\" or \"!\" expected");
            break;
        }
        if (tok(&p)) rc = perr(err, name, line, "nothing may follow the response");
    }
    if (!rc && last >= 0 && (why = rec_check(&f->r[last]))) rc = perr(err, name, f->r[last].line, why);
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
    if (r->kind == WIFI_REC_CMD) {
        fprintf(o, "cmd%u 0x%08x", r->idx, (unsigned)r->arg);
        if (r->status != FM1_WIFI_OK) fprintf(o, " ! %s", wifi_status_name(r->status));
        else if (r->resp_any) fputs(" -> ?", o);
        else fprintf(o, " -> 0x%08x", (unsigned)r->resp);
    } else {
        if (r->kind == WIFI_REC_FRAME) {
            fprintf(o, "frame %s", r->write ? "tx" : "rx");
            if (r->attr[0]) fprintf(o, " %s", r->attr);
        } else {
            fprintf(o, "cmd%u %c f%u 0x%05x", r->kind == WIFI_REC_CMD52 ? 52u : 53u, r->write ? 'w' : 'r', r->fn,
                    (unsigned)r->addr);
        }
        for (unsigned i = 0; i < r->n; i++) {
            if (one_line && i == 16) { fprintf(o, " .. (%u bytes)", r->n); break; }
            if (!one_line && i && i % 16 == 0) fputs("\n ", o);
            if (r->any && r->any[i]) fputs(" ??", o);
            else fprintf(o, " %02x", r->data[i]);
        }
        if (r->status != FM1_WIFI_OK) fprintf(o, " ! %s", wifi_status_name(r->status));
    }
    if (!one_line) fputc('\n', o);
}

void wifi_fix_write(FILE *out, const wifi_fix *f)
{
    for (int i = 0; i < f->n; i++) wifi_rec_write(out, &f->r[i], 0);
}

int wifi_rec_match(const wifi_rec *w, const wifi_rec *g)
{
    if (w->kind != g->kind || w->status != g->status || w->n != g->n) return 0;
    switch (w->kind) {
    case WIFI_REC_CMD:
        if (w->idx != g->idx || w->arg != g->arg) return 0;
        if (w->status == FM1_WIFI_OK && !w->resp_any && w->resp != g->resp) return 0;
        break;
    case WIFI_REC_CMD52:
    case WIFI_REC_CMD53:
        if (w->write != g->write || w->fn != g->fn || w->addr != g->addr) return 0;
        break;
    case WIFI_REC_FRAME:
        if (w->write != g->write || strcmp(w->attr, g->attr)) return 0;
        break;
    }
    for (unsigned i = 0; i < w->n; i++)
        if (!(w->any && w->any[i]) && w->data[i] != g->data[i]) return 0;
    return 1;
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

enum { ST_IDLE, ST_READY, ST_STBY, ST_SELECTED };   /* the card states the model keeps */

typedef struct { unsigned fn; uint32_t addr; uint8_t *b; unsigned n, pos; } mqueue;
typedef struct { unsigned idx; int has_fn, has_addr; unsigned fn; uint32_t addr; int status, used; } mfail;

struct sdio_model {
    uint8_t *reg;           /* 8 functions x 128 KiB */
    uint32_t ocr;
    uint32_t rca[16];
    int nrca, rca_pos;
    uint32_t cur_rca;
    int state;
    mqueue *q;
    int nq;
    mfail *fail;
    int nfail;
    wifi_fix log;
};

static sdio_model *cur;

sdio_model *sdio_model_new(void)
{
    sdio_model *m = xrealloc(NULL, sizeof *m);
    memset(m, 0, sizeof *m);
    m->reg = xrealloc(NULL, (FM1_SDIO_FN_MAX + 1u) * (FM1_SDIO_ADDR_MAX + 1u));
    memset(m->reg, 0, (FM1_SDIO_FN_MAX + 1u) * (FM1_SDIO_ADDR_MAX + 1u));
    m->ocr = FM1_SDIO_R4_READY | 1u << 28 | 0xFF8000u;   /* ready, one function, no memory, 2.7-3.6 V */
    m->rca[0] = 1;
    m->nrca = 0;   /* 0: no rca line in a script yet, the default above */
    return m;
}

void sdio_model_free(sdio_model *m)
{
    if (!m) return;
    if (cur == m) cur = NULL;
    for (int i = 0; i < m->nq; i++) free(m->q[i].b);
    free(m->q);
    free(m->fail);
    free(m->reg);
    wifi_fix_free(&m->log);
    free(m);
}

void sdio_model_use(sdio_model *m) { cur = m; }
const wifi_fix *sdio_model_log(const sdio_model *m) { return &m->log; }

static uint8_t *reg_at(const sdio_model *m, unsigned fn, uint32_t addr) { return &m->reg[fn * (FM1_SDIO_ADDR_MAX + 1u) + addr]; }

static mqueue *queue_at(sdio_model *m, unsigned fn, uint32_t addr, int make)
{
    for (int i = 0; i < m->nq; i++)
        if (m->q[i].fn == fn && m->q[i].addr == addr) return &m->q[i];
    if (!make) return NULL;
    m->q = xrealloc(m->q, (size_t)(m->nq + 1) * sizeof *m->q);
    mqueue *q = &m->q[m->nq++];
    memset(q, 0, sizeof *q);
    q->fn = fn;
    q->addr = addr;
    return q;
}

static void queue_byte(mqueue *q, uint8_t b)
{
    if (q->n % 64 == 0) q->b = xrealloc(q->b, q->n + 64);
    q->b[q->n++] = b;
}

/* The script: ocr, rca, reg, next, fail (sdio_model.h). reg and next take continuation lines. */
int sdio_model_script(sdio_model *m, const char *text, const char *name, FILE *err)
{
    size_t len = strlen(text);
    char *buf = xrealloc(NULL, len + 1);
    memcpy(buf, text, len + 1);
    int line = 0, rc = 0;
    int dkind = 0;          /* 1 reg, 2 next: what a continuation line feeds */
    unsigned dfn = 0;
    uint32_t daddr = 0;
    mqueue *dq = NULL;
    for (char *s = buf, *nl; s && *s && !rc; s = nl ? nl + 1 : NULL) {
        nl = strchr(s, '\n');
        if (nl) *nl = 0;
        line++;
        char *c = strchr(s, '#');
        if (c) *c = 0;
        char *p = s, *t;
        int cont = s[0] == ' ' || s[0] == '\t';
        if (!s[strspn(s, " \t")]) continue;
        uint32_t v;
        if (!cont) {
            t = tok(&p);
            dkind = 0;
            if (!strcmp(t, "ocr") || !strcmp(t, "rca")) {
                int is_ocr = t[0] == 'o';
                if (!(t = tok(&p)) || hex_u32(t, &v) || (!is_ocr && v > 0xFFFF) || tok(&p)) { rc = perr(err, name, line, "one hex value expected"); break; }
                if (is_ocr) m->ocr = v;
                else if (m->nrca == 16) { rc = perr(err, name, line, "at most 16 rca lines"); break; }
                else m->rca[m->nrca++] = v;
                continue;
            }
            if (!strcmp(t, "fail")) {
                mfail fl = { 0 };
                char *end;
                if (!(t = tok(&p)) || strncmp(t, "cmd", 3) || !isdigit((unsigned char)t[3]) ||
                    (fl.idx = (unsigned)strtoul(t + 3, &end, 10), *end)) { rc = perr(err, name, line, "fail needs a command"); break; }
                if (!(t = tok(&p))) { rc = perr(err, name, line, "fail needs an error"); break; }
                if (!fn_tok(t, &fl.fn)) {
                    fl.has_fn = 1;
                    if (!(t = tok(&p))) { rc = perr(err, name, line, "fail needs an error"); break; }
                    if (!hex_u32(t, &fl.addr)) {
                        fl.has_addr = 1;
                        if (!(t = tok(&p))) { rc = perr(err, name, line, "fail needs an error"); break; }
                    }
                }
                if (status_parse(t, &fl.status) || fl.status == FM1_WIFI_OK || tok(&p)) { rc = perr(err, name, line, "bad error name"); break; }
                m->fail = xrealloc(m->fail, (size_t)(m->nfail + 1) * sizeof *m->fail);
                m->fail[m->nfail++] = fl;
                continue;
            }
            if (strcmp(t, "reg") && strcmp(t, "next")) { rc = perr(err, name, line, "unknown script line"); break; }
            dkind = t[0] == 'r' ? 1 : 2;
            if (!(t = tok(&p)) || fn_tok(t, &dfn)) { rc = perr(err, name, line, "function f0-f7 expected"); break; }
            if (!(t = tok(&p)) || hex_u32(t, &daddr) || daddr > FM1_SDIO_ADDR_MAX) { rc = perr(err, name, line, "17-bit address expected"); break; }
            dq = dkind == 2 ? queue_at(m, dfn, daddr, 1) : NULL;
        } else {
            if (!dkind) { rc = perr(err, name, line, "a continuation line follows no reg or next line"); break; }
        }
        wifi_rec tmp = { 0 };
        while ((t = tok(&p)))
            if (data_tok(&tmp, t)) { rc = perr(err, name, line, "bad data byte"); break; }
        for (unsigned i = 0; !rc && i < tmp.n; i++) {
            if (tmp.any[i]) { rc = perr(err, name, line, "\"??\" belongs in expected fixtures, not scripts"); break; }
            if (dkind == 2) { queue_byte(dq, tmp.data[i]); continue; }
            if (daddr > FM1_SDIO_ADDR_MAX) { rc = perr(err, name, line, "reg data runs past the 17-bit address space"); break; }
            *reg_at(m, dfn, daddr++) = tmp.data[i];
        }
        free(tmp.data);
        free(tmp.any);
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
    if (!cur) { fputs("sdio_model: an SDIO command with no model in use (sdio_model_use)\n", stderr); exit(2); }
    return cur;
}

static int take_fail(sdio_model *m, unsigned idx, unsigned fn, uint32_t addr)
{
    for (int i = 0; i < m->nfail; i++) {
        mfail *f = &m->fail[i];
        if (f->used || f->idx != idx || (f->has_fn && f->fn != fn) || (f->has_addr && f->addr != addr)) continue;
        f->used = 1;
        return f->status;
    }
    return FM1_WIFI_OK;
}

static uint8_t fn_mask(const sdio_model *m) { return (uint8_t)(((1u << (FM1_SDIO_R4_NFN(m->ocr) + 1u)) - 1u) & ~1u); }

/* I/O Ready follows I/O Enable for each function the card has: the model's functions are ready at once. */
uint8_t sdio_model_peek(const sdio_model *m, unsigned fn, uint32_t addr)
{
    if (fn == 0 && addr == FM1_SDIO_CCCR_IO_READY) return *reg_at(m, 0, FM1_SDIO_CCCR_IO_ENABLE) & fn_mask(m);
    return *reg_at(m, fn, addr);
}

static int cccr_read_only(uint32_t a)
{
    return a == FM1_SDIO_CCCR_REV || a == FM1_SDIO_CCCR_SD_REV || a == FM1_SDIO_CCCR_IO_READY ||
           a == FM1_SDIO_CCCR_INT_PENDING || a == FM1_SDIO_CCCR_CAPS || (a >= FM1_SDIO_CCCR_CIS && a < FM1_SDIO_CCCR_CIS + 3u) ||
           a == FM1_SDIO_CCCR_EXEC || a == FM1_SDIO_CCCR_READY;
}

static void io_write(sdio_model *m, unsigned fn, uint32_t addr, uint8_t v)
{
    if (fn == 0 && cccr_read_only(addr)) return;
    *reg_at(m, fn, addr) = v;
}

static void io_read(sdio_model *m, unsigned fn, uint32_t addr, uint8_t *buf, unsigned len)
{
    mqueue *q = queue_at(m, fn, addr, 0);
    for (unsigned i = 0; i < len; i++)
        buf[i] = q && q->pos < q->n ? q->b[q->pos++] : sdio_model_peek(m, fn, addr + i);
}

/* ERANGE: the host would not send it. EBUS: the card refuses it (not selected, no such function). */
static int io_check(const sdio_model *m, unsigned fn, uint32_t addr, unsigned len, unsigned max)
{
    if (fn > FM1_SDIO_FN_MAX || addr > FM1_SDIO_ADDR_MAX || len == 0 || len > max || addr + len - 1u > FM1_SDIO_ADDR_MAX)
        return FM1_WIFI_ERANGE;
    if (m->state != ST_SELECTED || fn > FM1_SDIO_R4_NFN(m->ocr)) return FM1_WIFI_EBUS;
    return FM1_WIFI_OK;
}

int fm1_wifi_sdio_cmd(unsigned idx, uint32_t arg, uint32_t *resp)
{
    sdio_model *m = model();
    int st = take_fail(m, idx, 0, 0);
    uint32_t r = 0;
    if (st == FM1_WIFI_OK) {
        switch (idx) {
        case FM1_SDIO_CMD5:   /* R4; initialisation ends when C is set */
            r = m->ocr;
            if (m->state == ST_IDLE && (r & FM1_SDIO_R4_READY)) m->state = ST_READY;
            break;
        case FM1_SDIO_CMD3:   /* R6: a new RCA each time, card status bits 0 */
            if (m->state != ST_READY && m->state != ST_STBY) { st = FM1_WIFI_EBUS; break; }
            m->cur_rca = m->nrca ? m->rca[m->rca_pos < m->nrca ? m->rca_pos : m->nrca - 1] : m->rca[0];
            m->rca_pos++;
            r = m->cur_rca << 16;
            m->state = ST_STBY;
            break;
        case FM1_SDIO_CMD7:   /* selects the card with its RCA; any other RCA deselects it, without a response */
            if (m->state != ST_STBY && m->state != ST_SELECTED) { st = FM1_WIFI_EBUS; break; }
            if (m->cur_rca && arg >> 16 == m->cur_rca) m->state = ST_SELECTED;
            else { m->state = ST_STBY; st = FM1_WIFI_EBUS; }
            break;
        default:
            st = FM1_WIFI_EBUS;   /* not modelled */
        }
    }
    wifi_rec *rec = fix_add(&m->log);
    rec->kind = WIFI_REC_CMD;
    rec->idx = idx;
    rec->arg = arg;
    rec->resp = st == FM1_WIFI_OK ? r : 0;
    rec->status = st;
    if (st == FM1_WIFI_OK && resp) *resp = r;
    return st;
}

static int io_cmd(int kind, unsigned fn, uint32_t addr, int write, uint8_t *buf, unsigned len)
{
    sdio_model *m = model();
    int st = take_fail(m, kind == WIFI_REC_CMD52 ? 52u : 53u, fn, addr);
    if (st == FM1_WIFI_OK) st = io_check(m, fn, addr, len, kind == WIFI_REC_CMD52 ? 1u : CMD53_MAX);
    wifi_rec *rec = fix_add(&m->log);
    rec->kind = kind;
    rec->idx = kind == WIFI_REC_CMD52 ? 52u : 53u;
    rec->write = !!write;
    rec->fn = fn;
    rec->addr = addr;
    rec->status = st;
    if (write) {
        for (unsigned i = 0; i < len && i < CMD53_MAX; i++) {
            rec_byte(rec, buf[i], 0);
            if (st == FM1_WIFI_OK) io_write(m, fn, addr + i, buf[i]);
        }
    } else if (st == FM1_WIFI_OK) {
        io_read(m, fn, addr, buf, len);
        for (unsigned i = 0; i < len; i++) rec_byte(rec, buf[i], 0);
    }
    return st;
}

int fm1_wifi_cmd52(unsigned fn, uint32_t addr, int write, uint8_t *data) { return io_cmd(WIFI_REC_CMD52, fn, addr, write, data, 1); }

int fm1_wifi_cmd53(unsigned fn, uint32_t addr, int write, void *buf, unsigned len)
{
    return io_cmd(WIFI_REC_CMD53, fn, addr, write, buf, len);
}
