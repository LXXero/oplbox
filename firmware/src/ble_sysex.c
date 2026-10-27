/* SPDX-License-Identifier: GPL-3.0-only */
/* The Bluetooth test page over USB SysEx (docs/BLUETOOTH_PAGE.md), in the updater's framing as wifi_sysex.c and
 * slot_sysex.c: F0 pack7(00 59 cmd len24 body ~sum(body)) F7, numbers little-endian.
 *
 *   0x66 report  host: off24          device: status total24 off24 serial16 data (512 bytes at most)
 *   0x67 run     host: (none) or wait8  device: status run16
 *                wait8: seconds a central has to connect once advertising is on (0-120; 0 skips the connect and MIDI
 *                steps); none: 20
 *
 * The statuses are wifi_sysex.c's (WIFI_OK, WIFI_E_REQ, WIFI_E_NONE, WIFI_E_RUNNING, WIFI_E_EDIT) and so are the
 * report's framing helpers (wr_begin, wr_line, wr_counters, wr_frame) and the reply function: include this file after
 * wifi_sysex.c. The report is built the same way, with its own magic "OPLB" (version 1):
 *   "OPLB" version flags run16 text_len16 text, then the counters (n8, n x u32), then the steps as the WiFi report's
 *   frames: one per step, in order: step index (fm1ble_test.h FM1BLE_STEP_*), result (FM1BLE_NOT_RUN..SKIP), the bytes
 *   of raw evidence it had and kept (both the same here), then those bytes.
 * serial16 changes whenever the report is rebuilt, so a reader that sees it change between chunks starts again. A run
 * is started by the main loop as SAVE then PLAY would on the page; the run command only asks for it and answers at
 * once with the number the run will have.
 *
 * Include it with the app's:
 *   ble_report_state()         WR_NONE (no run since start), WR_RUNNING (asked for or running), WR_DONE
 *   ble_report(&n, &serial)    the report and its length (WR_DONE)
 *   ble_run_ask(wait_s, &run)  0: a run will start, *run its number; else WIFI_E_RUNNING (*run the one running) or
 *                              WIFI_E_EDIT
 * The command bytes are none of the updater's (0x11, 0x30), fm1-base's (0x41-0x43, 0x50-0x54), the slots' (0x60, 0x61)
 * or the WiFi page's (0x62-0x65). */
#define BLE_CMD_REPORT 0x66u
#define BLE_CMD_RUN 0x67u
#define BLE_MAGIC "OPLB"
#define BLE_WAIT_DEFAULT 20u
#define BLE_WAIT_MAX 120u

/* a report for run number run: wr_begin, then the magic */
static void wb_begin(wr_buf *w, uint8_t *b, uint32_t max, uint32_t text_max, uint32_t run)
{
    uint32_t i;
    wr_begin(w, b, max, text_max, run);
    for (i = 0; i < 4u; i++) b[i] = (uint8_t)BLE_MAGIC[i];
}

static void ble_report_cmd(const uint8_t *d, uint32_t n)
{
    uint8_t h[9];
    uint32_t off = n == 10u ? wifi_rd24(d + 6) : 0, total = 0, serial = 0, k = 0, st, state = ble_report_state();
    const uint8_t *r = 0;
    if (n != 10u) st = WIFI_E_REQ;
    else if (state == WR_NONE) st = WIFI_E_NONE;
    else if (state == WR_RUNNING) st = WIFI_E_RUNNING;
    else {
        r = ble_report(&total, &serial);
        if (off > total) st = WIFI_E_REQ;
        else { st = WIFI_OK; k = total - off > WIFI_CHUNK ? WIFI_CHUNK : total - off; }
    }
    h[0] = (uint8_t)st;
    wifi_wr24(h + 1, st == WIFI_OK || st == WIFI_E_REQ ? total : 0);
    wifi_wr24(h + 4, off);
    wr_put16(h + 7, serial);
    wifi_reply(BLE_CMD_REPORT, h, sizeof h, k ? r + off : 0, k);
}

static void ble_run_cmd(const uint8_t *d, uint32_t n)
{
    uint8_t h[3];
    uint32_t run = 0, st = WIFI_E_REQ;
    if (n == 7u) st = (uint32_t)ble_run_ask(BLE_WAIT_DEFAULT, &run);
    else if (n == 8u && d[6] <= BLE_WAIT_MAX) st = (uint32_t)ble_run_ask(d[6], &run);
    h[0] = (uint8_t)st;
    wr_put16(h + 1, run);
    wifi_reply(BLE_CMD_RUN, h, sizeof h, 0, 0);
}

/* a frame in the updater's framing that is not the updater's (d: decoded, n: its length); 1 = it was ours */
static int ble_sysex(const uint8_t *d, uint32_t n)
{
    if (n < 7u) return 0;
    if (d[2] == BLE_CMD_REPORT) { ble_report_cmd(d, n); return 1; }
    if (d[2] == BLE_CMD_RUN) { ble_run_cmd(d, n); return 1; }
    return 0;
}
