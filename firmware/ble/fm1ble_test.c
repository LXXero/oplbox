/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run's state machine (fm1ble_test.h) on the baseband driver (fm1ble.h). Own code.
 *
 * What the run does: it powers up the radio and the baseband, then advertises straight from the driver (connectable, the
 * name OPLBOX in the scan response, the BLE-MIDI service UUID) in three trials of FM1BLE_TRIAL_MS, each from its own
 * address (fm1ble_test.h): without the shared radio's common init, with it (docs/BLUETOOTH_RADIO.md "not settled" 1), and
 * with it at the highest level. A central scanning meanwhile (tools/ble_central.py) says which reached the air; the
 * firmware's own evidence is per trial: the event flags, the changes of TXTOG (the hardware's record of a buffer sent)
 * and the packets received. adv_air passes only with events and one of the other two: an event timer that runs with
 * nothing sent (the first FM-1 run, 48ea034) fails it. Four RX trials follow, each with its own receive setup
 * (fm1ble_test.h): what a central's SCAN_REQs did in each is in the rxtN counters. Then the stack comes up on the radio of the trial that sent
 * (fm1ble_stack.h: Cordio's PAL, Cordio, NimBLE): hci passes when NimBLE's start-up through Cordio ends with advertising
 * on; connect when a central connects within the wait; midi_in when a BLE-MIDI write from it reaches the MIDI input;
 * midi_out when OPLBOX's notification of a Note On is acknowledged (Cordio's Number Of Completed Packets). A connection
 * is closed before the run turns the radio off. Everything is polled from the main loop: the PAL's counters say how late
 * it was.
 */
#include <string.h>
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"
#include "fm1ble_test.h"
#include "ble_midi.h"
#include "ble_nimble.h"
#include "fm1ble_stack.h"
#include "pal_fm1.h"

static const char *const g_steps[FM1BLE_STEPS] = {
    "radio", "block", "hci", "adv_set", "adv_air", "connect", "midi_in", "midi_out"
};

static const char *const g_ctrs[FM1BLE_N_CTR] = { FM1BLE_CTR_NAMES };

const char *fm1ble_step_name(int step) { return step >= 0 && step < FM1BLE_STEPS ? g_steps[step] : 0; }
const char *fm1ble_ctr_name(int ctr) { return ctr >= 0 && ctr < FM1BLE_N_CTR ? g_ctrs[ctr] : 0; }

/* the driver's state and its RAM block: not on the stack, the baseband reads the block */
static fm1ble g_b;
static uint32_t g_blk[FM1BLE_BLOCK_SIZE / 4u];

enum { P_RADIO, P_BLOCK, P_ADV, P_SETTLE, P_WINDOW, P_TRIAL_END, P_STACK, P_HCI, P_WAIT_CONN, P_MIDI_IN, P_MIDI_OUT,
       P_LEAVE, P_END };
#define ALL_TRIALS (FM1BLE_TRIALS + FM1BLE_RX_TRIALS)
static uint8_t g_phase, g_variant, g_sent, g_left, g_trial, g_trial_bits, g_adv_err, g_rearm_used, g_stack_rx;
static uint32_t g_phase_t, g_wait_ms, g_nocp0;
static struct { uint32_t events, flips, rx, scan_req; } g_tr[ALL_TRIALS], g_at;   /* each trial's counts; the counts at its start */
static uint16_t g_hw[8];                         /* the entity's hardware-written words after the last air trial */
/* the RX trials' setups (fm1ble_test.h): the gain table, WINCNTL2 */
static const struct { uint8_t agc; uint16_t win; } RXT[FM1BLE_RX_TRIALS] = { { 0, 0x32 }, { 1, 0 }, { 1, 0x32 }, { 1, 0x96 } };
/* what each RX trial saw: its first packet's words and bytes (fm1ble_test.h FM1BLE_RXT_*), or the words it left */
static struct { uint8_t got; uint32_t stat, pkt, bytes; } g_rx[FM1BLE_RX_TRIALS];
static uint32_t g_agc_ctl, g_agc_rssi, g_rx_stat1;
static int g_rf_common;                          /* what the shared radio's init returned the first time (trial 1) */

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

static void put16(fm1ble_step *s, uint32_t v)
{
    if (v > 0xFFFFu)
        v = 0xFFFFu;
    if (s->nraw + 2u <= FM1BLE_RAW) {
        s->raw[s->nraw++] = (uint8_t)v;
        s->raw[s->nraw++] = (uint8_t)(v >> 8);
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

static uint32_t sat8(uint32_t v) { return v > 0xFFu ? 0xFFu : v; }
static uint32_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : v; }
static uint32_t s16(int32_t v) { return (uint32_t)(uint16_t)(int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static uint32_t ew(uint32_t off) { return (uint32_t)(g_b.blk[g_b.entity + off] | g_b.blk[g_b.entity + off + 1u] << 8); }

/* an RX trial's first packet (fm1ble_test.h FM1BLE_RXT_*), from the driver's record of the last one */
static void rx_seen(uint32_t k)
{
    const fm1ble_diag *d = &g_b.d;
    uint32_t i;
    g_rx[k].got = 1;
    g_rx[k].stat = (uint32_t)d->rx_stat[d->rx_last_buf & 1u] | (uint32_t)d->rx_last_ahdr << 16;
    g_rx[k].pkt = (uint32_t)d->rx_last_dhdr | (uint32_t)d->rx_last_buf << 16 | (ew(FM1BLE_E_LASTCHMAP) & 0xFFu) << 24;
    g_rx[k].bytes = 0;
    for (i = 0; i < 4u && i < d->rx_last_n; i++)
        g_rx[k].bytes |= (uint32_t)d->rx_last[i] << (8u * i);
}

static void counters(fm1ble_run *r)
{
    const fm1ble_diag *d = &g_b.d;
    const pal_fm1_stats *p = pal_fm1_state();
    uint32_t k;
    r->ctr[FM1BLE_C_RADIO_REGS_OK] = d->rb_ok;
    r->ctr[FM1BLE_C_RADIO_REGS_BAD] = d->rb_bad;
    r->ctr[FM1BLE_C_VARIANT] = g_variant;
    r->ctr[FM1BLE_C_RF_COMMON] = (uint32_t)g_rf_common;
    r->ctr[FM1BLE_C_EVENTS] = d->events;
    r->ctr[FM1BLE_C_TX_FLIPS] = d->tx_flips;
    r->ctr[FM1BLE_C_RX_PACKETS] = d->rx_packets;
    r->ctr[FM1BLE_C_RX_SCAN_REQ] = d->rx_scan_req;
    r->ctr[FM1BLE_C_RX_CONN_IND] = d->rx_conn_ind;
    r->ctr[FM1BLE_C_RX_OTHER] = d->rx_other;
    r->ctr[FM1BLE_C_PENDING_LAST] = d->pending_last;
    r->ctr[FM1BLE_C_BB_ERR] = (uint32_t)d->err;
    r->ctr[FM1BLE_C_AIR_TRIALS] = g_trial_bits;
    r->ctr[FM1BLE_C_TXTOG] = d->txtog;
    r->ctr[FM1BLE_C_REARM_USED] = g_rearm_used;
    r->ctr[FM1BLE_C_ADV_ERR] = (uint32_t)(int32_t)(int8_t)g_adv_err;
    r->ctr[FM1BLE_C_RX_HDR] = (uint32_t)d->rx_hdr[0] | (uint32_t)d->rx_hdr[1] << 16;
    r->ctr[FM1BLE_C_RX_STAT] = (uint32_t)d->rx_stat[0] | (uint32_t)d->rx_stat[1] << 16;
    r->ctr[FM1BLE_C_RX_TYPE] = d->rx_last_type;
    r->ctr[FM1BLE_C_RX_LEN] = d->rx_last_len;
    /* the PAL's (pal_fm1.h): Cordio's operations, receive completions, responses too late for T_IFS, operations the
     * baseband never ended, the most the scheduler's timer fired late; Cordio's advertising packets completed, per channel;
     * the hardware advertising events armed, the packets that went in one already armed, the events the baseband ended
     * and the ones a CONNECT_IND ended; connection events the baseband ended and the packets received in them; operations
     * the driver refused; and the entity's EVTCOUNT word, which the hardware writes */
    r->ctr[FM1BLE_C_PAL_ADV_OPS] = p->adv_ops;
    r->ctr[FM1BLE_C_PAL_CONN_OPS] = p->conn_ops;
    r->ctr[FM1BLE_C_PAL_RX_OK] = p->rx_ok;
    r->ctr[FM1BLE_C_PAL_RX_TIMEOUT] = p->rx_timeout;
    r->ctr[FM1BLE_C_PAL_RESP_LATE] = p->resp_late;
    r->ctr[FM1BLE_C_PAL_STUCK] = p->stuck;
    r->ctr[FM1BLE_C_PAL_TIMER_LATE_US] = p->timer_late_us;
    r->ctr[FM1BLE_C_PAL_DONE_37] = p->adv_done[0];
    r->ctr[FM1BLE_C_PAL_DONE_38] = p->adv_done[1];
    r->ctr[FM1BLE_C_PAL_DONE_39] = p->adv_done[2];
    r->ctr[FM1BLE_C_TXPWR] = g_b.txpwr;
    r->ctr[FM1BLE_C_RAM_END] = d->ram_end;
    r->ctr[FM1BLE_C_PAL_ADV_EVENTS] = p->adv_events;
    r->ctr[FM1BLE_C_PAL_ADV_COVERED] = p->adv_covered;
    r->ctr[FM1BLE_C_PAL_ADV_ENDED] = p->adv_ended;
    r->ctr[FM1BLE_C_PAL_ADV_CONN_IND] = p->adv_conn_ind;
    r->ctr[FM1BLE_C_PAL_CONN_ENDED] = p->conn_ended;
    r->ctr[FM1BLE_C_PAL_CONN_RX] = p->conn_rx;
    r->ctr[FM1BLE_C_PAL_REFUSED] = p->refused;
    if (g_b.blk)
        r->ctr[FM1BLE_C_EVTCOUNT] = ew(FM1BLE_E_EVTCOUNT);
    for (k = 0; k < FM1BLE_RX_TRIALS; k++) {
        uint32_t *c = &r->ctr[FM1BLE_C_RXT + 4u * k], j = FM1BLE_TRIALS + k;
        c[FM1BLE_RXT_COUNTS] = sat8(g_tr[j].events) | sat8(g_tr[j].flips) << 8 | sat8(g_tr[j].rx) << 16 | sat8(g_tr[j].scan_req) << 24;
        c[FM1BLE_RXT_STAT] = g_rx[k].stat;
        c[FM1BLE_RXT_PKT] = g_rx[k].pkt;
        c[FM1BLE_RXT_BYTES] = g_rx[k].bytes;
    }
    /* the receive setup the stack runs on (0xFF before it starts), what the AGC block read after the last RX trial, the
     * windows the last operation left (WINCNTL0 low, WINCNTL2 high), Cordio's last connection receive timeout, RXSTAT1
     * after the last RX trial */
    r->ctr[FM1BLE_C_RX_STACK_TRIAL] = g_stack_rx;
    r->ctr[FM1BLE_C_RX_AGC_TABLE] = d->agc_table;
    r->ctr[FM1BLE_C_RX_WIN] = g_b.rx_set ? g_b.rx_win : FM1BLE_ADV_WIN_US;
    r->ctr[FM1BLE_C_AGC_CTL] = g_agc_ctl;
    r->ctr[FM1BLE_C_AGC_RSSI] = g_agc_rssi;
    if (g_b.blk)
        r->ctr[FM1BLE_C_CONN_WIN] = ew(FM1BLE_E_WINCNTL0) | ew(FM1BLE_E_WINCNTL2) << 16;
    r->ctr[FM1BLE_C_PAL_CONN_TIMEOUT_US] = p->conn_timeout_us;
    r->ctr[FM1BLE_C_RX_STAT1] = g_rx_stat1;
    /* the connection events (pal_fm1.c: the busy wait, the window setups tried), the hardware's words in them */
    r->ctr[FM1BLE_C_PAL_BUSY_WAITS] = p->busy_waits;
    r->ctr[FM1BLE_C_PAL_BUSY_US_MAX] = p->busy_us_max;
    r->ctr[FM1BLE_C_PAL_RESP_US_MAX] = p->resp_us_max;
    r->ctr[FM1BLE_C_PAL_CONN_RX_BAD] = p->conn_rx_bad;
    r->ctr[FM1BLE_C_PAL_CONN_SETUP] = p->conn_setup;
    r->ctr[FM1BLE_C_PAL_SETUP_MOVES] = p->setup_moves;
    r->ctr[FM1BLE_C_PAL_LATE_OPS] = p->late_ops;
    r->ctr[FM1BLE_C_CONN_FLIPS] = d->conn_flips;
    for (k = 0; k < 3u; k++) {
        r->ctr[FM1BLE_C_PAL_SET_COUNTS + k] = sat16(p->set_events[k]) | sat16(p->set_rx[k]) << 16;
        r->ctr[FM1BLE_C_PAL_SET_ARR + k] = s16(p->set_arr_min[k]) | s16(p->set_arr_max[k]) << 16;
    }
    r->ctr[FM1BLE_C_CONN_ACKS] = d->conn_acks;
    r->ctr[FM1BLE_C_CONN_RX_WORDS] = (uint32_t)d->conn_stat | (uint32_t)d->conn_dhdr << 16;
}

/* the addresses: made-up static random ones (the FM-1 has no device address: docs/BLUETOOTH_RADIO.md PalCfgLoadData),
 * least significant byte first; the stack's, then the trials' (fm1ble_test.h) differ in the first byte */
static const uint8_t g_addr[6] = { 0x01, 0x00, 0x31, 0x4D, 0x46, 0xC2 };

/* the advertisement: connectable, the BLE-MIDI service UUID in the data, the name OPLBOX in the scan response */
static void make_adv(fm1ble_adv *a, uint8_t first)
{
    memset(a, 0, sizeof *a);
    a->type = 0;
    a->own_random = 1;
    a->chmap = 7;
    memcpy(a->addr, g_addr, 6);
    a->addr[0] = first;
    a->interval_slots = 160;                         /* 100 ms */
    a->nadv = (uint8_t)ble_midi_adv_data(a->adv);
    a->nrsp = (uint8_t)ble_midi_scan_rsp(a->rsp, "OPLBOX");
}

void fm1ble_test_begin(fm1ble_run *r, uint32_t now_ms, uint32_t wait_ms)
{
    memset(r, 0, sizeof *r);
    memset(&g_b, 0, sizeof g_b);
    memset(g_tr, 0, sizeof g_tr);
    memset(g_hw, 0, sizeof g_hw);
    memset(g_rx, 0, sizeof g_rx);
    g_agc_ctl = g_agc_rssi = g_rx_stat1 = 0;
    g_stack_rx = 0xFFu;
    g_rf_common = 0;
    fm1ble_rx_setup(&g_b, 0, 0);                     /* the air trials: the receive setup up to 3b39ab4 (fm1ble_test.h) */
    r->state = FM1BLE_RUNNING;
    r->t0 = r->now = now_ms;
    g_phase = P_RADIO;
    g_variant = 0;
    g_trial = 0;
    g_trial_bits = 0;
    g_adv_err = 0;
    g_rearm_used = 0;
    g_wait_ms = wait_ms;
    g_sent = g_left = 0;
}

static void start_radio(fm1ble_run *r)
{
    fm1ble_diag *d = &g_b.d;
    fm1ble_step *s = &r->step[FM1BLE_STEP_RADIO];
    int i;

    fm1ble_radio_up(&g_b, g_variant, 0);
    if (g_variant && g_trial == 1)                   /* later power-ups find it done: on the FM-1 WiFi is up, it returns 0 */
        g_rf_common = d->rf_common;
    if (s->result != FM1BLE_NOT_RUN)                 /* a later power-up (another variant): its readbacks are in the counters */
        return;
    for (i = 0; i < 8; i++)
        put32(s, d->rb[i]);
    if (d->rb_ok >= 4u)
        finish(r, FM1BLE_STEP_RADIO, FM1BLE_PASS, "regs read back as written");
    else
        finish(r, FM1BLE_STEP_RADIO, FM1BLE_FAIL, "analog registers do not read back: not powered?");
}

/* the block step: the start address must read back; the end (0x2FCBC) read 0 on the first FM-1 run, and JieLi's driver
 * writes it and never reads it (docs/BLUETOOTH_RADIO.md D.1), so its readback is evidence, not the test. Whether the
 * baseband really works from the block shows in adv_air: TXTOG is a word of the block the hardware changes. */
static void block_step(fm1ble_run *r, int rc)
{
    const fm1ble_diag *d = &g_b.d;
    fm1ble_step *s = &r->step[FM1BLE_STEP_BLOCK];
    int k;
    if (s->result != FM1BLE_NOT_RUN)
        return;
    for (k = 0; k < 8; k++)
        put32(s, d->bb[k]);
    if (rc == FM1BLE_ETIMEOUT)
        finish(r, FM1BLE_STEP_BLOCK, FM1BLE_FAIL, "baseband stays busy after power-up");
    else if (rc != 0 || d->bb[4] != g_b.blk_bus)
        finish(r, FM1BLE_STEP_BLOCK, FM1BLE_FAIL, "block start address does not read back");
    else if (d->bb[5] > d->bb[4])
        finish(r, FM1BLE_STEP_BLOCK, FM1BLE_PASS, "RAM block set, link open, baseband idle");
    else if (d->bb[5] == 0)
        finish(r, FM1BLE_STEP_BLOCK, FM1BLE_PASS, "block start reads back; end reads 0 (write-only)");
    else
        finish(r, FM1BLE_STEP_BLOCK, FM1BLE_PASS, "block start reads back; end reads other");
}

/* "SENT IN TRIALS 0 1 2" or the like, from the bits */
static void trials_note(char *t, const char *head, uint32_t bits)
{
    uint32_t n = 0, k;
    while (head[n]) {
        t[n] = head[n];
        n++;
    }
    for (k = 0; k < FM1BLE_TRIALS; k++)
        if (bits & (1u << k)) {
            t[n++] = ' ';
            t[n++] = (char)('0' + k);
        }
    t[n] = 0;
}

/* After the trials: adv_air's result, then the stack on the radio of the variant that showed it sent (variant 1, the
 * superset, when both did or neither did). The stack advertises at the SDK's level. */
static void air_step(fm1ble_run *r)
{
    fm1ble_step *s = &r->step[FM1BLE_STEP_ADV_AIR];
    uint32_t k, ev = 0;
    char t[FM1BLE_NOTE];

    for (k = 0; k < FM1BLE_TRIALS; k++) {
        put16(s, g_tr[k].events);
        put16(s, g_tr[k].flips);
        put16(s, g_tr[k].rx);
        ev += g_tr[k].events;
    }
    for (k = 0; k < 8; k++)
        put16(s, g_hw[k]);
    if (g_trial_bits) {
        trials_note(t, g_rearm_used ? "sent (re-armed), trials" : "sent, trials", g_trial_bits);
        finish(r, FM1BLE_STEP_ADV_AIR, FM1BLE_PASS, t);
    } else if (ev) {
        finish(r, FM1BLE_STEP_ADV_AIR, FM1BLE_FAIL, "events, but TXTOG never changed: nothing sent");
    } else {
        finish(r, FM1BLE_STEP_ADV_AIR, FM1BLE_FAIL, "no event flag in the baseband, any trial");
    }
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
        if (r->cur == FM1BLE_STEP_RADIO)
            r->cur = FM1BLE_STEP_BLOCK;
        g_phase = P_BLOCK;
        break;
    case P_BLOCK:
        i = fm1ble_bb_init(&g_b, g_blk);
        block_step(r, i);
        if (r->cur == FM1BLE_STEP_BLOCK)
            r->cur = FM1BLE_STEP_ADV_SET;
        g_phase = P_ADV;
        break;
    case P_ADV: {                                    /* trial g_trial: the driver's own ADV_IND from the trial's address */
        fm1ble_adv a;
        make_adv(&a, (uint8_t)(FM1BLE_TRIAL_ADDR0 + g_trial));
        fm1ble_op_power(&g_b, (uint8_t)(g_trial == 2 ? FM1BLE_TXPWR_MAX : FM1BLE_TXPWR_LEVEL));
        s = &r->step[FM1BLE_STEP_ADV_SET];
        i = fm1ble_adv_start(&g_b, &a);
        if (i != 0)
            g_adv_err = (uint8_t)i;
        if (s->result == FM1BLE_NOT_RUN) {
            for (int k = 0; k < a.nadv && s->nraw < FM1BLE_RAW; k++)
                s->raw[s->nraw++] = a.adv[k];
            if (i == 0)
                finish(r, FM1BLE_STEP_ADV_SET, FM1BLE_PASS, "ADV_IND armed: service UUID, name in scan response");
            else
                finish(r, FM1BLE_STEP_ADV_SET, FM1BLE_FAIL, "fm1ble_adv_start refused (see adv_err)");
            r->cur = FM1BLE_STEP_ADV_AIR;
            if (i != 0) {
                skip_rest(r, FM1BLE_STEP_ADV_AIR, "advertising never ran");
                g_phase = P_END;
                break;
            }
        }
        g_at.events = d->events;
        g_at.flips = d->tx_flips;
        g_at.rx = d->rx_packets;
        g_at.scan_req = d->rx_scan_req;
        g_phase = i == 0 ? P_SETTLE : P_TRIAL_END;
        g_phase_t = now_ms;
        break;
    }
    case P_SETTLE:                                   /* the first event is armed for the next slot: give it 20 ms */
        if (now_ms - g_phase_t >= 20u) {
            g_phase = P_WINDOW;
            g_phase_t = now_ms;
        }
        break;
    case P_WINDOW:                                   /* FM1BLE_TRIAL_MS of advertising for a central to see */
        fm1ble_poll(&g_b);
        if (g_trial >= FM1BLE_TRIALS && !g_rx[g_trial - FM1BLE_TRIALS].got && d->rx_packets != g_at.rx)
            rx_seen(g_trial - FM1BLE_TRIALS);
        if (!g_b.rearm && now_ms - g_phase_t >= 400u && d->events - g_at.events < 3u) {
            fm1ble_set_rearm(&g_b, 1);              /* D.4: JieLi's handler does per-event bookkeeping: arm the next ourselves */
            g_rearm_used = 1;
            fm1ble_adv_stop(&g_b);                   /* and start again, so the first event is armed (an empty flag may wait) */
            {
                fm1ble_adv a;
                make_adv(&a, (uint8_t)(FM1BLE_TRIAL_ADDR0 + g_trial));
                fm1ble_adv_start(&g_b, &a);
            }
        }
        if (now_ms - g_phase_t >= FM1BLE_TRIAL_MS)
            g_phase = P_TRIAL_END;
        break;
    case P_TRIAL_END: {                              /* what the trial showed; the next trial, or the stack */
        static const uint32_t hw[8] = { FM1BLE_E_TXTOG, FM1BLE_E_RXTOG, FM1BLE_E_ANCHOR, FM1BLE_E_BITOFF, FM1BLE_E_EVTCOUNT,
                                        FM1BLE_E_LASTCHMAP, FM1BLE_E_RXSTAT0, FM1BLE_E_FORMAT };
        const uint8_t *e = g_b.blk + g_b.entity;
        g_tr[g_trial].events = d->events - g_at.events;
        g_tr[g_trial].flips = d->tx_flips - g_at.flips;
        g_tr[g_trial].rx = d->rx_packets - g_at.rx;
        g_tr[g_trial].scan_req = d->rx_scan_req - g_at.scan_req;
        if (g_trial < FM1BLE_TRIALS) {
            if (g_tr[g_trial].events >= 3u && (g_tr[g_trial].flips || g_tr[g_trial].rx))
                g_trial_bits |= (uint8_t)(1u << g_trial);
            for (i = 0; i < 8; i++)
                g_hw[i] = (uint16_t)(e[hw[i]] | e[hw[i] + 1u] << 8);
        } else {                                     /* an RX trial: with no packet, the words it left; the AGC block */
            uint32_t k = g_trial - FM1BLE_TRIALS;
            if (!g_rx[k].got) {
                g_rx[k].stat = ew(FM1BLE_E_RXSTAT0) | ew(FM1BLE_E_RXAHDR0) << 16;
                g_rx[k].pkt = ew(FM1BLE_E_RXDHDR0) | 0xFFu << 16 | (ew(FM1BLE_E_LASTCHMAP) & 0xFFu) << 24;
            }
            g_agc_ctl = fm1ble_port_rd32(FM1BLE_AGC_0);
            g_agc_rssi = fm1ble_port_rd32(FM1BLE_AGC_RSSI);
            g_rx_stat1 = ew(FM1BLE_E_RXSTAT1);
        }
        if (g_b.adv_on)
            fm1ble_adv_stop(&g_b);
        g_trial++;
        if (g_trial == 1) {                          /* trial 1: again from power-up, with the shared radio's init first */
            fm1ble_radio_down(&g_b);
            g_variant = 1;
            g_phase = P_RADIO;
        } else if (g_trial < FM1BLE_TRIALS) {        /* trial 2: the same radio, the highest level */
            g_phase = P_ADV;
        } else if (g_trial < ALL_TRIALS && (g_tr[0].events || g_tr[1].events || g_tr[2].events)) {
            /* an RX trial (none when no air trial had an event): its setup; the gain table needs a power-up */
            uint32_t k = g_trial - FM1BLE_TRIALS;
            fm1ble_rx_setup(&g_b, RXT[k].agc, RXT[k].win);
            if (RXT[k].agc && !d->agc_table) {
                fm1ble_radio_down(&g_b);
                g_variant = 1;
                g_phase = P_RADIO;
            } else {
                g_phase = P_ADV;
            }
        } else {
            g_phase = P_STACK;
        }
        break;
    }
    case P_STACK:                                    /* adv_air's result; the stack takes over on the radio that sent */
        air_step(r);
        if (!g_tr[0].events && !g_tr[1].events && !g_tr[2].events) {
            skip_rest(r, FM1BLE_STEP_HCI, "advertising never ran");
            g_phase = P_END;
            break;
        }
        {   /* the window of the first RX trial that received, in the order 2, 3, 0, 1; trial 2's (JieLi's) when none did */
            static const uint8_t pick[FM1BLE_RX_TRIALS] = { 2, 3, 0, 1 };
            g_stack_rx = 2;
            for (i = 0; i < FM1BLE_RX_TRIALS; i++)
                if (g_tr[FM1BLE_TRIALS + pick[i]].rx) {
                    g_stack_rx = pick[i];
                    break;
                }
            fm1ble_rx_setup(&g_b, 1, RXT[g_stack_rx].win);
        }
        if ((g_trial_bits & 1u) && !(g_trial_bits & 6u)) {   /* only variant 0 sent: the radio again without the shared init */
            fm1ble_radio_down(&g_b);
            g_variant = 0;
            fm1ble_radio_up(&g_b, 0, 0);
            fm1ble_bb_init(&g_b, g_blk);
        }
        fm1ble_set_rearm(&g_b, 0);
        fm1ble_op_power(&g_b, FM1BLE_TXPWR_LEVEL);
        r->cur = FM1BLE_STEP_HCI;
        if (fm1ble_stack_start(&g_b, g_addr) != 0) {
            put32(&r->step[FM1BLE_STEP_HCI], (uint32_t)fm1ble_stack_state()->err);
            finish(r, FM1BLE_STEP_HCI, FM1BLE_FAIL, fm1ble_stack_state()->err == -1 ? "Cordio did not start: heap too small"
                                                                                   : "NimBLE did not start: its GATT tables");
            g_phase = P_END;
            break;
        }
        g_phase = P_HCI;
        g_phase_t = now_ms;
        break;
    case P_HCI: {                                    /* NimBLE's start-up through Cordio, then advertising through Cordio */
        const ble_nimble_status *n = ble_nimble_state();
        fm1ble_stack_poll();
        if (n->state == BLE_NIMBLE_ADVERTISING || n->state == BLE_NIMBLE_CONNECTED || n->state == BLE_NIMBLE_FAILED ||
            now_ms - g_phase_t >= 3000u) {
            const fm1ble_stack_info *k = fm1ble_stack_state();
            s = &r->step[FM1BLE_STEP_HCI];
            for (i = 0; i < 6; i++)
                r->bdaddr[i] = n->bdaddr[i];
            put32(s, k->hci_cmds);
            put32(s, k->hci_events);
            put32(s, k->heap_used);
            put32(s, (uint32_t)n->fail);
            put32(s, n->hci_refused);
            for (i = 0; i < 6 && s->nraw < FM1BLE_RAW; i++)
                s->raw[s->nraw++] = n->bdaddr[i];
            if (n->state == BLE_NIMBLE_ADVERTISING || n->state == BLE_NIMBLE_CONNECTED) {
                finish(r, FM1BLE_STEP_HCI, FM1BLE_PASS, "Cordio answered NimBLE; advertising via Cordio");
                r->cur = FM1BLE_STEP_CONNECT;
                g_phase = P_WAIT_CONN;
            } else {
                finish(r, FM1BLE_STEP_HCI, FM1BLE_FAIL, n->state == BLE_NIMBLE_FAILED ? "NimBLE start failed (raw: fail code)"
                                                                                      : "no answer to NimBLE in 3 s");
                skip_rest(r, FM1BLE_STEP_CONNECT, "needs the controller");
                g_phase = P_END;
            }
            if (g_phase == P_WAIT_CONN && g_wait_ms == 0u) {
                skip_rest(r, FM1BLE_STEP_CONNECT, "connect wait is 0");
                g_phase = P_LEAVE;
            }
            g_phase_t = now_ms;
        }
        break;
    }
    case P_WAIT_CONN: {
        const ble_nimble_status *n = ble_nimble_state();
        fm1ble_stack_poll();
        s = &r->step[FM1BLE_STEP_CONNECT];
        if (n->state == BLE_NIMBLE_CONNECTED) {
            put32(s, n->interval);
            put32(s, n->latency);
            put32(s, n->timeout);
            put32(s, n->mtu);
            finish(r, FM1BLE_STEP_CONNECT, FM1BLE_PASS, "a central connected");
            r->cur = FM1BLE_STEP_MIDI_IN;
            g_phase = P_MIDI_IN;
            g_phase_t = now_ms;
        } else if (now_ms - g_phase_t >= g_wait_ms) {
            for (int k = 0; k < d->rx_last_n && s->nraw < FM1BLE_RAW; k++)
                s->raw[s->nraw++] = d->rx_last[k];
            if (d->rx_conn_ind)
                finish(r, FM1BLE_STEP_CONNECT, FM1BLE_FAIL, "CONNECT_IND received, not connected (pal ctrs)");
            else if (d->rx_scan_req)
                finish(r, FM1BLE_STEP_CONNECT, FM1BLE_FAIL, "SCAN_REQ seen, no CONNECT_IND");
            else
                finish(r, FM1BLE_STEP_CONNECT, FM1BLE_FAIL, "no SCAN_REQ or CONNECT_IND: scan with a phone");
            skip_rest(r, FM1BLE_STEP_MIDI_IN, "needs a connection");
            g_phase = P_LEAVE;
            g_phase_t = now_ms;
        }
        break;
    }
    case P_MIDI_IN: {                                /* the central plays: a BLE-MIDI write reaches the MIDI input */
        const ble_nimble_status *n = ble_nimble_state();
        uint8_t m[3];
        fm1ble_stack_poll();
        s = &r->step[FM1BLE_STEP_MIDI_IN];
        if (ble_midi_get(fm1ble_stack_midi(), &m[0], &m[1], &m[2])) {
            for (i = 0; i < 3; i++)
                s->raw[s->nraw++] = m[i];
            put32(s, n->writes);
            finish(r, FM1BLE_STEP_MIDI_IN, FM1BLE_PASS, "MIDI from the central (raw: the message)");
            r->cur = FM1BLE_STEP_MIDI_OUT;
            g_phase = P_MIDI_OUT;
            g_phase_t = now_ms;
            g_sent = 0;
        } else if (n->state != BLE_NIMBLE_CONNECTED || now_ms - g_phase_t >= g_wait_ms) {
            put32(s, n->writes);
            put32(s, n->rx_bad);
            finish(r, FM1BLE_STEP_MIDI_IN, FM1BLE_FAIL, n->state != BLE_NIMBLE_CONNECTED ? "the central left" :
                   n->writes ? "writes came, no MIDI in them" : "no write: play a note on the phone");
            skip_rest(r, FM1BLE_STEP_MIDI_OUT, "needs MIDI in");
            g_phase = P_LEAVE;
            g_phase_t = now_ms;
        }
        break;
    }
    case P_MIDI_OUT: {                               /* OPLBOX plays: a notification, acknowledged (Number Of Completed Packets) */
        static const uint8_t note_on[5] = { 0x80, 0x80, 0x90, 0x3C, 0x64 };
        const ble_nimble_status *n = ble_nimble_state();
        const fm1ble_stack_info *k = fm1ble_stack_state();
        fm1ble_stack_poll();
        s = &r->step[FM1BLE_STEP_MIDI_OUT];
        if (!g_sent && n->notify_on) {
            g_nocp0 = k->nocp;
            g_sent = ble_nimble_notify(0, note_on, sizeof note_on) == 0 ? 1u : 2u;
        }
        if (g_sent == 1u && k->nocp > g_nocp0) {
            put32(s, n->notified);
            put32(s, k->nocp);
            finish(r, FM1BLE_STEP_MIDI_OUT, FM1BLE_PASS, "Note On notified, buffer handed back");
            g_phase = P_LEAVE;
            g_phase_t = now_ms;
        } else if (g_sent == 2u || n->state != BLE_NIMBLE_CONNECTED || now_ms - g_phase_t >= g_wait_ms) {
            put32(s, n->notified);
            put32(s, n->notify_failed);
            put32(s, k->nocp);
            finish(r, FM1BLE_STEP_MIDI_OUT, FM1BLE_FAIL, g_sent == 2u ? "NimBLE refused the notification" :
                   !g_sent ? "the central did not subscribe" : "notified, never acknowledged");
            g_phase = P_LEAVE;
            g_phase_t = now_ms;
        }
        break;
    }
    case P_LEAVE:                                    /* the run ends: a connection is closed before the radio goes off */
        fm1ble_stack_poll();
        if (ble_nimble_state()->state == BLE_NIMBLE_CONNECTED && now_ms - g_phase_t < 500u) {
            if (!g_left) {
                ble_nimble_disconnect();
                g_left = 1;
            }
        } else {
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
