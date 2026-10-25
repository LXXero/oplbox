/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the WiFi register model (tests/wifi/sdio_model.c), its fixture format and diff, and the HAL's
 * fm1_wifi_sdio_enum / fm1_wifi_mac_rd / fm1_wifi_mac_wr running against it (built with -DFM1_WIFI_HOST
 * -DFM1_WIFI_SDIO_MODEL).
 *   wifi_model <dir> <out>        all checks; <dir> holds sdio_enum_card.txt and sdio_enum_expect.txt, and the
 *                                 enumeration's log is written to <out>/wifi_sdio_enum.txt
 *   wifi_model diff <want> <got>  the diff helper on two fixture files: exit 1 when they differ
 * Exits 1 when a check fails.
 */
#include <stdlib.h>
#include <string.h>
#include "fm1_wifi_regs.h"
#include "sdio_model.h"

/* The CPU-side accessors (FM1_WIFI_HOST): nothing the bus code does may touch them. */
static int n_mmio;
uint32_t fm1_wifi_host_rd(uint32_t addr) { (void)addr; n_mmio++; return 0; }
void fm1_wifi_host_wr(uint32_t addr, uint32_t v) { (void)addr; (void)v; n_mmio++; }

static int bad;
static void check(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

static FILE *quiet;

/* A scratch file under <out> instead of tmpfile(): glibc's tmpfile() always uses /tmp, and where /tmp is
 * read-only it returns NULL. Stops the test with a message when the file cannot be made either. */
static const char *scratch_dir = ".";
static FILE *scratch(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/wifi_model.tmp", scratch_dir);
    FILE *f = fopen(path, "w+");
    if (!f) { perror(path); fputs("FAIL: cannot open a scratch file under <out>\n", stderr); exit(1); }
    remove(path);                       /* stays usable until fclose; nothing is left behind */
    return f;
}

static sdio_model *card(const char *script)
{
    sdio_model *m = sdio_model_new();
    if (script && sdio_model_script(m, script, "script", stderr)) { check(0, "script parses"); }
    sdio_model_use(m);
    return m;
}

static int count_kind(const wifi_fix *f, int kind, unsigned idx)
{
    int n = 0;
    for (int i = 0; i < f->n; i++) n += f->r[i].kind == kind && (kind != WIFI_REC_CMD || f->r[i].idx == idx);
    return n;
}

static const wifi_rec *last_rec(const sdio_model *m) { const wifi_fix *f = sdio_model_log(m); return f->n ? &f->r[f->n - 1] : NULL; }

/* The diff of `want` (fixture text) against a run's log; -1 when the text does not parse. */
static int diff_text(const char *want_text, const wifi_fix *got, FILE *out)
{
    wifi_fix want = { 0 };
    if (wifi_fix_parse(&want, want_text, "want", stderr)) { wifi_fix_free(&want); return -1; }
    int nd = wifi_fix_diff(out, &want, got);
    wifi_fix_free(&want);
    return nd;
}

/* wl30_sdio_enum, replayed: the HAL's enumeration against the card script, diffed against the fixture. */
static void test_enum(const char *dir, const char *outdir)
{
    char card_path[512], want_path[512], out_path[512];
    snprintf(card_path, sizeof card_path, "%s/sdio_enum_card.txt", dir);
    snprintf(want_path, sizeof want_path, "%s/sdio_enum_expect.txt", dir);
    snprintf(out_path, sizeof out_path, "%s/wifi_sdio_enum.txt", outdir);
    sdio_model *m = sdio_model_new();
    check(sdio_model_load(m, card_path, stderr) == 0, "card script loads");
    sdio_model_use(m);
    check(fm1_wifi_sdio_enum() == FM1_WIFI_OK, "enumeration succeeds");
    FILE *o = fopen(out_path, "w");
    if (o) { wifi_fix_write(o, sdio_model_log(m)); fclose(o); }
    check(o != NULL, "enumeration log written");

    wifi_fix want = { 0 }, run = { 0 };
    check(wifi_fix_load(&want, want_path, stderr) == 0, "expected fixture loads");
    check(wifi_fix_load(&run, out_path, stderr) == 0, "the run's log reads back as a fixture");
    int nd = wifi_fix_diff(stderr, &want, &run);
    check(nd == 0, "enumeration matches tests/wifi/sdio_enum_expect.txt");
    check(wifi_fix_diff(quiet, sdio_model_log(m), &run) == 0, "log written and read back is the same");
    check(want.n == 12, "fixture has the 11 commands of docs/WIFI.md and the I/O Ready read");

    /* what the card holds afterwards */
    check(sdio_model_peek(m, 0, FM1_SDIO_CCCR_IO_ENABLE) == 0xFF, "I/O Enable 0xFF");
    check(sdio_model_peek(m, 0, FM1_SDIO_CCCR_IO_READY) == 0x02, "I/O Ready: function 1 only");
    check(sdio_model_peek(m, 0, FM1_SDIO_CCCR_BUS_IF) == FM1_SDIO_BUS_WIDTH_4, "4-bit bus");
    check(sdio_model_peek(m, 0, FM1_SDIO_CCCR_INT_ENABLE) == 0x03, "interrupts: master and function 1");
    for (uint32_t a = 0x10; a <= 0x13; a++) check(sdio_model_peek(m, 1, a) == 0, "function 1 0x10-0x13 cleared");
    check(n_mmio == 0, "the enumeration touches no CPU-side register");

    /* the diff catches a change against this run: one byte, a missing, an extra and a reordered record */
    char *text = NULL;
    {
        FILE *t = scratch();
        wifi_fix_write(t, &want);
        long n = ftell(t);
        rewind(t);
        text = malloc((size_t)n + 1);
        text[fread(text, 1, (size_t)n, t)] = 0;
        fclose(t);
    }
    char *mut = malloc(strlen(text) + 64), *p;
    strcpy(mut, text);
    check(diff_text(mut, &run, quiet) == 0, "the fixture written out still matches");
    p = strstr(mut, "0x00007 02");
    if (p) p[9] = '1';
    check(p && diff_text(mut, &run, quiet) == 1, "diff: one byte changed is one difference");
    strcpy(mut, text);
    p = strstr(mut, "cmd52 w f0 0x00002");
    if (p) p[6] = 'r';
    check(p && diff_text(mut, &run, quiet) == 1, "diff: a read where the run wrote is one difference");
    strcpy(mut, text);
    p = strstr(mut, "cmd52 w f1 0x00012 00\n");
    if (p) memmove(p, p + 22, strlen(p + 22) + 1);
    check(p && diff_text(mut, &run, quiet) == 1, "diff: a record missing from the fixture is one difference");
    strcpy(mut, text);
    p = strstr(mut, "cmd3 ");
    if (p) { memmove(p + 30, p, strlen(p) + 1); memcpy(p, "cmd52 r f0 0x00003 02        \n", 30); }
    check(p && diff_text(mut, &run, quiet) == 1, "diff: a record the run lacks is one difference");
    {
        FILE *t = scratch();
        strcpy(mut, text);
        p = strstr(mut, "0x00007 02");
        if (p) p[9] = '1';
        diff_text(mut, &run, t);
        long n = ftell(t);
        rewind(t);
        char *rep = malloc((size_t)n + 1);
        rep[fread(rep, 1, (size_t)n, t)] = 0;
        fclose(t);
        check(strstr(rep, "different:") && strstr(rep, "want line 7: cmd52 w f0 0x00007 01") &&
                  strstr(rep, "got  line 7: cmd52 w f0 0x00007 02"), "diff report names the line and both records");
        free(rep);
    }
    check(diff_text("cmd5 1f000000 -> ?\ncmd5 1f000000 -> 90ff8000\ncmd3 0 -> 00010000\ncmd7 00010000 -> 0\n"
                    "cmd52 w f0 00002 ??\ncmd52 r f0 00003 02\ncmd52 w f0 00007 02\ncmd52 w f0 00004 03\n"
                    "cmd52 w f1 00010 00\ncmd52 w f1 00011 00\ncmd52 w f1 00012 00\ncmd52 w f1 00013 00\n", &run, quiet) == 0,
          "\"??\" and \"-> ?\" match anything");
    free(mut);
    free(text);
    wifi_fix_free(&want);
    wifi_fix_free(&run);
    sdio_model_free(m);
}

/* The enumeration's error paths, each with the card the specification allows. */
static void test_enum_errors(void)
{
    sdio_model *m = card("ocr 0x10ff8000\n");   /* never ready */
    check(fm1_wifi_sdio_enum() == FM1_WIFI_ETIMEOUT, "a card that never reports C: ETIMEOUT");
    check(count_kind(sdio_model_log(m), WIFI_REC_CMD, 5) == 1 + (int)FM1_WIFI_CMD5_TRIES, "CMD5 bounded");
    check(count_kind(sdio_model_log(m), WIFI_REC_CMD52, 0) == 0, "no CMD52 to a card that is not ready");
    sdio_model_free(m);

    m = card("ocr 0x80ff8000\n");               /* no I/O function */
    check(fm1_wifi_sdio_enum() == FM1_WIFI_EBUS, "a card without function 1: EBUS");
    check(sdio_model_log(m)->n == 1, "... after the first CMD5");
    sdio_model_free(m);

    m = card("rca 0000\nrca 0002\n");           /* RCA 0 is reserved: ask again */
    check(fm1_wifi_sdio_enum() == FM1_WIFI_OK, "RCA 0 then 2: enumeration succeeds");
    check(count_kind(sdio_model_log(m), WIFI_REC_CMD, 3) == 2, "CMD3 sent again after RCA 0");
    check(diff_text("cmd5 1f000000 -> ?\ncmd5 1f000000 -> ?\ncmd3 0 -> 0\ncmd3 0 -> 00020000\ncmd7 00020000 -> ?\n",
                    &(wifi_fix){ sdio_model_log(m)->r, 5, 5 }, quiet) == 0, "CMD7 selects the RCA the card offered");
    sdio_model_free(m);

    m = card("next f0 0x00003 00 00 00 02\n");   /* function 1 ready on the 4th read */
    check(fm1_wifi_sdio_enum() == FM1_WIFI_OK, "I/O Ready delayed by 3 reads: enumeration succeeds");
    {
        const wifi_fix *lf = sdio_model_log(m);
        int reads = 0, first_f1 = -1, last_rd = -1;
        for (int i = 0; i < lf->n; i++) {
            const wifi_rec *q = &lf->r[i];
            if (q->kind == WIFI_REC_CMD52 && q->fn == 0 && q->addr == FM1_SDIO_CCCR_IO_READY && !q->write) { reads++; last_rd = i; }
            if (q->kind == WIFI_REC_CMD52 && q->fn == 1 && first_f1 < 0) first_f1 = i;
        }
        check(reads == 4, "I/O Ready read until function 1's bit is set");
        check(first_f1 > last_rd, "function 1 is not touched before it is ready");
    }
    sdio_model_free(m);

    char never[16 + 3 * FM1_WIFI_IO_READY_TRIES + 8] = "next f0 0x00003";   /* all zeros, then a late 02 */
    for (unsigned i = 0; i <= FM1_WIFI_IO_READY_TRIES; i++) strcat(never, i == FM1_WIFI_IO_READY_TRIES ? " 02\n" : " 00");
    m = card(never);
    check(fm1_wifi_sdio_enum() == FM1_WIFI_ETIMEOUT, "I/O Ready never set within the bound: ETIMEOUT");
    check(count_kind(sdio_model_log(m), WIFI_REC_CMD52, 0) == 1 + (int)FM1_WIFI_IO_READY_TRIES, "I/O Ready poll bounded");
    int f1 = 0;
    for (int i = 0; i < sdio_model_log(m)->n; i++) f1 += sdio_model_log(m)->r[i].kind == WIFI_REC_CMD52 && sdio_model_log(m)->r[i].fn == 1;
    check(f1 == 0, "no function 1 access when it never becomes ready");
    sdio_model_free(m);

    m = card("fail cmd52 f1 0x00011 ebus\n");
    check(fm1_wifi_sdio_enum() == FM1_WIFI_EBUS, "a failed CMD52: EBUS");
    const wifi_rec *r = last_rec(m);
    check(r && r->kind == WIFI_REC_CMD52 && r->addr == 0x11 && r->status == FM1_WIFI_EBUS, "stops at the failed write");
    sdio_model_free(m);

    m = card("fail cmd7 etimeout\n");
    check(fm1_wifi_sdio_enum() == FM1_WIFI_ETIMEOUT, "a failed CMD7 is passed on");
    check(count_kind(sdio_model_log(m), WIFI_REC_CMD52, 0) == 0, "no CMD52 after a failed CMD7");
    sdio_model_free(m);
}

/* The card's state and registers, as the specification describes them. */
static void test_model(void)
{
    uint8_t b = 0x77;
    uint32_t r = 0;
    sdio_model *m = card(NULL);
    check(fm1_wifi_cmd52(0, FM1_SDIO_CCCR_IO_ENABLE, 1, &b) == FM1_WIFI_EBUS, "CMD52 before CMD7: EBUS");
    check(fm1_wifi_sdio_cmd(FM1_SDIO_CMD3, 0, &r) == FM1_WIFI_EBUS, "CMD3 before CMD5: EBUS");
    check(fm1_wifi_sdio_cmd(FM1_SDIO_CMD5, 0, &r) == FM1_WIFI_OK && r == 0x90FF8000u, "CMD5 answers R4");
    check(fm1_wifi_sdio_cmd(FM1_SDIO_CMD3, 0, &r) == FM1_WIFI_OK && r == 0x00010000u, "CMD3 answers RCA 1");
    check(fm1_wifi_sdio_cmd(FM1_SDIO_CMD7, 0x00020000u, &r) == FM1_WIFI_EBUS, "CMD7 with another RCA: no response");
    check(fm1_wifi_cmd52(0, 0, 0, &b) == FM1_WIFI_EBUS, "... and the card stays deselected");
    check(fm1_wifi_sdio_cmd(FM1_SDIO_CMD7, 0x00010000u, &r) == FM1_WIFI_OK, "CMD7 with its RCA selects it");
    check(fm1_wifi_sdio_cmd(9, 0, &r) == FM1_WIFI_EBUS, "a command the model lacks: EBUS");
    b = 0x5A;
    check(fm1_wifi_cmd52(0, FM1_SDIO_CCCR_IO_READY, 1, &b) == FM1_WIFI_OK, "a write to I/O Ready is sent");
    b = 0x5A;
    check(fm1_wifi_cmd52(0, FM1_SDIO_CCCR_REV, 1, &b) == FM1_WIFI_OK, "a write to the CCCR revision is sent");
    check(fm1_wifi_cmd52(0, FM1_SDIO_CCCR_IO_READY, 0, &b) == FM1_WIFI_OK && b == 0, "... read-only: I/O Ready stays 0");
    check(fm1_wifi_cmd52(0, FM1_SDIO_CCCR_REV, 0, &b) == FM1_WIFI_OK && b == 0, "... read-only: revision stays 0");
    b = 0x06;
    fm1_wifi_cmd52(0, FM1_SDIO_CCCR_IO_ENABLE, 1, &b);
    check(fm1_wifi_cmd52(0, FM1_SDIO_CCCR_IO_READY, 0, &b) == FM1_WIFI_OK && b == 0x02,
          "I/O Ready follows I/O Enable, for the one function only");
    check(fm1_wifi_cmd52(2, 0, 0, &b) == FM1_WIFI_EBUS, "function 2 on a one-function card: EBUS");
    check(fm1_wifi_cmd52(8, 0, 0, &b) == FM1_WIFI_ERANGE, "function 8: ERANGE");
    check(fm1_wifi_cmd52(1, 0x20000, 0, &b) == FM1_WIFI_ERANGE, "address past 17 bits: ERANGE");
    uint8_t big[4] = { 0 };
    check(fm1_wifi_cmd53(1, 0x1FFFE, 0, big, 4) == FM1_WIFI_ERANGE, "CMD53 past the address space: ERANGE");
    check(fm1_wifi_cmd53(1, 0, 0, big, 0) == FM1_WIFI_ERANGE, "CMD53 of 0 bytes: ERANGE");
    const wifi_rec *lr = last_rec(m);
    check(lr && lr->kind == WIFI_REC_CMD53 && lr->status == FM1_WIFI_ERANGE && lr->n == 0, "refused reads are logged without data");
    sdio_model_free(m);

    /* the MAC accessors through the model: 4-byte CMD53 on function 1, little-endian, and a polled register */
    m = card("reg f1 0x01000 00 01 71 35          # MAC_CSR0, a made-up ASIC version\n"
             "next f1 0x0101c 00 00 02 00\n"
             "  00 00 02 00                        # BBP_CSR_CFG busy twice, then the register file's 0\n");
    check(fm1_wifi_sdio_enum() == FM1_WIFI_OK, "enumerated");
    check(fm1_wifi_mac_rd(MAC_CSR0, &r) == FM1_WIFI_OK && r == 0x35710100u, "mac_rd assembles little-endian");
    check(fm1_wifi_mac_wr(MAC_SYS_CTRL, 3) == FM1_WIFI_OK, "mac_wr");
    check(diff_text("cmd53 w f1 0x01004 03000000\n", &(wifi_fix){ (wifi_rec *)last_rec(m), 1, 1 }, quiet) == 0,
          "mac_wr is one CMD53 write of 4 bytes, low byte first");
    check(fm1_wifi_mac_rd(MAC_SYS_CTRL, &r) == FM1_WIFI_OK && r == 3, "the register file keeps what was written");
    uint32_t busy[3];
    for (int i = 0; i < 3; i++) fm1_wifi_mac_rd(BBP_CSR_CFG, &busy[i]);
    check(busy[0] == BBP_CSR_CFG_BUSY && busy[1] == BBP_CSR_CFG_BUSY && busy[2] == 0, "queued answers first, then the register file");
    check(n_mmio == 0, "no CPU-side register touched");
    sdio_model_free(m);
}

static void test_format(void)
{
    const char *txt =
        "# a comment\n"
        "\n"
        "cmd5 0x1f000000 -> 0x90ff8000   # trailing comment\n"
        "cmd7 00020000 ! ebus\n"
        "cmd52 r f2 0x00000 ! ebus\n"
        "cmd53 r f1 0x0000c 0011223344556677 8899aabbccddeeff\n"
        "  0102 ??\n"
        "cmd53 w f1 0x00000 01 02 ! ebus\n"
        "frame rx ch=6 rssi=-42 80 00 00 00 ff ff ff ff ff ff\r\n"
        "\t02 00 00 00 00 01\n"
        "frame tx 40 00\n";
    wifi_fix f = { 0 };
    check(wifi_fix_parse(&f, txt, "format", stderr) == 0 && f.n == 7, "fixture parses: 7 records");
    if (f.n == 7) {
        check(f.r[0].kind == WIFI_REC_CMD && f.r[0].idx == 5 && f.r[0].arg == 0x1F000000u && f.r[0].resp == 0x90FF8000u &&
                  f.r[0].line == 3, "command record");
        check(f.r[1].status == FM1_WIFI_EBUS && f.r[2].status == FM1_WIFI_EBUS && f.r[2].n == 0, "error records");
        check(f.r[3].n == 19 && f.r[3].data[15] == 0xFF && f.r[3].data[17] == 0x02 && f.r[3].any[18], "CMD53 with a continuation line");
        check(f.r[4].write && f.r[4].n == 2 && f.r[4].status == FM1_WIFI_EBUS, "failed write keeps its data");
        check(f.r[5].kind == WIFI_REC_FRAME && !f.r[5].write && !strcmp(f.r[5].attr, "ch=6 rssi=-42") && f.r[5].n == 16 &&
                  f.r[5].data[0] == 0x80 && f.r[5].data[15] == 0x01, "rx frame with attributes, CRLF and a tab continuation");
        check(f.r[6].kind == WIFI_REC_FRAME && f.r[6].write && f.r[6].n == 2, "tx frame");
        FILE *t = scratch();
        wifi_fix_write(t, &f);
        long n = ftell(t);
        rewind(t);
        char *s = malloc((size_t)n + 1);
        s[fread(s, 1, (size_t)n, t)] = 0;
        fclose(t);
        check(diff_text(s, &f, quiet) == 0, "written and read back: the same records");
        free(s);
        wifi_fix g = { 0 };
        wifi_fix_parse(&g, "frame rx ch=1 80 00 00 00 ff ff ff ff ff ff\n  02 00 00 00 00 01\nframe tx 40 00\n", "g", stderr);
        check(wifi_fix_diff(quiet, &(wifi_fix){ f.r + 5, 2, 2 }, &g) == 1, "frames: a different attribute differs");
        wifi_fix_free(&g);
    }
    wifi_fix_free(&f);

    static const char *const badtxt[] = {
        "cmd52 x f0 0x00002 ff\n",            /* neither r nor w */
        "cmd52 w f8 0x00002 ff\n",            /* function 8 */
        "cmd52 w f0 0x20000 ff\n",            /* address past 17 bits */
        "cmd52 w f0 0x00002 ff 00\n",         /* CMD52 with two bytes */
        "cmd52 w f0 0x00002\n",               /* ... with none */
        "cmd53 w f1 0x00000 0\n",             /* odd hex */
        "cmd53 r f1 0x00000 00 ! ebus\n",     /* a failed read with data */
        "cmd5 0x1f000000\n",                  /* no response */
        "cmd5 0x1f000000 -> 0x1 0x2\n",       /* trailing token */
        "cmd5 0x1f000000\n  00\n",            /* continuation after a command record */
        "  00\n",                             /* continuation first */
        "cmd52 w f0 0x00002 ff ! eweird\n",   /* unknown error */
        "cmd53 w f1 0x00000 00 ! ebus 01\n",  /* data after the error */
        "frame xx 00\n",                      /* neither rx nor tx */
        "frame rx ch=1\n",                    /* frame without data */
        "mmio w 0x30f04 1\n",                 /* unknown record */
        "cm\n",                               /* short token */
        "cmd\n",
    };
    for (size_t i = 0; i < sizeof badtxt / sizeof *badtxt; i++) {
        wifi_fix b = { 0 };
        char what[96];
        snprintf(what, sizeof what, "bad fixture %zu refused: %.40s", i, badtxt[i]);
        check(wifi_fix_parse(&b, badtxt[i], "bad", quiet) == -1, what);
        wifi_fix_free(&b);
    }
    /* the error names the line */
    {
        FILE *t = scratch();
        wifi_fix b = { 0 };
        wifi_fix_parse(&b, "cmd5 0 -> 0\n\n# x\ncmd52 w f0 0x00002 ff 00\n", "named.txt", t);
        wifi_fix_free(&b);
        rewind(t);
        char line[128] = "";
        check(fgets(line, sizeof line, t) && !strncmp(line, "named.txt:4: ", 13), "a parse error names file and line");
        fclose(t);
    }
    static const char *const badscript[] = {
        "ocr\n", "rca 0x10000\n", "reg f0\n", "reg f0 0x1ffff 00 00\n", "next f1 0x0101c ??\n",
        "fail cmd52 f1 0x10\n", "fail cmd52 ok\n", "bogus 1\n", "  00\n",
    };
    for (size_t i = 0; i < sizeof badscript / sizeof *badscript; i++) {
        sdio_model *m = sdio_model_new();
        char what[96];
        snprintf(what, sizeof what, "bad script %zu refused: %.40s", i, badscript[i]);
        check(sdio_model_script(m, badscript[i], "bad", quiet) == -1, what);
        sdio_model_free(m);
    }
}

static int diff_files(const char *want_path, const char *got_path)
{
    wifi_fix want = { 0 }, got = { 0 };
    if (wifi_fix_load(&want, want_path, stderr) || wifi_fix_load(&got, got_path, stderr)) return 2;
    int nd = wifi_fix_diff(stdout, &want, &got);
    printf("%d difference%s\n", nd, nd == 1 ? "" : "s");
    wifi_fix_free(&want);
    wifi_fix_free(&got);
    return nd ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "diff")) return diff_files(argv[2], argv[3]);
    if (argc != 3) { fputs("usage: wifi_model <dir> <out> | wifi_model diff <want> <got>\n", stderr); return 2; }
    scratch_dir = argv[2];
    quiet = fopen("/dev/null", "w");
    if (!quiet) quiet = scratch();
    test_enum(argv[1], argv[2]);
    test_enum_errors();
    test_model();
    test_format();
    fclose(quiet);
    if (bad) return 1;
    printf("wifi_model: register model, fixture format, diff and the wl30_sdio_enum replay pass\n");
    return 0;
}
