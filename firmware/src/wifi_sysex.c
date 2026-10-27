/* SPDX-License-Identifier: GPL-3.0-only */
/* The WiFi test page over USB SysEx (docs/WIFI_STAGE4.md "Reading the results over USB"), in the updater's framing
 * as slot_sysex.c: F0 pack7(00 59 cmd len24 body ~sum(body)) F7, numbers little-endian.
 *
 *   0x62 report  host: off24     device: status total24 off24 serial16 data (512 bytes at most)
 *   0x63 run     host: (none)    device: status run16
 *
 * The report is the last run's results, kept in RAM until the next run: every line the page's ten pages show, as
 * text, then the counters and the frames the FIFO gave, as bytes (wr_* below build it; docs/API.md has the layout).
 * serial16 changes whenever the report is rebuilt (the page rebuilds it when its RAM check changes), so a reader
 * that sees it change between chunks starts again. A run is started by the main loop as SAVE then PLAY would; the
 * run command only asks for it and answers at once with the number the run will have.
 *
 * Include it with the app's:
 *   wifi_report_state()         WR_NONE (no run since start), WR_RUNNING (asked for or running), WR_DONE
 *   wifi_report(&n, &serial)    the report and its length (WR_DONE)
 *   wifi_run_ask(&run)          0: a run will start, *run its number; else WIFI_E_RUNNING (*run the one running) or
 *                               WIFI_E_EDIT
 *   wifi_reply(cmd, head, nh, data, nd)   one message to the computer (slot_sysex.c's slot_reply)
 * The command bytes are none of the updater's (0x11, 0x30), fm1-base's (0x41-0x43, 0x50-0x54) or the slots' (0x60,
 * 0x61). */
#define WIFI_CMD_REPORT 0x62u
#define WIFI_CMD_RUN 0x63u
#define WIFI_CHUNK 512u
enum { WIFI_OK, WIFI_E_REQ, WIFI_E_NONE, WIFI_E_RUNNING, WIFI_E_EDIT };
enum { WR_NONE, WR_RUNNING, WR_DONE };

/* ---- the report: "OPLW" version flags run16 text_len16 text, then K32 counters, then the frames
 *      (step ch had16 kept16 bytes each). Built front to back into the app's buffer; one that does not fit is cut
 *      short and says so (flags bit 0: the text, bit 1: the frames) rather than overrun. */
#define WR_MAGIC "OPLW"
#define WR_VERSION 1u
#define WR_HEAD 10u
enum { WR_CUT_TEXT = 1, WR_CUT_FRAMES = 2 };
typedef struct {
    uint8_t *b;
    uint32_t max, n, text_max, n_ctr_at, n_fr_at;
} wr_buf;

static void wr_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* start a report for run number run; text_max bytes at most of text */
static void wr_begin(wr_buf *w, uint8_t *b, uint32_t max, uint32_t text_max, uint32_t run)
{
    uint32_t i;
    w->b = b; w->max = max; w->text_max = text_max; w->n = WR_HEAD; w->n_ctr_at = w->n_fr_at = 0;
    for (i = 0; i < 4u; i++) b[i] = (uint8_t)WR_MAGIC[i];
    b[4] = WR_VERSION; b[5] = 0;
    wr_put16(b + 6, run);
    wr_put16(b + 8, 0);
}

/* one line of text (a '\n' is added); the text ends at the first wr_counters */
static void wr_line(wr_buf *w, const char *s)
{
    uint32_t k = 0, t = w->n - WR_HEAD;
    while (s[k]) k++;
    if (w->n_ctr_at || t + k + 1u > w->text_max || w->n + k + 1u > w->max) { w->b[5] |= WR_CUT_TEXT; return; }
    while (*s) w->b[w->n++] = (uint8_t)*s++;
    w->b[w->n++] = '\n';
    wr_put16(w->b + 8, w->n - WR_HEAD);
}

/* the counters, once, after the text: n values (255 at most) */
static void wr_counters(wr_buf *w, const uint32_t *v, uint32_t n)
{
    uint32_t i;
    if (w->n_ctr_at || w->n + 2u + 4u * n > w->max || n > 255u) { w->b[5] |= WR_CUT_FRAMES; return; }
    w->n_ctr_at = w->n;
    w->b[w->n++] = (uint8_t)n;
    for (i = 0; i < n; i++, w->n += 4u) { wr_put16(w->b + w->n, v[i]); wr_put16(w->b + w->n + 2, v[i] >> 16); }
    w->n_fr_at = w->n;
    w->b[w->n++] = 0;                           /* frames so far */
}

/* one frame after the counters: the step that read it (1 passive, 2 active, 3 join), its channel, the bytes it had,
 * and the first kept of them */
static void wr_frame(wr_buf *w, uint32_t step, uint32_t ch, uint32_t had, const uint8_t *p, uint32_t kept)
{
    uint32_t i;
    if (!w->n_fr_at || w->b[w->n_fr_at] == 255u || w->n + 6u + kept > w->max) { w->b[5] |= WR_CUT_FRAMES; return; }
    w->b[w->n++] = (uint8_t)step;
    w->b[w->n++] = (uint8_t)ch;
    wr_put16(w->b + w->n, had); wr_put16(w->b + w->n + 2, kept);
    w->n += 4u;
    for (i = 0; i < kept; i++) w->b[w->n++] = p[i];
    w->b[w->n_fr_at]++;
}

/* ---- the commands */
static uint32_t wifi_rd24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static void wifi_wr24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); }

static void wifi_report_cmd(const uint8_t *d, uint32_t n)
{
    uint8_t h[9];
    uint32_t off = n == 10u ? wifi_rd24(d + 6) : 0, total = 0, serial = 0, k = 0, st, state = wifi_report_state();
    const uint8_t *r = 0;
    if (n != 10u) st = WIFI_E_REQ;
    else if (state == WR_NONE) st = WIFI_E_NONE;
    else if (state == WR_RUNNING) st = WIFI_E_RUNNING;
    else {
        r = wifi_report(&total, &serial);
        if (off > total) st = WIFI_E_REQ;
        else { st = WIFI_OK; k = total - off > WIFI_CHUNK ? WIFI_CHUNK : total - off; }
    }
    h[0] = (uint8_t)st;
    wifi_wr24(h + 1, st == WIFI_OK || st == WIFI_E_REQ ? total : 0);
    wifi_wr24(h + 4, off);
    wr_put16(h + 7, serial);
    wifi_reply(WIFI_CMD_REPORT, h, sizeof h, k ? r + off : 0, k);
}

static void wifi_run_cmd(const uint8_t *d, uint32_t n)
{
    uint8_t h[3];
    uint32_t run = 0, st = n == 7u ? (uint32_t)wifi_run_ask(&run) : WIFI_E_REQ;
    (void)d;
    h[0] = (uint8_t)st;
    wr_put16(h + 1, run);
    wifi_reply(WIFI_CMD_RUN, h, sizeof h, 0, 0);
}

/* a frame in the updater's framing that is not the updater's (d: decoded, n: its length); 1 = it was ours */
static int wifi_sysex(const uint8_t *d, uint32_t n)
{
    if (n < 7u) return 0;
    if (d[2] == WIFI_CMD_REPORT) { wifi_report_cmd(d, n); return 1; }
    if (d[2] == WIFI_CMD_RUN) { wifi_run_cmd(d, n); return 1; }
    return 0;
}
