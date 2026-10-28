/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run for the host build (tests/run_host.sh), in place of firmware/ble/fm1ble_test.c: the same API
 * (fm1ble_test.h), but a scripted run, not a measurement. Every number and byte below is MADE UP to look like what the
 * real run will report; nothing here came from an FM-1 or a controller. The counter names are the firmware's. It exists to test the page, the SysEx report and
 * tools/fm1_ble_report.py: the text, the layout, the SKIP after a FAIL, the timing of a run the page polls.
 *
 * Environment (read at each begin):
 *   HOST_BLE_CONNECT=1   a central connects and the MIDI steps pass; otherwise connect FAILs after the wait
 *   HOST_BLE_FAIL=NAME   the step with this name (fm1ble_step_name) FAILs; the steps after it are SKIP
 * Steps take 100 ms each, adv_air 1000 ms, connect 500 ms when a central comes (or the whole wait when none does).
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../../firmware/ble/fm1ble_test.h"

static const char *const g_steps[FM1BLE_STEPS] = {
    "radio", "block", "hci", "adv_set", "adv_air", "connect", "midi_in", "midi_out"
};
static const char *const g_ctrs[FM1BLE_N_CTR] = { FM1BLE_CTR_NAMES };   /* the firmware's names (fm1ble_test.h) */

const char *fm1ble_step_name(int step) { return step >= 0 && step < FM1BLE_STEPS ? g_steps[step] : 0; }
const char *fm1ble_ctr_name(int ctr) { return ctr >= 0 && ctr < FM1BLE_N_CTR ? g_ctrs[ctr] : 0; }

static uint32_t g_wait_ms, g_t_step;
static int g_fail = -1, g_connect;

void fm1ble_test_begin(fm1ble_run *r, uint32_t now_ms, uint32_t wait_ms)
{
    const char *f = getenv("HOST_BLE_FAIL");
    int i;
    memset(r, 0, sizeof *r);
    r->state = FM1BLE_RUNNING;
    r->t0 = r->now = g_t_step = now_ms;
    g_wait_ms = wait_ms;
    g_connect = getenv("HOST_BLE_CONNECT") != 0;
    g_fail = -1;
    for (i = 0; f && i < FM1BLE_STEPS; i++)
        if (!strcmp(f, g_steps[i]))
            g_fail = i;
}

static void put(fm1ble_step *s, const void *raw, uint32_t n)
{
    memcpy(s->raw + s->nraw, raw, n);
    s->nraw = (uint8_t)(s->nraw + n);
}

static void note(fm1ble_step *s, const char *t)
{
    size_t n = strlen(t);
    memset(s->note, 0, FM1BLE_NOTE);
    memcpy(s->note, t, n < FM1BLE_NOTE - 1 ? n : FM1BLE_NOTE - 1);
}

/* what step i reports when it passes (or the failure it is told to have) */
static void finish(fm1ble_run *r, int i)
{
    fm1ble_step *s = &r->step[i];
    static const uint8_t reg[8] = { 0x07, 0, 0, 0, 0x98, 0, 0, 0 };                 /* two made-up readbacks */
    static const uint8_t hci[] = {
        0x0E, 0x04, 0x01, 0x03, 0x0C, 0x00,                                         /* Reset: Command Complete, status 0 */
        0x0E, 0x0C, 0x01, 0x01, 0x10, 0x00, 0x09, 0x00, 0x00, 0x09, 0xFF, 0xFF, 0x00, 0x00,  /* Read Local Version */
        0x0E, 0x0A, 0x01, 0x09, 0x10, 0x00, 0x01, 0x00, 0x31, 0x4D, 0x46, 0x02,     /* Read BD_ADDR */
        0x0E, 0x07, 0x01, 0x02, 0x20, 0x00, 0x1B, 0x00, 0x02 };                     /* LE Read Buffer Size: 27 B x 2 */
    static const uint8_t addr[6] = { 0x02, 0x46, 0x4D, 0x31, 0x00, 0x01 };
    s->ms = r->now - r->t0;
    if (i == g_fail) {
        s->result = FM1BLE_FAIL;
        note(s, "made-up failure (HOST_BLE_FAIL)");
        put(s, reg, 4);
        r->ctr[FM1BLE_C_BB_ERR] = 0x12;
        return;
    }
    s->result = FM1BLE_PASS;
    switch (i) {
    case FM1BLE_STEP_RADIO: put(s, reg, 8); note(s, "2 of 2 registers read back"); r->ctr[FM1BLE_C_RADIO_REGS_OK] = 2; break;
    case FM1BLE_STEP_BLOCK: note(s, "baseband started, no interrupt storm"); r->ctr[FM1BLE_C_RAM_END] = 0x01C16940u; break;
    case FM1BLE_STEP_HCI:
        put(s, hci, sizeof hci);
        note(s, "4 commands answered, LE buffers 27 x 2");
        memcpy(r->bdaddr, addr, 6);
        r->ctr[FM1BLE_C_PAL_ADV_OPS] = 4;
        break;
    case FM1BLE_STEP_ADV_SET: note(s, "4 commands accepted"); r->ctr[FM1BLE_C_TXPWR] = 6; break;
    case FM1BLE_STEP_ADV_AIR: note(s, "37 advertising events in 1 s"); r->ctr[FM1BLE_C_EVENTS] = 37; r->ctr[FM1BLE_C_TX_FLIPS] = 37;
        r->ctr[FM1BLE_C_AIR_TRIALS] = 7; r->ctr[FM1BLE_C_PAL_ADV_EVENTS] = 12;
        r->ctr[FM1BLE_C_PAL_CFG] = 100u | 400u << 16;
        r->ctr[FM1BLE_C_PAL_TX_ORDER] = 3u | 0u << 4 | 2u << 8 | 1u << 12;
        r->ctr[FM1BLE_C_PAL_RESP_US_MIN] = 0xFFFFu;
        break;
    case FM1BLE_STEP_CONNECT:
        if (!g_connect) {
            s->result = FM1BLE_FAIL;
            note(s, "no central connected in 20 s");
        } else {
            static const uint8_t cc[] = { 0x3E, 0x13, 0x01, 0x00, 0x40, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0xEE, 0xFF, 0xC0,
                                          0x18, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00 };
            put(s, cc, sizeof cc);
            note(s, "connected, interval 30 ms, MTU 185");
            r->ctr[FM1BLE_C_RX_CONN_IND] = 1; r->ctr[FM1BLE_C_RX_LEN] = 34; r->ctr[FM1BLE_C_RX_TYPE] = 5;
            /* two connections (made up): padw missed all 6 events, padb received 38 of 40 */
            r->ctr[FM1BLE_C_PAL_CONNS] = 2; r->ctr[FM1BLE_C_PAL_GOOD] = 0xFFu;
            r->ctr[FM1BLE_C_NIMBLE_CONNS] = 2u | 1u << 16;
            r->ctr[FM1BLE_C_CONN_SUM] = 6u | 6u << 4; r->ctr[FM1BLE_C_CONN_SUM + 1] = 0x80008000u;
            r->ctr[FM1BLE_C_CONN_SUM + 2] = 262u | 846u << 16;
            r->ctr[FM1BLE_C_CONN_SUM + 3] = 5u | 40u << 4 | 38u << 16; r->ctr[FM1BLE_C_CONN_SUM + 4] = 31u | 29u << 16;
            r->ctr[FM1BLE_C_CONN_SUM + 5] = 0x80008000u;
            r->ctr[FM1BLE_C_CONN_RX_WORDS] = 0x00090801u; r->ctr[FM1BLE_C_PAL_SET4 + 2] = 40u | 38u << 16;
            /* the TX side (made up): the first connection intime (one stale flag, one event halted), the second jieli */
            r->ctr[FM1BLE_C_CONN_SUM_D] = 2u | 1u << 16 | 1u << 24; r->ctr[FM1BLE_C_CONN_SUM_D + 1] = 1u | 37u << 4;
            r->ctr[FM1BLE_C_PAL_CONN_TXS] = 2u | 1u << 4; r->ctr[FM1BLE_C_PAL_GOOD_TX] = 0xFFu;
            r->ctr[FM1BLE_C_PAL_TX_COUNTS + 2] = 6u; r->ctr[FM1BLE_C_PAL_TX_COUNTS + 1] = 40u | 38u << 16;
            r->ctr[FM1BLE_C_PAL_TX_SENT + 1] = 37u | 2u << 16;
            r->ctr[FM1BLE_C_PAL_RESP_HIST] = 30u | 5u << 8 | 2u << 16 | 1u << 24; r->ctr[FM1BLE_C_PAL_RESP_US_MIN] = 21u;
            r->ctr[FM1BLE_C_PAL_STALE_RX] = 1u; r->ctr[FM1BLE_C_PAL_HALTS] = 1u;
            r->ctr[FM1BLE_C_PAL_FIRST_RX_TX] = 0x01u | 0x05u << 8 | 1u << 16 | 1u << 31;
            r->ctr[FM1BLE_C_CONN_TXTOG] = 1u | 1u << 16; r->ctr[FM1BLE_C_CONN_TXDHDR] = 0x0905u | 0x0001u << 16;
            r->ctr[FM1BLE_C_CONN_TXBUF] = 1u | 0x8295u << 16;
            /* the link layer (made up): the first connection's central asked LL_FEATURE_REQ and LL_VERSION_IND, Cordio
             * answered the first, then a packet with a bad CRC whose answer did not go in; the second's one L2CAP PDU */
            r->ctr[FM1BLE_C_TRACE] = 0x03u | 1u << 5 | 1u << 6 | 1u << 7 | 0x08u << 8 | 0x09u << 16 | 3u << 21 | 3u << 23;
            r->ctr[FM1BLE_C_TRACE + 1] = 0x03u | 1u << 5 | 1u << 6 | 1u << 7 | 0x0Cu << 8 | 0x07u << 16 | 3u << 21 | 3u << 23 |
                                         0x09u << 25;
            r->ctr[FM1BLE_C_TRACE + 2] = 0x01u | 0x05u << 16 | 1u << 21;
            r->ctr[FM1BLE_C_TRACE + 20] = 0x02u | 1u << 5 | 1u << 6 | 0x07u << 8 | 0x01u << 16 | 3u << 21 | 1u << 24;
            r->ctr[FM1BLE_C_CONN_OPS] = 1u << 8 | 1u << 12; r->ctr[FM1BLE_C_CONN_LL] = 0u | 1u << 8 | 0u << 16 | 2u << 24;
            r->ctr[FM1BLE_C_CONN_LL + 1] = 1u | 0u << 8 | 3u << 16 | 38u << 24;
            r->ctr[FM1BLE_C_PAL_TX3_COUNTS] = 12u | 10u << 16; r->ctr[FM1BLE_C_PAL_TX3_SENT] = 9u;
            r->ctr[FM1BLE_C_PAL_ARQ_DUP] = 1u | 4u << 16; r->ctr[FM1BLE_C_PAL_ARQ_UNWRITTEN] = 1u | 3u << 16;
            r->ctr[FM1BLE_C_STACK_ACL] = 3u | 2u << 16; r->ctr[FM1BLE_C_STACK_NOCP] = 2u;
            r->ctr[FM1BLE_C_NIMBLE_MTU] = 185u | 1u << 16; r->ctr[FM1BLE_C_PAL_RXTOG] = 1u;
            /* the PDU capture (made up): two ARQ connections, the first ahead; bit 2 changed once at a first packet;
             * BlueZ's MTU request, NimBLE's answer written ahead, BlueZ's Read By Group Type request and NimBLE's error */
            {
                static const uint8_t cap[] = {
                    1, 4, 0x14, 2, 1, 0, 0xFF, 3, 1, 0, 0, 0, 0x28, 0, 3, 0,
                    6, 2, 7, 4, 0, 1, 3, 0, 4, 0, 2, 5, 2, 0, 0, 0, 0, 0, 0, 0,
                    7, 0x0A, 7, 5, 0, 9, 3, 0, 4, 0, 3, 0x17, 0, 0, 0, 0, 0, 0, 0, 0,
                    6, 2, 0x0B, 7, 0, 1, 7, 0, 4, 0, 0x10, 1, 0, 0xFF, 0xFF, 0, 0x28, 0, 0, 0,
                    7, 0x0E, 9, 8, 0, 0x0C, 5, 0, 4, 0, 1, 0x10, 1, 0, 0x0A, 0, 0, 0, 0, 0 };
                memcpy(r->cap, cap, sizeof cap);
                r->ncap = sizeof cap;
            }
        }
        break;
    case FM1BLE_STEP_MIDI_IN: { static const uint8_t m[] = { 0x81, 0x85, 0x90, 0x3C, 0x64 };
        put(s, m, sizeof m); note(s, "note-on 3C/64 from the central"); r->ctr[FM1BLE_C_PAL_RX_OK] = 185; break; }
    case FM1BLE_STEP_MIDI_OUT: { static const uint8_t m[] = { 0x87, 0xE8, 0x90, 0x3C, 0x7F };
        put(s, m, sizeof m); note(s, "notification sent, buffer returned"); r->ctr[FM1BLE_C_PAL_DONE_37] = 1; r->ctr[FM1BLE_C_PAL_DONE_38] = 2;
        r->ctr[FM1BLE_C_PAL_DONE_39] = 3; break; }
    }
}

int fm1ble_test_poll(fm1ble_run *r, uint32_t now_ms)
{
    uint32_t dur;
    int i = r->cur;
    r->now = now_ms;
    if (r->state != FM1BLE_RUNNING)
        return r->state == FM1BLE_DONE;
    if (i >= FM1BLE_STEPS) {
        r->state = FM1BLE_DONE;
        return 1;
    }
    /* a failed step ends the run: the steps after it are SKIP */
    if (i > 0 && r->step[i - 1].result == FM1BLE_FAIL) {
        for (; i < FM1BLE_STEPS; i++) {
            r->step[i].result = FM1BLE_SKIP;
            note(&r->step[i], "skipped: an earlier step failed");
        }
        r->state = FM1BLE_DONE;
        return 1;
    }
    if ((i == FM1BLE_STEP_CONNECT || i == FM1BLE_STEP_MIDI_IN || i == FM1BLE_STEP_MIDI_OUT) && g_wait_ms == 0) {
        r->step[i].result = FM1BLE_SKIP;
        note(&r->step[i], "skipped: no wait for a central");
        r->cur++;
        return 0;
    }
    dur = i == FM1BLE_STEP_ADV_AIR ? 1000u : i == FM1BLE_STEP_CONNECT ? (g_connect ? 500u : g_wait_ms) : 100u;
    if (now_ms - g_t_step < dur)
        return 0;
    if (i == FM1BLE_STEP_CONNECT && !g_connect && g_fail != i) {
        char t[FM1BLE_NOTE] = "no central connected in ";
        uint32_t s = g_wait_ms / 1000u, n = (uint32_t)strlen(t);
        if (s >= 100u) t[n++] = (char)('0' + s / 100u);
        if (s >= 10u) t[n++] = (char)('0' + s / 10u % 10u);
        t[n++] = (char)('0' + s % 10u);
        t[n++] = ' '; t[n++] = 's'; t[n] = 0;
        finish(r, i);
        note(&r->step[i], t);
    } else {
        finish(r, i);
    }
    g_t_step = now_ms;
    r->cur++;
    return 0;
}
