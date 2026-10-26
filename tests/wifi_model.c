/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of the fm1wifi library (firmware/wifi/) against the register model (tests/wifi/sdio_model.c, its
 * port on the host), and of the model's fixture format and diff. Built from firmware/wifi/ and tests/wifi/
 * only, so a dependency on OPLBOX or the bsp fails to compile here.
 *   wifi_model <dir> <out>        all checks; <dir> holds core_model.txt, bringup_expect.txt and
 *                                 diag_expect.txt, <out> radio_trace.txt (tests/run_host.sh decompresses
 *                                 tests/wifi/radio_trace.txt.gz there); the runs' logs are written to
 *                                 <out>/wifi_bringup.txt, <out>/wifi_diag.txt and <out>/wifi_radio.txt
 *   wifi_model diff <want> <got>  the diff helper on two fixture files: exit 1 when they differ
 * Exits 1 when a check fails.
 */
#include <stdlib.h>
#include <string.h>
#include "fm1wifi.h"
#include "sdio_model.h"

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

static sdio_model *core(const char *path, const char *script)
{
    sdio_model *m = sdio_model_new();
    if (path && sdio_model_load(m, path, stderr)) check(0, "core script loads");
    if (script && sdio_model_script(m, script, "script", stderr)) check(0, "script parses");
    sdio_model_use(m);
    return m;
}

static int count_kind(const wifi_fix *f, int kind, uint32_t addr)
{
    int n = 0;
    for (int i = 0; i < f->n; i++) n += f->r[i].kind == kind && (kind == WIFI_REC_DELAY || f->r[i].addr == addr);
    return n;
}

/* The diff of `want` (fixture text) against a run's log; -1 when the text does not parse. */
static int diff_text(const char *want_text, const wifi_fix *got, FILE *out)
{
    wifi_fix want = { 0 };
    if (wifi_fix_parse(&want, want_text, "want", stderr)) { wifi_fix_free(&want); return -1; }
    int nd = wifi_fix_diff(out, &want, got);
    wifi_fix_free(&want);
    return nd;
}

/* The log since record `from`, as a fixture of its own */
static wifi_fix tail(const sdio_model *m, int from) { const wifi_fix *f = sdio_model_log(m); return (wifi_fix){ f->r + from, f->n - from, f->n - from }; }

/* A run diffed against an expected fixture file, and written to <out>/name so it can be read */
static void against(const char *dir, const char *outdir, const char *expect, const char *name, const wifi_fix *run, const char *what)
{
    char want_path[512], out_path[512];
    wifi_fix want = { 0 }, back = { 0 };
    snprintf(want_path, sizeof want_path, "%s/%s", dir, expect);
    snprintf(out_path, sizeof out_path, "%s/%s", outdir, name);
    FILE *o = fopen(out_path, "w");
    check(o != NULL, "a run's log written");
    if (o) { wifi_fix_write(o, run); fclose(o); }
    check(wifi_fix_load(&want, want_path, stderr) == 0, "an expected fixture loads");
    check(wifi_fix_load(&back, out_path, stderr) == 0, "the run's log reads back as a fixture");
    int nd = wifi_fix_diff(stderr, &want, run);
    check(nd == 0, what);
    check(wifi_fix_diff(quiet, run, &back) == 0, "log written and read back is the same");
    wifi_fix_free(&want);
    wifi_fix_free(&back);
}

/* Stage 2's bring-up and first reads, replayed against core_model.txt and diffed against the fixtures written
 * by hand from docs/WIFI.md; then power-down puts every register back. */
static void test_bringup(const char *dir, const char *outdir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    sdio_model *m = core(path, NULL);
    uint32_t before[12];
    static const uint32_t regs[12] = { 0x14040, 0x14044, 0x14048, 0x1404c, 0x14050, 0x1405c, 0x30f00, 0x30f04,
                                       0x14054, 0x14058, 0x14060, 0x14064 };
    for (int i = 0; i < 12; i++) before[i] = sdio_model_peek(m, regs[i]);

    uint32_t v = 0xDEADBEEFu;
    uint8_t b = 0;
    check(fm1wifi_mac_rd(MAC_CSR0, &v) == FM1WIFI_ESTATE && v == 0xDEADBEEFu, "MAC read before power-up: OFF");
    check(fm1wifi_bbp_rd(0, &b) == FM1WIFI_ESTATE, "BBP read before power-up: OFF");
    check(sdio_model_log(m)->n == 0, "... and neither touched the core");

    check(fm1wifi_power_up() == FM1WIFI_OK && fm1wifi_is_up(), "power-up");
    against(dir, outdir, "bringup_expect.txt", "wifi_bringup.txt", sdio_model_log(m),
            "power-up matches tests/wifi/bringup_expect.txt");
    check(count_kind(sdio_model_log(m), WIFI_REC_DELAY, 0) == 4, "four delays: 1, 1, 1 and 50 us");
    check(sdio_model_clk(m) == 0, "CLK_CON1's WiFi bits cleared");
    check(sdio_model_peek(m, 0x14040) == 0x14F && sdio_model_peek(m, 0x1405c) == 0x47000005, "WF_CON0 and WF_CON7 as listed");
    check(fm1wifi_power_up() == FM1WIFI_ESTATE, "a second power-up is refused");

    int at = sdio_model_log(m)->n;
    fm1wifi_diag d;
    check(fm1wifi_diag_read(&d) == FM1WIFI_OK && d.err == FM1WIFI_OK && d.bbp_err == FM1WIFI_OK, "diagnostic read: OK");
    check(d.mac_csr0 == 0x35920200u && d.mac_sys_ctrl == 0x0Cu && d.bbp0 == 0x21, "MAC_CSR0, MAC_SYS_CTRL, BBP 0 as the model holds");
    wifi_fix t = tail(m, at);
    against(dir, outdir, "diag_expect.txt", "wifi_diag.txt", &t, "diagnostic reads match tests/wifi/diag_expect.txt");

    check(fm1wifi_bbp_wr(0x42, 0x99) == FM1WIFI_OK && sdio_model_bbp(m, 0x42) == 0x99, "BBP write reaches the baseband");
    check(fm1wifi_bbp_rd(0x42, &b) == FM1WIFI_OK && b == 0x99, "... and reads back");
    check(fm1wifi_mac_wr(MAC_SYS_CTRL, 3) == FM1WIFI_OK && sdio_model_peek(m, 0x31004) == 3, "MAC write at 0x30000 + address");
    check(fm1wifi_mac_rd(0x1001, &v) == FM1WIFI_ERANGE, "unaligned MAC address: RANGE");
    check(fm1wifi_mac_rd(0x10000, &v) == FM1WIFI_ERANGE, "MAC address past the window: RANGE");
    check(fm1wifi_mac_rd(FM1_WF_RX_FIFO, &v) == FM1WIFI_ENOTYET && fm1wifi_mac_wr(FM1_WF_TX_FIFO, 0) == FM1WIFI_ENOTYET,
          "the frame FIFOs: NOT YET (DMA, stages 4-5)");

    fm1wifi_power_down();
    check(!fm1wifi_is_up(), "powered down");
    for (int i = 0; i < 12; i++) {
        char what[64];
        snprintf(what, sizeof what, "power-down restores 0x%05x", (unsigned)regs[i]);
        check(sdio_model_peek(m, regs[i]) == before[i], what);
    }
    check(sdio_model_clk(m) == 0xC000u, "power-down restores CLK_CON1's WiFi bits");
    check(fm1wifi_mac_rd(MAC_CSR0, &v) == FM1WIFI_ESTATE, "MAC read after power-down: OFF");
    at = sdio_model_log(m)->n;
    fm1wifi_power_down();
    check(sdio_model_log(m)->n == at, "a second power-down does nothing");
    check(sdio_model_bad(m) == 0, "no access outside the WiFi blocks");
    sdio_model_free(m);
}

/* The bounded waits and the cores that answer nothing: an error, never a hang. */
static void test_errors(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    fm1wifi_diag d;

    /* BBP_CSR_CFG's BUSY never clears */
    sdio_model *m = core(path, "stuck 0x3101c 0x00020000\n");
    check(fm1wifi_diag_read(&d) == FM1WIFI_ETIMEOUT && d.bbp_err == FM1WIFI_ETIMEOUT, "BUSY stuck: TIMEOUT");
    check(d.mac_csr0 == 0x35920200u, "... the MAC reads before it still done");
    check(count_kind(sdio_model_log(m), WIFI_REC_RD, 0x3101c) == (int)FM1WIFI_BBP_TRIES, "BBP poll bounded");
    check(count_kind(sdio_model_log(m), WIFI_REC_WR, 0x3101c) == 0, "no request while BUSY never cleared");
    fm1wifi_power_down();
    sdio_model_free(m);

    /* BUSY clears for the first wait, then never again */
    char late[64 + 12 * FM1WIFI_BBP_TRIES];
    strcpy(late, "next 0x3101c 0x0");
    m = core(path, NULL);
    for (unsigned i = 0; i < FM1WIFI_BBP_TRIES + 1u; i++) strcat(late, " 0x20000");
    strcat(late, "\n");
    if (sdio_model_script(m, late, "late", stderr)) check(0, "late script parses");
    uint8_t b;
    check(fm1wifi_power_up() == FM1WIFI_OK && fm1wifi_bbp_rd(0, &b) == FM1WIFI_ETIMEOUT, "BUSY stuck after the request: TIMEOUT");
    check(count_kind(sdio_model_log(m), WIFI_REC_WR, 0x3101c) == 2, "... after the request and its BUSY write");
    check(count_kind(sdio_model_log(m), WIFI_REC_RD, 0x3101c) == 1 + (int)FM1WIFI_BBP_TRIES, "... the second wait bounded too");
    fm1wifi_power_down();
    sdio_model_free(m);

    /* a core that answers nothing: MAC_CSR0 reads 0 (no reg line), or all ones */
    m = core(NULL, NULL);
    check(fm1wifi_diag_read(&d) == FM1WIFI_EBUS && d.mac_csr0 == 0, "MAC_CSR0 0: NO ANSWER");
    fm1wifi_power_down();
    sdio_model_free(m);
    m = core(NULL, "reg 0x31000 0xffffffff\nstuck 0x3101c 0xffffffff\n");
    check(fm1wifi_diag_read(&d) == FM1WIFI_EBUS && d.bbp_err == FM1WIFI_ETIMEOUT, "all ones: NO ANSWER, BBP TIMEOUT");
    fm1wifi_power_down();
    check(sdio_model_bad(m) == 0, "no access outside the WiFi blocks");
    sdio_model_free(m);

    check(!strcmp(fm1wifi_strerror(FM1WIFI_ETIMEOUT), "TIMEOUT") && !strcmp(fm1wifi_strerror(FM1WIFI_EBUS), "NO ANSWER") &&
              !strcmp(fm1wifi_strerror(FM1WIFI_ESTATE), "OFF") && !strcmp(fm1wifi_strerror(7), "?"), "error names");
}

/* The CPU-side accessors bound their index; the WL30 block takes only its six documented offsets. */
static void test_accessors(void)
{
    sdio_model *m = core(NULL, NULL);
    fm1wifi_wla_wr(31, 1);
    fm1wifi_wla_wr(40, 1);
    fm1wifi_wl_wr(8, 1);
    fm1wifi_wl_anl_wr(4, 1);
    fm1wifi_wf_wr(10, 1);
    fm1wifi_wl30_wr(0x08, 1);
    fm1wifi_wl30_wr(0x20, 1);
    check(fm1wifi_wla_rd(40) == 0 && fm1wifi_wl30_rd(0x0C) == 0, "out-of-range reads give 0");
    check(sdio_model_log(m)->n == 0, "out-of-range indexes and offsets touch nothing");
    fm1wifi_wf_wr(9, 0x77);
    fm1wifi_wl30_wr(0x1C, 0x55);
    check(sdio_model_peek(m, 0x14064) == 0x77 && sdio_model_peek(m, 0x30f1c) == 0x55, "WF_CON9 at 0x14064, WL30 0x1C at 0x30f1c");
    check(sdio_model_bad(m) == 0, "no access outside the WiFi blocks");
    sdio_model_free(m);
}

/* The RAM pattern check: a byte the "hardware" changed is counted, and the first one found. */
static void test_pattern(void)
{
    static uint32_t ram[256];
    uint8_t *p = (uint8_t *)ram;
    fm1wifi_pattern_fill(p + 1, sizeof ram - 2);   /* rounded inward: words 1..254 */
    fm1wifi_ramcheck c = fm1wifi_pattern_check(p + 1, sizeof ram - 2);
    check(c.changed == 0 && c.first == 0, "untouched: nothing changed");
    check(ram[0] == 0 && ram[255] == 0, "the fill keeps to whole words inside the region");
    check(ram[1] != ram[2], "the pattern depends on the address");
    p[100] ^= 1; p[513] = (uint8_t)~p[513]; ram[200] = ~ram[200];
    c = fm1wifi_pattern_check(p + 1, sizeof ram - 2);
    check(c.changed == 6 && c.first == (uintptr_t)(p + 100), "three places changed: 6 bytes, the first at its address");
    c = fm1wifi_pattern_check(p, 0);
    check(c.changed == 0, "an empty region");
}

static void test_format(void)
{
    const char *txt =
        "# a comment\n"
        "\n"
        "wr32 0x14040 0x00000100   # trailing comment\n"
        "rd32 31000 -> 35920200\n"
        "rd32 0x3101c -> ?\n"
        "clk 0x0000 -> 0xc000\n"
        "delay 50\n"
        "frame rx ch=6 rssi=-42 80 00 00 00 ff ff ff ff ff ff\r\n"
        "\t02 00 00 00 00 01 ??\n"
        "frame tx 40 00\n";
    {
        wifi_fix w = { 0 };
        check(wifi_fix_parse(&w, "bbp 0x16 0x00\nwin txbe 0x5c 0x10\nwin rxfe 0x17 0x82\n", "w", stderr) == 0 && w.n == 3 &&
                  w.r[0].kind == WIFI_REC_BBP && w.r[0].addr == 0x16 && w.r[1].kind == WIFI_REC_WIN && w.r[1].addr == 0x25c &&
                  w.r[1].val == 0x10 && w.r[2].addr == 0x317, "bbp and win records parse");
        check(diff_text("bbp 0x16 0x00\nwin txbe 0x5c 0x11\nwin rxfe 0x17 0x82\n", &w, quiet) == 1, "diff: a window value differs");
        check(diff_text("bbp 0x16 0x00\nwin cca 0x5c 0x10\nwin rxfe 0x17 0x82\n", &w, quiet) == 1, "diff: another window differs");
        wifi_fix_free(&w);
    }
    wifi_fix f = { 0 };
    check(wifi_fix_parse(&f, txt, "format", stderr) == 0 && f.n == 7, "fixture parses: 7 records");
    if (f.n == 7) {
        check(f.r[0].kind == WIFI_REC_WR && f.r[0].addr == 0x14040 && f.r[0].val == 0x100 && f.r[0].line == 3, "write record");
        check(f.r[1].kind == WIFI_REC_RD && f.r[1].addr == 0x31000 && f.r[1].val == 0x35920200u && !f.r[1].any, "read record");
        check(f.r[2].any, "\"-> ?\" matches any value");
        check(f.r[3].kind == WIFI_REC_CLK && f.r[3].addr == 0 && f.r[3].val == 0xC000, "clk record");
        check(f.r[4].kind == WIFI_REC_DELAY && f.r[4].val == 50, "delay record, decimal");
        check(f.r[5].kind == WIFI_REC_FRAME && !f.r[5].write && !strcmp(f.r[5].attr, "ch=6 rssi=-42") && f.r[5].n == 17 &&
                  f.r[5].data[0] == 0x80 && f.r[5].data[15] == 0x01 && f.r[5].bany[16], "rx frame with attributes, CRLF and a tab continuation");
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

        wifi_fix got = { 0 };
        wifi_fix_parse(&got, "wr32 0x14040 0x100\nrd32 0x31000 -> 0x35920200\nrd32 0x3101c -> 0x9\nclk 0 -> 0xc000\ndelay 50\n", "got", stderr);
        wifi_fix head = { f.r, 5, 5 };
        check(wifi_fix_diff(quiet, &head, &got) == 0, "diff: a matching run");
        check(diff_text("wr32 0x14040 0x101\nrd32 0x31000 -> 0x35920200\nrd32 0x3101c -> ?\nclk 0 -> 0xc000\ndelay 50\n", &got, quiet) == 1,
              "diff: one value changed is one difference");
        check(diff_text("rd32 0x31000 -> 0x35920200\nrd32 0x3101c -> ?\nclk 0 -> 0xc000\ndelay 50\n", &got, quiet) == 1,
              "diff: a record missing from the fixture is one difference");
        check(diff_text("wr32 0x14040 0x100\ndelay 1\nrd32 0x31000 -> 0x35920200\nrd32 0x3101c -> ?\nclk 0 -> 0xc000\ndelay 50\n", &got, quiet) == 1,
              "diff: a record the run lacks is one difference");
        check(diff_text("rd32 0x14040 -> 0x100\nrd32 0x31000 -> 0x35920200\nrd32 0x3101c -> ?\nclk 0 -> 0xc000\ndelay 50\n", &got, quiet) == 1,
              "diff: a read where the run wrote is one difference");
        FILE *r = scratch();
        diff_text("wr32 0x14040 0x100\nrd32 0x31000 -> 0x1\n", &got, r);
        rewind(r);
        char rep[1024];
        rep[fread(rep, 1, sizeof rep - 1, r)] = 0;
        fclose(r);
        check(strstr(rep, "different:") && strstr(rep, "want line 2: rd32 0x31000 -> 0x00000001") &&
                  strstr(rep, "got  line 2: rd32 0x31000 -> 0x35920200"), "diff report names the line and both records");
        wifi_fix_free(&got);
        wifi_fix g = { 0 };
        wifi_fix_parse(&g, "frame rx ch=1 80 00 00 00 ff ff ff ff ff ff\n  02 00 00 00 00 01 00\nframe tx 40 00\n", "g", stderr);
        check(wifi_fix_diff(quiet, &(wifi_fix){ f.r + 5, 2, 2 }, &g) == 1, "frames: a different attribute differs");
        wifi_fix_free(&g);
    }
    wifi_fix_free(&f);

    static const char *const badtxt[] = {
        "wr32 0x14040\n",                     /* no value */
        "wr32 0x14040 1 2\n",                 /* two values */
        "rd32 0x31000\n",                     /* no "->" */
        "rd32 0x31000 -> \n",                 /* no value after it */
        "rd32 0x31000 -> 1 2\n",              /* trailing token */
        "rd32 0x31000 = 1\n",                 /* not "->" */
        "rd32 zz -> 1\n",                     /* bad address */
        "wr32 0x123456789 0\n",               /* address past 32 bits */
        "delay\n", "delay 0x10\n", "delay 5 6\n",
        "clk -> 1\n",
        "wr32 0x14040 1\n  00\n",             /* continuation after a register record */
        "  00\n",                             /* continuation first */
        "frame xx 00\n",                      /* neither rx nor tx */
        "frame rx ch=1\n",                    /* frame without data */
        "frame rx 0\n",                       /* odd hex */
        "cmd52 w f0 0x00002 ff\n",            /* the SDIO command records are gone */
        "bbp 0x100 0\n", "bbp 1\n", "win dma 0x10 0x1\n", "win cca 0x100 0\n", "win cca 0x10\n",
        "bogus\n",
    };
    for (size_t i = 0; i < sizeof badtxt / sizeof *badtxt; i++) {
        wifi_fix b = { 0 };
        char what[96];
        snprintf(what, sizeof what, "bad fixture %zu refused: %.40s", i, badtxt[i]);
        check(wifi_fix_parse(&b, badtxt[i], "bad", quiet) == -1, what);
        wifi_fix_free(&b);
    }
    {
        FILE *t = scratch();
        wifi_fix b = { 0 };
        wifi_fix_parse(&b, "wr32 0 0\n\n# x\nrd32 0x31000\n", "named.txt", t);
        wifi_fix_free(&b);
        rewind(t);
        char line[128] = "";
        check(fgets(line, sizeof line, t) && !strncmp(line, "named.txt:4: ", 13), "a parse error names file and line");
        fclose(t);
    }
    static const char *const badscript[] = {
        "reg\n", "reg 0x31002 1\n", "reg 0x31000\n", "reg 0x31000 1 2\n", "next 0x3101c\n", "stuck 0x3101c 1 2\n",
        "bbp 0x100 1\n", "bbp 0 0x100\n", "clk 0x1\n", "clk\n", "bogus 1\n", "next 0x3101c zz\n",
        "hang\n", "hang dma\n", "hang tune x\n", "reg 0x20000 1\n",
    };
    for (size_t i = 0; i < sizeof badscript / sizeof *badscript; i++) {
        sdio_model *m = sdio_model_new();
        char what[96];
        snprintf(what, sizeof what, "bad script %zu refused: %.40s", i, badscript[i]);
        check(sdio_model_script(m, badscript[i], "bad", quiet) == -1, what);
        sdio_model_free(m);
    }
    /* the model's own behaviour: queued answers first, then the register; stuck wins over writes */
    sdio_model *m = core(NULL, "reg 0x14040 7\nnext 0x14040 1 2\nstuck 0x14044 9\n");
    check(fm1wifi_wf_rd(0) == 1 && fm1wifi_wf_rd(0) == 2 && fm1wifi_wf_rd(0) == 7, "next answers, then the register");
    fm1wifi_wf_wr(1, 3);
    check(fm1wifi_wf_rd(1) == 9 && sdio_model_peek(m, 0x14044) == 3, "stuck answers whatever was written");
    sdio_model_free(m);
}

/* ---- stage 3: the radio (docs/WIFI_STAGE3.md) */

#define N_CPU 80u
static uint32_t cpu_reg(unsigned i) { return i < 40u ? FM1_WLA_CON(i) : FM1_JL_WL_BASE + 4u * (i - 40u); }

static void test_radio(const char *dir, const char *outdir)
{
    char path[512];
    uint32_t before[N_CPU];
    wifi_fix want = { 0 };
    fm1wifi_radio r;
    uint8_t peak = 0;
    unsigned i;
    snprintf(path, sizeof path, "%s/radio_trace.txt", outdir);
    check(wifi_fix_load(&want, path, stderr) == 0 && want.n > 30000, "the radio trace (tools/wifi_ir.py --trace) loads");
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    sdio_model *m = core(path, NULL);
    sdio_model_raw_log(m, 0);
    for (i = 0; i < N_CPU; i++) before[i] = sdio_model_peek(m, cpu_reg(i));
    check(fm1wifi_set_channel(1) == FM1WIFI_ESTATE && fm1wifi_adc_peak(&peak) == FM1WIFI_ESTATE, "channel and ADC before the radio: OFF");
    check(fm1wifi_power_up() == FM1WIFI_OK, "power-up before the radio");
    sdio_model_zero(m);                 /* tools/wifi_ir.py's run starts from zeroed registers: so does this one */
    sdio_model_script(m, "reg 0x31000 0x00300100\n", "csr0", stderr);
    sdio_model_wlog_clear(m);
    check(fm1wifi_radio_up(NULL, &r) == FM1WIFI_OK && r.err == FM1WIFI_OK && !strcmp(r.stage, "DONE") && fm1wifi_radio_is_up(),
          "radio up: OK");
    snprintf(path, sizeof path, "%s/wifi_radio.txt", outdir);
    FILE *o = fopen(path, "w");
    if (o) { wifi_fix_write(o, sdio_model_wlog(m)); fclose(o); }
    check(wifi_fix_diff(stderr, &want, sdio_model_wlog(m)) == 0,
          "the radio's register, BBP and window writes and delays match JieLi's (radio_trace.txt), record for record");
    wifi_fix_free(&want);
    check(r.mac_sys_ctrl == 0x0Cu && r.bbp0 == 0, "MAC_SYS_CTRL and BBP 0 read after the resets are released");
    check(r.pll_mid == 95 && r.pll_bank == 64, "PLL: middle bank 95, bank 64 for divider 121 (the model's VCO)");
    check(r.bw[0] == 141 && r.bw[1] == 80 && r.bw[2] == 44, "filter codes 141, 80, 44");
    check(r.txlo[0] == 57 && r.txlo[1] == 41 && r.tx_dc[0] == -1 && r.tx_dc[1] == 0, "TX LO 57/41, TX DC -1/0");
    check(r.tx_gain == 5 && r.tx_phase == 2, "TX gain imbalance 5, phase 2 on Q");
    check(r.rx_gain == -20 && r.rx_phase == 7, "RX gain imbalance -20, phase 7 on I");
    check(r.rx_dc[0][0] == (0x200 | 123) && r.rx_dc[0][1] == 77 && r.rx_dc[1][0] == (0x200 | 103) && r.rx_dc[3][1] == 21,
          "RX DC codes per gain range");
    check(r.channel == 6 && r.power == 6 && (sdio_model_peek(m, FM1_WF_CON(7)) & 0x00FF00FFu) == 0x00060000u + 107u,
          "channel 6 (WF_CON7 bits 7-0 = 107), TX power 6");
    check(r.got == FM1WIFI_GOT_ALL, "every value marked read (got)");
    check(sdio_model_trims(m) == 1 && !sdio_model_in_trim(m), "the port told before the trim and after it, once");
    check((sdio_model_peek(m, FM1_WLA_CON(7)) >> 8 & 0x7Fu) == 57u && (sdio_model_peek(m, FM1_WLA_CON(8)) & 0x7Fu) == 41u &&
              sdio_model_win(m, 2, 8) == 0xFF && sdio_model_win(m, 2, 9) == 0 && (sdio_model_win(m, 2, 98) & 0x0Fu) == 0x03u,
          "the trim's results stay in the registers (TX LO in WLA_CON7/8, TX DC -1/0 in the TX back end)");
    check(fm1wifi_set_channel(13) == FM1WIFI_OK && (sdio_model_peek(m, FM1_WF_CON(7)) & 0xFFu) == 114u, "channel 13: 114");
    check(fm1wifi_set_channel(0) == FM1WIFI_ERANGE && fm1wifi_set_channel(14) == FM1WIFI_ERANGE, "channel 0 and 14: RANGE");
    check(fm1wifi_set_power(3) == FM1WIFI_OK && (sdio_model_peek(m, FM1_WF_CON(7)) >> 16 & 0xFFu) == 3u, "TX power 3");
    check(fm1wifi_set_power(7) == FM1WIFI_ERANGE, "TX power 7: RANGE");
    sdio_model_wlog_clear(m);
    check(fm1wifi_adc_peak(&peak) == FM1WIFI_OK && peak > 0, "ADC peak read");
    {
        const wifi_fix *w = sdio_model_wlog(m);
        check(w->n == 3 && w->r[0].kind == WIFI_REC_WIN && w->r[0].addr == 0x317 && w->r[0].val == 0x82 &&
                  w->r[1].kind == WIFI_REC_DELAY && w->r[1].val == 1000 && w->r[2].val == 2,
              "... as RX front end 23 = 0x82, 1 ms, the peak, 23 = 2");
    }
    fm1wifi_power_down();
    check(!fm1wifi_is_up() && !fm1wifi_radio_is_up(), "powered down");
    for (i = 0; i < N_CPU; i++) {
        char what[80];
        snprintf(what, sizeof what, "power-down restores %05x after the radio", (unsigned)cpu_reg(i));
        check(sdio_model_peek(m, cpu_reg(i)) == before[i], what);
    }
    check(sdio_model_bad(m) == 0, "the radio: no access outside the WiFi blocks");
    sdio_model_free(m);

    /* the calibration bytes reach their registers */
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    m = core(path, NULL);
    sdio_model_raw_log(m, 0);
    {
        fm1wifi_cal c = fm1wifi_cal_default;
        c.xosc[0] = 3; c.xosc[1] = 12; c.pa[0] = 5; c.pa[4] = 9; c.pa[5] = 2;
        check(fm1wifi_radio_up(&c, &r) == FM1WIFI_OK, "radio up with another calibration");
        uint32_t w12 = sdio_model_peek(m, FM1_WLA_CON(12)), w9 = sdio_model_peek(m, FM1_WLA_CON(9));
        uint32_t w10 = sdio_model_peek(m, FM1_WLA_CON(10)), w11 = sdio_model_peek(m, FM1_WLA_CON(11));
        check((w12 >> 13 & 15u) == 3u && (w12 >> 19 & 15u) == 12u, "crystal caps into WLA_CON12 bits 16-13, 22-19");
        check((w9 >> 24 & 7u) == 5u && (w10 >> 22 & 15u) == 9u && (w11 & 7u) == 2u, "PA trim into WLA_CON9, 10, 11");
    }
    fm1wifi_power_down();
    sdio_model_free(m);
}

static void test_radio_waits(const char *dir)
{
    static const struct { const char *script, *stage, *what; unsigned polls; uint32_t got; } T[] = {
        { "hang tune\n", "TRIM", "the filter tune never done", 1000, 0 },
        { "hang measure\n", "TRIM", "the measurement never ready", 1000, 7 },                /* the filter codes only */
        { "hang anl\n", "TABLE", "the RF digital block always busy", 1001, 0xF7FFF },        /* + the read that saves it */
        { "stuck 0x3101c 0x00020000\n", "PHY", "BBP BUSY stuck", 1000, 0 },
    };
    char path[512];
    snprintf(path, sizeof path, "%s/core_model.txt", dir);
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        char what[120];
        fm1wifi_radio r;
        sdio_model *m = core(path, T[i].script);
        sdio_model_raw_log(m, 0);
        int rc = fm1wifi_radio_up(NULL, &r);
        snprintf(what, sizeof what, "%s: TIMEOUT at %s after %u polls (stopped at %s, %u polls)", T[i].what, T[i].stage,
                 T[i].polls, r.stage ? r.stage : "?", sdio_model_polls(m));
        check(rc == FM1WIFI_ETIMEOUT && r.err == FM1WIFI_ETIMEOUT && !strcmp(r.stage, T[i].stage) &&
                  sdio_model_polls(m) == T[i].polls && !fm1wifi_radio_is_up(), what);
        snprintf(what, sizeof what, "%s: got %04x, the values read before it (got %04x)", T[i].what, (unsigned)T[i].got,
                 (unsigned)r.got);
        check(r.got == T[i].got, what);
        snprintf(what, sizeof what, "%s: the port told the trim ended", T[i].what);
        check(!sdio_model_in_trim(m) && sdio_model_trims(m) >= 0, what);
        fm1wifi_power_down();
        check(!fm1wifi_is_up(), "powered down after a TIMEOUT");
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
    test_bringup(argv[1], argv[2]);
    test_errors(argv[1]);
    test_accessors();
    test_pattern();
    test_format();
    test_radio(argv[1], argv[2]);
    test_radio_waits(argv[1]);
    fclose(quiet);
    if (bad) return 1;
    printf("wifi_model: fm1wifi power-up, first reads, bounded waits, power-down, RAM check; the radio against JieLi's "
           "trace, its results, its waits; model, fixtures, diff pass\n");
    return 0;
}
