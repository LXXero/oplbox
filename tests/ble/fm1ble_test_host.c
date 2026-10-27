/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run for the host build (tests/run_host.sh), in place of firmware/ble/fm1ble_test.c: the same API
 * (fm1ble_test.h), but a scripted run, not a measurement. Every number and byte below is MADE UP to look like what the
 * real run will report; nothing here came from an FM-1 or a controller. It exists to test the page, the SysEx report and
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
static const char *const g_ctrs[FM1BLE_N_CTR] = {
    "radio_regs_ok", "radio_regs_bad", "bb_irq", "bb_irq_tx", "bb_irq_rx", "hci_cmds", "hci_events", "hci_bad",
    "hci_status", "hci_op", "adv_events", "connects", "disconnects", "acl_in", "acl_out", "att_mtu",
    "interval", "latency", "timeout", "midi_in", "midi_out", "notify_failed", "nocp", "reserved"
};

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
        r->ctr[8] = 0x12;
        return;
    }
    s->result = FM1BLE_PASS;
    switch (i) {
    case FM1BLE_STEP_RADIO: put(s, reg, 8); note(s, "2 of 2 registers read back"); r->ctr[0] = 2; break;
    case FM1BLE_STEP_BLOCK: note(s, "baseband started, no interrupt storm"); r->ctr[2] = 3; break;
    case FM1BLE_STEP_HCI:
        put(s, hci, sizeof hci);
        note(s, "4 commands answered, LE buffers 27 x 2");
        memcpy(r->bdaddr, addr, 6);
        r->ctr[5] = 4; r->ctr[6] = 4;
        break;
    case FM1BLE_STEP_ADV_SET: note(s, "4 commands accepted"); r->ctr[5] = 8; r->ctr[6] = 8; break;
    case FM1BLE_STEP_ADV_AIR: note(s, "37 advertising events in 1 s"); r->ctr[2] = 114; r->ctr[3] = 111; r->ctr[10] = 37; break;
    case FM1BLE_STEP_CONNECT:
        if (!g_connect) {
            s->result = FM1BLE_FAIL;
            note(s, "no central connected in 20 s");
        } else {
            static const uint8_t cc[] = { 0x3E, 0x13, 0x01, 0x00, 0x40, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0xEE, 0xFF, 0xC0,
                                          0x18, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00 };
            put(s, cc, sizeof cc);
            note(s, "connected, interval 30 ms, MTU 185");
            r->ctr[11] = 1; r->ctr[15] = 185; r->ctr[16] = 24; r->ctr[18] = 72;
        }
        break;
    case FM1BLE_STEP_MIDI_IN: { static const uint8_t m[] = { 0x81, 0x85, 0x90, 0x3C, 0x64 };
        put(s, m, sizeof m); note(s, "note-on 3C/64 from the central"); r->ctr[19] = 1; r->ctr[13] = 1; break; }
    case FM1BLE_STEP_MIDI_OUT: { static const uint8_t m[] = { 0x87, 0xE8, 0x90, 0x3C, 0x7F };
        put(s, m, sizeof m); note(s, "notification sent, buffer returned"); r->ctr[20] = 1; r->ctr[14] = 1; r->ctr[22] = 1; break; }
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
