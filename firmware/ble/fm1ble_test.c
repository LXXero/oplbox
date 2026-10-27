/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run's state machine (fm1ble_test.h) on the baseband driver (fm1ble.h). Own code. Not run on an FM-1.
 *
 * What the run does, and does not: it powers up the radio and the baseband, starts one connectable advertisement (the
 * name OPLBOX, the BLE-MIDI service UUID) straight from the driver, counts the baseband's event flags for a second and
 * records every SCAN_REQ and CONNECT_IND the radio takes in. If no event flag comes it tries again with the shared radio's
 * common init first (docs/BLUETOOTH_RADIO.md "not settled" 1). The Cordio controller and NimBLE host are built and tested
 * (docs/BLUETOOTH_CORDIO.md) but the chip's Cordio PAL is not written, so the HCI, connect and MIDI steps report SKIP with
 * that reason, or FAIL with the raw CONNECT_IND when a central tried.
 */
#include <string.h>
#include "fm1ble.h"
#include "fm1ble_test.h"
#include "ble_midi.h"

static const char *const g_steps[FM1BLE_STEPS] = {
    "radio", "block", "hci", "adv_set", "adv_air", "connect", "midi_in", "midi_out"
};

static const char *const g_ctrs[FM1BLE_N_CTR] = {
    "radio_regs_ok", "radio_regs_bad", "variant", "rf_common", "events", "rearms", "rx_packets", "rx_scan_req",
    "rx_conn_ind", "rx_other", "pending_last", "bb_err", "events_v0", "events_v1", "rearm_used", "adv_err",
    "rx_hdr", "rx_stat", "rx_type", "rx_len", "reserved20", "reserved21", "reserved22", "reserved23"
};

const char *fm1ble_step_name(int step) { return step >= 0 && step < FM1BLE_STEPS ? g_steps[step] : 0; }
const char *fm1ble_ctr_name(int ctr) { return ctr >= 0 && ctr < FM1BLE_N_CTR ? g_ctrs[ctr] : 0; }

/* the driver's state and its RAM block: not on the stack, the baseband reads the block */
static fm1ble g_b;
static uint32_t g_blk[FM1BLE_BLOCK_SIZE / 4u];

enum { P_RADIO, P_BLOCK, P_ADV, P_SETTLE, P_WINDOW, P_REARM_WINDOW, P_RETRY, P_CONNECT, P_END };
static uint8_t g_phase, g_variant;
static uint32_t g_phase_t, g_wait_ms, g_ev_before;

static void note(fm1ble_step *s, const char *t)
{
    uint32_t n = 0;
    while (t[n] && n < FM1BLE_NOTE - 1u) {
        s->note[n] = t[n];
        n++;
    }
    s->note[n] = 0;
}

static void put32(fm1ble_step *s, uint32_t v)
{
    if (s->nraw + 4u <= FM1BLE_RAW) {
        s->raw[s->nraw++] = (uint8_t)v;
        s->raw[s->nraw++] = (uint8_t)(v >> 8);
        s->raw[s->nraw++] = (uint8_t)(v >> 16);
        s->raw[s->nraw++] = (uint8_t)(v >> 24);
    }
}

static void finish(fm1ble_run *r, int k, int result, const char *t)
{
    r->step[k].result = (uint8_t)result;
    r->step[k].ms = r->now - r->t0;
    note(&r->step[k], t);
}

static void skip_rest(fm1ble_run *r, int from, const char *why)
{
    int k;
    for (k = from; k < FM1BLE_STEPS; k++)
        if (r->step[k].result == FM1BLE_NOT_RUN)
            finish(r, k, FM1BLE_SKIP, why);
}

static void counters(fm1ble_run *r)
{
    const fm1ble_diag *d = &g_b.d;
    r->ctr[0] = d->rb_ok;
    r->ctr[1] = d->rb_bad;
    r->ctr[2] = d->variant;
    r->ctr[3] = (uint32_t)d->rf_common;
    r->ctr[4] = d->events;
    r->ctr[5] = d->rearms;
    r->ctr[6] = d->rx_packets;
    r->ctr[7] = d->rx_scan_req;
    r->ctr[8] = d->rx_conn_ind;
    r->ctr[9] = d->rx_other;
    r->ctr[10] = d->pending_last;
    r->ctr[11] = (uint32_t)d->err;
    r->ctr[14] = g_b.rearm;
    r->ctr[16] = (uint32_t)d->rx_hdr[0] | (uint32_t)d->rx_hdr[1] << 16;
    r->ctr[17] = (uint32_t)d->rx_stat[0] | (uint32_t)d->rx_stat[1] << 16;
    r->ctr[18] = d->rx_last_type;
    r->ctr[19] = d->rx_last_len;
}

/* the advertisement: connectable, the BLE-MIDI service UUID in the data, the name OPLBOX in the scan response; the address
 * is a made-up static random one (the FM-1 has no device address: docs/BLUETOOTH_RADIO.md PalCfgLoadData) */
static void make_adv(fm1ble_adv *a)
{
    static const uint8_t addr[6] = { 0x01, 0x00, 0x31, 0x4D, 0x46, 0xC2 };
    memset(a, 0, sizeof *a);
    a->type = 0;
    a->own_random = 1;
    a->chmap = 7;
    memcpy(a->addr, addr, 6);
    a->interval_slots = 160;                         /* 100 ms */
    a->nadv = (uint8_t)ble_midi_adv_data(a->adv);
    a->nrsp = (uint8_t)ble_midi_scan_rsp(a->rsp, "OPLBOX");
}

void fm1ble_test_begin(fm1ble_run *r, uint32_t now_ms, uint32_t wait_ms)
{
    memset(r, 0, sizeof *r);
    memset(&g_b, 0, sizeof g_b);
    r->state = FM1BLE_RUNNING;
    r->t0 = r->now = now_ms;
    g_phase = P_RADIO;
    g_variant = 0;
    g_wait_ms = wait_ms;
}

static void start_radio(fm1ble_run *r)
{
    fm1ble_diag *d = &g_b.d;
    fm1ble_step *s = &r->step[FM1BLE_STEP_RADIO];
    int i;

    fm1ble_radio_up(&g_b, g_variant, 0);
    for (i = 0; i < 8; i++)
        put32(s, d->rb[i]);
    if (d->rb_ok >= 4u)
        finish(r, FM1BLE_STEP_RADIO, FM1BLE_PASS, g_variant ? "regs read back, after shared radio init" : "regs read back as written");
    else
        finish(r, FM1BLE_STEP_RADIO, FM1BLE_FAIL, "analog registers do not read back: not powered?");
}

int fm1ble_test_poll(fm1ble_run *r, uint32_t now_ms)
{
    fm1ble_diag *d = &g_b.d;
    fm1ble_step *s;
    int i;

    if (r->state != FM1BLE_RUNNING)
        return r->state == FM1BLE_DONE;
    r->now = now_ms;
    switch (g_phase) {
    case P_RADIO:
        start_radio(r);
        r->cur = FM1BLE_STEP_BLOCK;
        g_phase = P_BLOCK;
        break;
    case P_BLOCK:
        s = &r->step[FM1BLE_STEP_BLOCK];
        i = fm1ble_bb_init(&g_b, g_blk);
        for (int k = 0; k < 8; k++)
            put32(s, d->bb[k]);
        if (i == 0 && d->bb[4] == g_b.blk_bus && d->bb[5] > d->bb[4])
            finish(r, FM1BLE_STEP_BLOCK, FM1BLE_PASS, "RAM block set, link open, baseband idle");
        else if (i == FM1BLE_ETIMEOUT)
            finish(r, FM1BLE_STEP_BLOCK, FM1BLE_FAIL, "baseband stays busy after power-up");
        else
            finish(r, FM1BLE_STEP_BLOCK, FM1BLE_FAIL, "block address does not read back");
        finish(r, FM1BLE_STEP_HCI, FM1BLE_SKIP, "Cordio PAL for the chip not written");
        r->cur = FM1BLE_STEP_ADV_SET;
        g_phase = P_ADV;
        break;
    case P_ADV: {
        fm1ble_adv a;
        make_adv(&a);
        s = &r->step[FM1BLE_STEP_ADV_SET];
        i = fm1ble_adv_start(&g_b, &a);
        for (int k = 0; k < a.nadv && s->nraw < FM1BLE_RAW; k++)
            s->raw[s->nraw++] = a.adv[k];
        r->ctr[15] = (uint32_t)i;
        if (i == 0)
            finish(r, FM1BLE_STEP_ADV_SET, FM1BLE_PASS, "ADV_IND armed: service UUID, name in scan response");
        else
            finish(r, FM1BLE_STEP_ADV_SET, FM1BLE_FAIL, "fm1ble_adv_start refused (see adv_err)");
        r->cur = FM1BLE_STEP_ADV_AIR;
        g_phase = i == 0 ? P_SETTLE : P_END;
        g_phase_t = now_ms;
        break;
    }
    case P_SETTLE:                                   /* the first event is armed for the next slot: give it 20 ms */
        if (now_ms - g_phase_t >= 20u) {
            g_phase = P_WINDOW;
            g_phase_t = now_ms;
            g_ev_before = d->events;
        }
        break;
    case P_WINDOW:                                   /* 400 ms without re-arming: does the hardware repeat by itself? */
        fm1ble_poll(&g_b);
        if (now_ms - g_phase_t >= 400u) {
            if (d->events >= 3u) {
                g_phase = P_CONNECT;
                g_phase_t = now_ms;
                r->ctr[g_variant ? 13 : 12] = d->events;
            } else {
                fm1ble_set_rearm(&g_b, 1);          /* D.4: JieLi's handler does per-event bookkeeping: arm the next ourselves */
                g_phase = P_REARM_WINDOW;
                g_phase_t = now_ms;
                r->ctr[g_variant ? 13 : 12] = d->events;
                g_ev_before = d->events;
                {   /* an event flag may be pending already: it re-arms in the poll */
                    fm1ble_poll(&g_b);
                }
            }
        }
        break;
    case P_REARM_WINDOW:
        fm1ble_poll(&g_b);
        if (now_ms - g_phase_t >= 600u) {
            if (d->events >= 3u && d->events > g_ev_before) {
                g_phase = P_CONNECT;
                g_phase_t = now_ms;
            } else {
                g_phase = P_RETRY;
            }
        }
        break;
    case P_RETRY:
        if (!g_variant) {                            /* again with the shared radio's init first */
            fm1ble_adv_stop(&g_b);
            fm1ble_radio_down(&g_b);
            g_variant = 1;
            r->step[FM1BLE_STEP_RADIO].nraw = 0;
            r->step[FM1BLE_STEP_BLOCK].nraw = 0;
            r->step[FM1BLE_STEP_ADV_SET].nraw = 0;
            r->cur = FM1BLE_STEP_RADIO;
            g_phase = P_RADIO;
        } else {
            finish(r, FM1BLE_STEP_ADV_AIR, FM1BLE_FAIL, "no event flag in the baseband, either variant");
            skip_rest(r, FM1BLE_STEP_CONNECT, "advertising never ran");
            g_phase = P_END;
        }
        break;
    case P_CONNECT:
        s = &r->step[FM1BLE_STEP_ADV_AIR];
        if (s->result == FM1BLE_NOT_RUN) {
            put32(s, d->events);
            put32(s, d->rearms);
            put32(s, d->pending_last);
            finish(r, FM1BLE_STEP_ADV_AIR, FM1BLE_PASS, g_b.rearm ? "events seen, driver re-arms each" : "events repeat on their own");
        }
        fm1ble_poll(&g_b);
        if (g_wait_ms == 0u) {
            skip_rest(r, FM1BLE_STEP_CONNECT, "connect wait is 0");
            g_phase = P_END;
        } else if (d->rx_conn_ind || now_ms - g_phase_t >= g_wait_ms) {
            s = &r->step[FM1BLE_STEP_CONNECT];
            for (int k = 0; k < d->rx_last_n && s->nraw < FM1BLE_RAW; k++)
                s->raw[s->nraw++] = d->rx_last[k];
            if (d->rx_conn_ind)
                finish(r, FM1BLE_STEP_CONNECT, FM1BLE_FAIL, "CONNECT_IND received; connection not set up (no Cordio PAL)");
            else if (d->rx_scan_req)
                finish(r, FM1BLE_STEP_CONNECT, FM1BLE_FAIL, "SCAN_REQ seen, no CONNECT_IND");
            else
                finish(r, FM1BLE_STEP_CONNECT, FM1BLE_FAIL, "no SCAN_REQ or CONNECT_IND: scan with a phone");
            skip_rest(r, FM1BLE_STEP_MIDI_IN, "needs a connection");
            g_phase = P_END;
        }
        break;
    default:
        break;
    }
    counters(r);
    if (g_phase == P_END) {
        skip_rest(r, 0, "not reached");
        fm1ble_radio_down(&g_b);                     /* the run leaves the radio off */
        r->state = FM1BLE_DONE;
        return 1;
    }
    return 0;
}
