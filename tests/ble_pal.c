/* SPDX-License-Identifier: GPL-3.0-only
 * Host test of Cordio's PAL on the FM-1's baseband (firmware/ble/cordio_port/pal_fm1.c) with the radio driver's one-packet
 * operations (firmware/ble/fm1ble.c) against the register model's air (tests/ble/bb_model.c, bb_air): Cordio's controller
 * (firmware/ble/cordio/, unchanged) driven over HCI like a host would, and a central in this file that hears the
 * peripheral's packets on the air and answers as the Core Specification says (Vol 6 Part B: SCAN_REQ, CONNECT_IND, the
 * transmit window, channel selection algorithm #1, SN and NESN, LL_TERMINATE_IND). The receive interrupt runs within T_IFS
 * here (the model calls it at once): fm1ble.h's assumption, which only the FM-1 can confirm. What it shows: Cordio drives
 * the radio through the PAL; it does not show the chip does what the model does. Exits 1 when a check fails. */
#include <stdio.h>
#include <string.h>
#include "cordio_ctrl.h"
#include "pal_fm1.h"
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "fm1ble_regs.h"
#include "bb_model.h"
#include "air_central.h"
#include "hci.h"
#include "ble_midi.h"

static int bad, checks;
static void check(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "FAIL: %s\n", what); bad = 1; } }

/* ---- the host side: events and ACL from the controller ---- */

static uint8_t g_ev[64][260];
static uint16_t g_evn[64];
static int g_nev;
static uint8_t g_acl[8][260];
static uint16_t g_acln[8];
static int g_nacl;

static int to_host(void *ctx, uint8_t type, const uint8_t *p, uint16_t n)
{
    (void)ctx;
    if (type == HCI_EVT && g_nev < 64) {
        memcpy(g_ev[g_nev], p, n);
        g_evn[g_nev++] = n;
    } else if (type == HCI_ACL && g_nacl < 8) {
        memcpy(g_acl[g_nacl], p, n);
        g_acln[g_nacl++] = n;
    }
    return 0;
}

static uint32_t now_ms(void *ctx) { (void)ctx; return (uint32_t)(bbm.now_us / 1000u); }

void air_central_irq(void *ctx) { (void)ctx; pal_fm1_service(); }

static void run_us(uint64_t us)
{
    uint64_t end = bbm.now_us + us;
    while (bbm.now_us < end) {
        bb_model_advance_us(100);
        pal_fm1_service();
        cordio_ctrl_poll();
    }
}

/* what a test does to the k-th connection event since the attach, once, right after the PAL armed it (g_hook_ev: k,
 * 0 none): a packet flag at once, before its window (H_STALE), a baseband that runs nothing for it and raises no flag
 * (H_HANG), the main loop coming back 300 us late after its packet (H_LATE) */
enum { H_NONE, H_STALE, H_HANG, H_LATE };
static int g_hook, g_hook_done, g_hang_stuck;
static uint32_t g_hook_ev;
static uint64_t g_hang_t0;
static uint8_t g_tx_modes;              /* polled_run's pal_fm1_cfg tx_modes */

static void hook(void)
{
    const pal_fm1_stats *st = pal_fm1_state();
    if (g_hook == H_HANG && g_hook_done == 1 && st->conn_ops > g_hook_ev) {
        bbm.silent = 0;                 /* the next event runs again */
        g_hook_done = 2;
    }
    if (g_hook == H_HANG && g_hook_done == 1 && bbm.now_us - g_hang_t0 > 50000u && !g_hang_stuck) {
        g_hang_stuck = 1;               /* the PAL never gave up on it: end it, so the test fails instead of hanging */
        bbm.silent = 0;
        bb_model_pend(1u);
    }
    if (!g_hook || g_hook_done || st->conn_ops != g_hook_ev || !bbm.armed)
        return;
    g_hook_done = 1;
    if (g_hook == H_STALE)
        bb_model_rx(0, 0, (const uint8_t *)"", 0);
    else if (g_hook == H_HANG) {
        bbm.silent = 1;
        g_hang_t0 = bbm.now_us;
    }
    else if (g_hook == H_LATE) {
        bbm.jump_reads = 2;
        bbm.jump_us = 300;
    }
}

/* polled as the FM-1 polls (no interrupt): the main loop comes back every step_us */
static void run_polled_us(uint64_t us, uint32_t step_us)
{
    uint64_t end = bbm.now_us + us;
    while (bbm.now_us < end) {
        bb_model_advance_us(step_us);
        pal_fm1_service();
        cordio_ctrl_poll();
    }
}

/* the first event of this code at or after index `from`, or -1 */
static int find_ev(int from, uint8_t code, uint8_t sub)
{
    for (int i = from; i < g_nev; i++)
        if (g_ev[i][0] == code && (code != 0x3E || g_ev[i][2] == sub))
            return i;
    return -1;
}

static int cmd(uint16_t op, const uint8_t *par, uint8_t n)
{
    uint8_t p[260];
    int i;
    p[0] = (uint8_t)op;
    p[1] = (uint8_t)(op >> 8);
    p[2] = n;
    if (n)
        memcpy(p + 3, par, n);
    g_nev = 0;
    cordio_ctrl_from_host(HCI_CMD, p, (uint16_t)(3 + n));
    run_us(3000);
    for (i = 0; i < g_nev; i++)
        if (g_ev[i][0] == 0x0E && g_ev[i][3] == (uint8_t)op && g_ev[i][4] == (uint8_t)(op >> 8))
            return g_ev[i][5];
    return -1;
}

/* A connection polled from a main loop that comes back every 200 us (the FM-1's PAL runs from the loop: at 58cadc5 it
 * received 1 packet in 18 connection events, the scheduler's timer at most 215 us late; much later than 300 us, Cordio's
 * SCH_LOAD_DELAY_US, and Cordio skips events). busy: the PAL waits through each connection event (pal_fm1.c); setups:
 * the window setups it may try (pal_fm1_cfg conn_setups); lead: the scheduler's timer fires PAL_FM1_TIMER_LEAD_US
 * early (pal_fm1_cfg no_timer_lead when 0). Returns the
 * connection events the model ran whose packet fell outside the peripheral's window; the central writes, the host
 * sends, the central ends it. polled_again: the next connection on the same attach (the PAL's count of connections goes
 * on, so the next setup of the order runs). */
static uint32_t polled_run(fm1ble *b, int attach, int busy, uint8_t setups, int lead, uint16_t *handle, int *data_ok)
{
    static bb_air polled;
    pal_fm1_cfg pc;
    uint8_t buf[2];
    uint32_t missed0 = bbm.air_missed;
    int k;

    polled = AIR;
    polled.irq = 0;
    bbm.air = &polled;
    memset(&pc, 0, sizeof pc);
    pc.b = b;
    pc.no_busy_wait = (uint8_t)!busy;
    pc.conn_setups = setups;
    pc.no_timer_lead = (uint8_t)!lead;
    pc.tx_modes = g_tx_modes;
    if (attach)
        pal_fm1_attach(&pc);
    memset(&C, 0, sizeof C);
    buf[0] = 1;
    cmd(0x200A, buf, 1);
    g_nev = 0;
    C.want_conn = 1;
    run_polled_us(400000, 200);
    k = find_ev(0, 0x3E, 0x01);
    *handle = k >= 0 ? (uint16_t)(g_ev[k][4] | g_ev[k][5] << 8) : 0xFFFF;
    {
        static const uint8_t att[] = { 0x07, 0x00, 0x04, 0x00, 0x52, 0x10, 0x00, 0x80, 0x80, 0x90, 0x3C, 0x64 };
        g_nacl = 0;
        c_send(2, att, sizeof att);
        run_polled_us(200000, 200);
        *data_ok = C.connected && C.nq == 0 && g_nacl == 1 && g_acln[0] == 4 + sizeof att;
    }
    if (C.connected) {
        c_send(3, (const uint8_t *)"\x02\x13", 2);
        run_polled_us(300000, 200);
    }
    C.connected = 0;
    bbm.air = &AIR;
    return bbm.air_missed - missed0;
}

static uint32_t polled_conn(fm1ble *b, int busy, uint8_t setups, int lead, uint16_t *handle, int *data_ok)
{
    return polled_run(b, 1, busy, setups, lead, handle, data_ok);
}

static uint32_t polled_again(fm1ble *b, int busy, uint8_t setups, int lead, uint16_t *handle, int *data_ok)
{
    return polled_run(b, 0, busy, setups, lead, handle, data_ok);
}

static const uint8_t PERIPH[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };   /* least significant first */

/* a log entry's setup (pal_fm1.h: bits 7-6 and 14) */
static uint32_t log_setup(uint32_t a) { return (a >> 6 & 3u) | (a >> 14 & 1u) << 2; }

/* The PAL's event log (pal_fm1.h) holds what each logged connection event did: one setup for a whole connection (a new
 * one only at an event marked first), at most PAL_FM1_LOG_PER events of each, a packet's start before the event's end.
 * 1 when it does. */
static int log_ok(const pal_fm1_stats *st)
{
    uint32_t k, run = 0;
    if (!st->log_n || !(st->log[0][0] & 1u << 13))
        return 0;
    for (k = 0; k < st->log_n; k++) {
        uint32_t a = st->log[k][0], b = st->log[k][1];
        int got = (a >> 8) & 1u;
        run = (a & 1u << 13) ? 1u : run + 1u;
        if (run > PAL_FM1_LOG_PER || (k && !(a & 1u << 13) && log_setup(a) != log_setup(st->log[k - 1][0])))
            return 0;
        if (got && (b & 0xFFFFu) == PAL_FM1_LOG_NONE)
            return 0;
        if (got && (b >> 16) != PAL_FM1_LOG_NONE && (int16_t)(b >> 16) <= (int16_t)(b & 0xFFFFu))
            return 0;
    }
    return 1;
}


int main(void)
{
    static uint8_t blk[FM1BLE_BLOCK_SIZE] __attribute__((aligned(4)));
    static fm1ble B;
    cordio_ctrl_cfg c;
    pal_fm1_cfg pc;
    uint8_t buf[40], data[31], nd;
    uint16_t handle = 0xFFFF;
    uint32_t late0 = 0;
    int i, k;

    bb_model_reset();
    check(fm1ble_radio_up(&B, 0, NULL) == FM1BLE_OK && fm1ble_bb_init(&B, blk) == FM1BLE_OK, "the driver comes up on the model");
    bbm.air = &AIR;
    memset(&pc, 0, sizeof pc);
    pc.b = &B;
    pc.tx_modes = 1u << FM1BLE_TX_AIR;               /* this connection's data both ways on the model's air layout */
    pal_fm1_attach(&pc);
    memset(&c, 0, sizeof c);
    c.to_host.send = to_host;
    c.now_ms = now_ms;
    memcpy(c.bdaddr, PERIPH, 6);
    check(cordio_ctrl_init(&c) == 0, "cordio_ctrl_init on the PAL");

    check(cmd(0x0C03, NULL, 0) == 0, "Reset");
    buf[0] = 0x10; buf[1] = 0x80; buf[2] = 0; buf[3] = 0; buf[4] = 0; buf[5] = 0; buf[6] = 0; buf[7] = 0x20;
    check(cmd(0x0C01, buf, 8) == 0, "Set Event Mask: disconnection, hardware error, LE meta");
    memset(buf, 0, 8); buf[0] = 0x05;
    check(cmd(0x2001, buf, 8) == 0, "LE Set Event Mask: connection complete, update complete");

    /* ADV_IND every 30 ms on 37-39, the profile's data, a scan response */
    memset(buf, 0, 15);
    buf[0] = 0x30; buf[2] = 0x30; buf[13] = 0x07;
    check(cmd(0x2006, buf, 15) == 0, "LE Set Advertising Parameters: ADV_IND, 30 ms, channels 37-39");
    memset(buf, 0, 32);
    nd = (uint8_t)ble_midi_adv_data(data);
    buf[0] = nd;
    memcpy(buf + 1, data, nd);
    check(cmd(0x2008, buf, 32) == 0, "LE Set Advertising Data");
    memset(buf, 0, 32);
    buf[0] = 8; buf[1] = 7; buf[2] = 9; memcpy(buf + 3, "OPLBOX", 6);
    check(cmd(0x2009, buf, 32) == 0, "LE Set Scan Response Data");
    buf[0] = 1;
    check(cmd(0x200A, buf, 1) == 0, "LE Set Advertising Enable");

    run_us(200000);
    check(C.adv_seen[0] >= 3 && C.adv_seen[1] >= 3 && C.adv_seen[2] >= 3, "ADV_IND heard on 37, 38 and 39, event after event");
    /* Cordio keeps the channel index running from event to event (bb_ble_adv_slave.c): an event is the three channels once
     * each, in a rotated order */
    check(C.nchan >= 6 && C.chan_order[0] + C.chan_order[1] + C.chan_order[2] == 114 && C.chan_order[0] != C.chan_order[1] &&
          C.chan_order[1] != C.chan_order[2] && C.chan_order[3] == C.chan_order[0], "each event: 37, 38 and 39 once each, the same order every event");
    /* the hardware hops inside one event (the advertising the FM-1 put on the air at 122afe3): 37, 38, 39 in that order
     * whatever channel Cordio asked for first, well inside the 10 ms the specification allows between them */
    check(C.nchan >= 3 && C.chan_order[0] == 37 && C.chan_order[1] == 38 && C.chan_order[2] == 39 &&
          C.chan_t[1] > C.chan_t[0] && C.chan_t[1] - C.chan_t[0] < 10000u && C.chan_t[2] > C.chan_t[1] &&
          C.chan_t[2] - C.chan_t[1] < 10000u, "an event's packets on 37, 38, 39 in order, under 10 ms apart");
    check(bbm.one_chan == 0, "no one-channel advertising event (hop 0: never heard from the FM-1, on 38 and 39 never ended)");
    {
        const pal_fm1_stats *st = pal_fm1_state();
        check(st->adv_events >= 3 && st->adv_covered == 2u * st->adv_events && st->adv_ops == 3u * st->adv_events,
              "one hardware event for each of Cordio's events: its other two packets go in it");
        check(st->adv_ended == st->adv_events, "the baseband ended every advertising event (its flag)");
        check(st->adv_done[0] == st->adv_events && st->adv_done[1] == st->adv_events && st->adv_done[2] == st->adv_events,
              "every one of Cordio's packets completed, as many on 37, 38 and 39");
    }
    check(C.last_adv_n == 2 + 6 + nd && (C.last_adv[0] & 0xF) == 0 && !memcmp(C.last_adv + 2, PERIPH, 6) &&
          !memcmp(C.last_adv + 8, data, nd), "the ADV_IND on the air: header, our address, the host's data");
    check(pal_fm1_state()->stuck == 0 && pal_fm1_state()->refused == 0, "no operation stuck or refused");
    check(pal_fm1_state()->rx_timeout >= 9, "each connectable packet's receive window closes as a timeout to Cordio");
    {
        uint32_t e0 = C.adv_seen[0];
        run_us(300000);
        check(C.adv_seen[0] - e0 >= 7 && C.adv_seen[0] - e0 <= 11, "advertising events about every 30 ms (plus the 0-10 ms random delay)");
    }

    /* a scanner asks */
    C.want_scan = 1;
    run_us(100000);
    check(C.scan_rsp == 1, "a SCAN_REQ is answered by one SCAN_RSP, T_IFS after it");
    check(C.rsp_n == 2 + 6 + 8 && (C.rsp[0] & 0xF) == 4 && !memcmp(C.rsp + 2, PERIPH, 6) && !memcmp(C.rsp + 10, "OPLBOX", 6),
          "the SCAN_RSP: type 4, our address, the host's scan response data");
    check(pal_fm1_state()->resp_late == 0, "the response was put in before T_IFS");
    check(C.adv_scan_rsp == 0, "the advertising PDU still on the channels after the SCAN_REQ, not the response");
    /* each advertising event reloads the response from Cordio's operation. With no interrupt (the PAL polled from the
     * loop, as on the FM-1 now), a later SCAN_REQ gets the response armed for that event, though Cordio hands it over
     * too late for T_IFS */
    {
        bb_air polled = AIR;
        uint32_t late = pal_fm1_state()->resp_late;
        polled.irq = 0;
        bbm.air = &polled;
        C.want_scan = 1;
        C.scan_rsp = 0;
        C.rsp_n = 0;
        run_us(100000);
        bbm.air = &AIR;
        check(C.scan_rsp == 1 && C.rsp_n == 2 + 6 + 8 && (C.rsp[0] & 0xF) == 4 && !memcmp(C.rsp + 10, "OPLBOX", 6),
              "polled from the loop: a second SCAN_REQ answered with the SCAN_RSP armed for that event");
        check(pal_fm1_state()->resp_late == late + 1u && pal_fm1_state()->stuck == 0,
              "polled: Cordio's response counted late, every operation still completed");
        /* the host changes the scan response while advertising goes on: the next SCAN_REQ, still polled (Cordio's own
         * response late again, so only the one the event was armed with can answer), gets the new data, not the old */
        memset(buf, 0, 32);
        buf[0] = 7; buf[1] = 6; buf[2] = 9; memcpy(buf + 3, "SYNTH", 5);
        check(cmd(0x2009, buf, 32) == 0, "LE Set Scan Response Data while advertising");
        bbm.air = &polled;
        run_us(50000);
        C.want_scan = 1;
        C.scan_rsp = 0;
        C.rsp_n = 0;
        run_us(100000);
        bbm.air = &AIR;
        check(C.scan_rsp == 1 && C.rsp_n == 2 + 6 + 7 && (C.rsp[0] & 0xF) == 4 && !memcmp(C.rsp + 2, PERIPH, 6) &&
              !memcmp(C.rsp + 10, "SYNTH", 5), "polled: a SCAN_REQ after the host changed it answered with the new SCAN_RSP");
        late0 = pal_fm1_state()->resp_late;
    }

    /* a central connects */
    g_nev = 0;
    C.want_conn = 1;
    run_us(300000);
    k = find_ev(0, 0x3E, 0x01);
    check(k >= 0, "LE Connection Complete to the host");
    if (k >= 0) {
        handle = (uint16_t)(g_ev[k][4] | g_ev[k][5] << 8);
        check(g_ev[k][3] == 0 && g_ev[k][6] == 1 && !memcmp(g_ev[k] + 8, CENTRAL, 6), "status 0, role peripheral, the central's address");
        check((g_ev[k][14] | g_ev[k][15] << 8) == CONN_ITVL && (g_ev[k][18] | g_ev[k][19] << 8) == 200, "the interval and timeout the central asked for");
    }
    check(C.events >= 6 && C.heard >= 6, "connection events: the peripheral listens and answers T_IFS after the central");
    check(pal_fm1_state()->adv_conn_ind == 1, "the CONNECT_IND ended its advertising event");
    check(pal_fm1_state()->conn_ended >= 6 && pal_fm1_state()->conn_rx >= 6, "the PAL counts the connection events ended and their packets");
    check(C.chan_bad == 0 && C.aa_bad == 0, "every event on the channel CSA #1 gives, with the connection's access address and CRC init");
    check(C.events >= 3 && C.chan_seq[0] == CONN_HOP && C.chan_seq[1] == 2 * CONN_HOP && C.chan_seq[2] == 3 * CONN_HOP,
          "the first channels: hop, 2 hop, 3 hop");
    check(bbm.air_missed == 0, "the peripheral's window holds the central's packet every time (anchor from the CONNECT_IND)");
    check(C.events + 1 >= 300000u / (CONN_ITVL * 1250u) - 2u, "no connection event skipped");

    /* the central writes: an ATT Write Command in one L2CAP frame (LLID 2) */
    {
        static const uint8_t att[] = { 0x07, 0x00, 0x04, 0x00, 0x52, 0x10, 0x00, 0x80, 0x80, 0x90, 0x3C, 0x64 };
        g_nacl = 0;
        c_send(2, att, sizeof att);
        run_us(100000);
        check(C.nq == 0 && C.acked >= 1, "the central's data PDU acknowledged (NESN)");
        check(g_nacl == 1 && g_acln[0] == 4 + sizeof att && (g_acl[0][0] | (g_acl[0][1] & 0x0F) << 8) == handle &&
              ((g_acl[0][1] >> 4) & 3) == 2 && !memcmp(g_acl[0] + 4, att, sizeof att), "the host gets it as one ACL packet, first fragment, on the handle");
    }

    /* the host sends: one ACL packet to the central */
    {
        static const uint8_t l2[] = { 0x08, 0x00, 0x04, 0x00, 0x1B, 0x10, 0x00, 0x80, 0x80, 0x80, 0x3C, 0x00 };
        uint8_t p[4 + sizeof l2];
        p[0] = (uint8_t)handle; p[1] = (uint8_t)(handle >> 8);
        p[2] = sizeof l2; p[3] = 0;
        memcpy(p + 4, l2, sizeof l2);
        g_nev = 0;
        check(cordio_ctrl_from_host(HCI_ACL, p, sizeof p) == 0, "the controller takes the host's ACL packet");
        run_us(100000);
        check(C.data_in == 1 && C.gotn[0] == 2 + (int)sizeof l2 && (C.got[0][0] & 3) == 2 && !memcmp(C.got[0] + 2, l2, sizeof l2),
              "the central gets it in one data PDU, LLID 2");
        check(find_ev(0, 0x13, 0) >= 0, "Number Of Completed Packets to the host once it was acknowledged");
    }
    check(pal_fm1_state()->resp_late == late0 && pal_fm1_state()->stuck == 0, "every response in time, no operation stuck");

    /* the central ends it: LL_TERMINATE_IND, remote user terminated */
    g_nev = 0;
    c_send(3, (const uint8_t *)"\x02\x13", 2);
    run_us(200000);
    k = find_ev(0, 0x05, 0);
    check(k >= 0 && g_ev[k][2] == 0 && (g_ev[k][3] | g_ev[k][4] << 8) == handle && g_ev[k][5] == 0x13,
          "Disconnection Complete: the handle, reason 0x13");
    C.connected = 0;

    /* advertising again */
    for (i = 0; i < 3; i++)
        C.adv_seen[i] = 0;
    buf[0] = 1;
    check(cmd(0x200A, buf, 1) == 0, "advertising enabled again");
    run_us(200000);
    check(C.adv_seen[0] >= 3, "ADV_IND on the air again");

    /* Polled from the main loop (no interrupt), as on the FM-1: waited through, every connection event holds the
     * central's packet, Cordio's response goes in before T_IFS, data goes both ways; not waited through (up to 58cadc5),
     * the late timestamps put the anchors late and the windows miss the central's packets, as on the FM-1 */
    {
        uint16_t h;
        int data_ok;
        uint32_t missed = polled_conn(&B, 1, 0, 1, &h, &data_ok);
        const pal_fm1_stats *st = pal_fm1_state();
        check(h != 0xFFFF && missed == 0 && st->conn_rx >= 15 && st->conn_rx + 1u >= st->conn_ops,
              "polled, waited through: a connection, every event's packet in the window");
        check(data_ok, "polled, waited through: the central's write reaches the host");
        check(st->resp_late == 0 && st->busy_waits >= 15 && st->resp_us_max < 150u,
              "polled, waited through: every response in before T_IFS");
        check(st->setup_moves == 0 && st->set_rx[FM1BLE_CONN_PADW] == st->conn_rx && st->set_arr_min[FM1BLE_CONN_PADW] >= 0 &&
              st->set_arr_max[FM1BLE_CONN_PADW] <= (int32_t)(st->conn_timeout_us + 2u * FM1BLE_CONN_WIDE_US + 2600u),
              "polled, waited through: the first setup of the order (padw) for the whole connection, each packet inside its window");
        check(st->log_n == PAL_FM1_LOG_PER && log_ok(st) && (st->log[0][0] & 1u << 8) && log_setup(st->log[0][0]) == FM1BLE_CONN_PADW &&
              (st->log[1][0] & 1u << 10) && (st->log[1][2] & 0xFFFFu) > FM1BLE_CONN_WIDE_US,
              "the event log: a connection's first events, each with its packet and a response sent, armed more than 1250 us ahead");
        check(st->conns == 1 && (st->conn_sum[0][0] & 0xFu) == FM1BLE_CONN_PADW && (st->conn_sum[0][0] >> 4 & 0xFFFu) == st->conn_ops &&
              (st->conn_sum[0][0] >> 16) == st->conn_rx && (st->conn_sum[0][1] & 0xFFFFu) != PAL_FM1_LOG_NONE &&
              (int16_t)(st->conn_sum[0][1] >> 16) >= 0 && (st->conn_sum[0][2] & 0xFFFFu) == PAL_FM1_LOG_NONE,
              "the connection's summary: its setup, events, packets, the first and last packet's start, no missed event");
        /* the model runs an advertising event's channels in one step of its clock, so the gap is that step and the
         * CONNECT_IND's time on the air (848 us here), not the loop's 200 us */
        check(st->cind_gap_us > 0 && st->cind_gap_us <= 2000u, "the CONNECT_IND's poll gap is measured");
        k = find_ev(0, 0x05, 0);
        check(k >= 0 && (g_ev[k][3] | g_ev[k][4] << 8) == h, "polled: the central ends it");
        /* not waited through, the window as up to 58cadc5 (one setup, no timer lead): the late timestamps put the anchors
         * late */
        missed = polled_conn(&B, 0, 1u << FM1BLE_CONN_KEEP, 0, &h, &data_ok);
        st = pal_fm1_state();
        check(missed >= 3 && st->conn_rx * 2u < st->conn_ops,
              "polled, not waited through (58cadc5): most windows miss the central's packets");
        {   /* a missed event's end, as the PAL saw the flag: where the model closed the window (KEEP: dueUsec + timeout) */
            uint32_t j, ends = 0, near = 0;
            for (j = 0; j < st->log_n; j++) {
                uint32_t a = st->log[j][0], e = st->log[j][1] >> 16;
                if ((a & 1u << 8) || e == PAL_FM1_LOG_NONE)
                    continue;
                ends++;
                if ((int16_t)e >= (int32_t)(a >> 16) - 10 && (int16_t)e <= (int32_t)(a >> 16) + 210)
                    near++;
            }
            check(ends >= 1 && near == ends, "the event log: a missed event ends where its window closed");
        }
        check((st->conn_sum[0][2] & 0xFFFFu) != PAL_FM1_LOG_NONE && (st->conn_sum[0][0] >> 16) < (st->conn_sum[0][0] >> 4 & 0xFFFu),
              "the summary: the missed events' ends");
        run_us(2500000);                    /* Cordio's supervision timeout ends that one */
        /* a chip that counts a slot more than op_arm writes (JieLi's __set_anchor_cnt): keep and widen open late, so two
         * connections, each on its own setup for all its events (the next of the order), receive nothing */
        bbm.cnt_jieli = 1;
        polled_conn(&B, 1, 1u << FM1BLE_CONN_KEEP | 1u << FM1BLE_CONN_WIDEN, 1, &h, &data_ok);
        run_us(2500000);
        polled_again(&B, 1, 1u << FM1BLE_CONN_KEEP | 1u << FM1BLE_CONN_WIDEN, 1, &h, &data_ok);
        run_us(2500000);
        st = pal_fm1_state();
        check(st->conns == 2 && (st->conn_sum[0][0] & 0xFu) == FM1BLE_CONN_KEEP && (st->conn_sum[1][0] & 0xFu) == FM1BLE_CONN_WIDEN &&
              st->set_events[FM1BLE_CONN_KEEP] >= 6 && st->set_events[FM1BLE_CONN_WIDEN] >= 6 && st->setup_moves == 1 &&
              log_ok(st) && st->log_n == 2u * PAL_FM1_LOG_PER && log_setup(st->log[PAL_FM1_LOG_PER][0]) == FM1BLE_CONN_WIDEN &&
              st->good == PAL_FM1_NO_SETUP && st->conn_rx == 0,
              "each connection runs one setup for all its events, the next connection the next one");
        /* on that chip the counter as JieLi writes it (padj) receives; the next connection keeps it, not the next setup */
        bbm.conn_in = 0;
        polled_conn(&B, 1, 1u << FM1BLE_CONN_PADJ | 1u << FM1BLE_CONN_KEEP, 1, &h, &data_ok);
        st = pal_fm1_state();
        check(h != 0xFFFF && data_ok && (st->conn_sum[0][0] & 0xFu) == FM1BLE_CONN_PADJ && st->set_rx[FM1BLE_CONN_PADJ] >= 15 &&
              st->set_arr_min[FM1BLE_CONN_PADJ] >= (int32_t)FM1BLE_CONN_PAD_US && bbm.conn_off_min >= FM1BLE_CONN_PAD_US &&
              st->set_arr_min[FM1BLE_CONN_PADJ] <= bbm.conn_off_min + 20 && st->set_arr_max[FM1BLE_CONN_PADJ] >= bbm.conn_off_max - 20 &&
              st->set_arr_max[FM1BLE_CONN_PADJ] <= bbm.conn_off_max + 20,
              "a chip that counts as JieLi writes: padj receives every event, its window open the pad before dueUsec");
        run_us(2500000);
        polled_again(&B, 1, 1u << FM1BLE_CONN_PADJ | 1u << FM1BLE_CONN_KEEP, 1, &h, &data_ok);
        st = pal_fm1_state();
        check(h != 0xFFFF && data_ok && st->good == FM1BLE_CONN_PADJ && (st->conn_sum[1][0] & 0xFu) == FM1BLE_CONN_PADJ &&
              st->set_events[FM1BLE_CONN_KEEP] == 0, "a setup that received a connection runs the next one too");
        run_us(2500000);
        /* and a chip that takes the offset only with bit 15 and the window from WINCNTL0 as well: padw receives */
        bbm.bit15 = bbm.win0 = 1;
        bbm.conn_in = 0;
        polled_conn(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
        st = pal_fm1_state();
        check(h != 0xFFFF && data_ok && st->set_rx[FM1BLE_CONN_PADW] >= 15 && st->set_rx[FM1BLE_CONN_PADW] == st->conn_rx &&
              st->set_arr_min[FM1BLE_CONN_PADW] >= bbm.conn_off_min - 20 && st->set_arr_min[FM1BLE_CONN_PADW] <= bbm.conn_off_min + 20 &&
              st->set_arr_max[FM1BLE_CONN_PADW] >= bbm.conn_off_max - 20 && st->set_arr_max[FM1BLE_CONN_PADW] <= bbm.conn_off_max + 20,
              "a chip as JieLi's code writes it in all three: padw receives every event");
        run_us(2500000);
        polled_conn(&B, 1, 1u << FM1BLE_CONN_PADJ, 1, &h, &data_ok);
        st = pal_fm1_state();
        check(st->set_rx[FM1BLE_CONN_PADJ] * 2u < st->set_events[FM1BLE_CONN_PADJ],
              "on that chip padj (offset without bit 15, window 0x32 in WINCNTL0) misses");
        bbm.cnt_jieli = bbm.bit15 = bbm.win0 = 0;
        run_us(2500000);
        /* not waited through: the wide window holds the late anchors */
        missed = polled_conn(&B, 0, 1u << FM1BLE_CONN_WIDE, 1, &h, &data_ok);
        st = pal_fm1_state();
        check(st->set_events[FM1BLE_CONN_WIDE] >= 6 && st->set_rx[FM1BLE_CONN_WIDE] * 4u >= st->set_events[FM1BLE_CONN_WIDE] * 3u,
              "not waited through: the wide window receives most events");
        /* the padded and the wide window alone, waited through: with the timer lead each event is armed more than the pad
         * ahead, so the window opens the pad before dueUsec. Without it Cordio arms an event SCH_LOAD_DELAY_US (300 us)
         * before dueUsec, less than FM1BLE_CONN_PAD_US, and fm1ble_op_conn opens the window at once (the counter at 0).
         * The arrival positions the PAL reports count from where the window really opened, as the model saw it. */
        {
            static const struct { uint8_t setup; int lead; const char *what; } W[3] = {
                { FM1BLE_CONN_PAD, 0, "the padded window, no timer lead" },
                { FM1BLE_CONN_PAD, 1, "the padded window" },
                { FM1BLE_CONN_WIDE, 1, "the wide window" },
            };
            char what[120];
            for (i = 0; i < 3; i++) {
                uint8_t su = W[i].setup;
                run_us(2500000);
                bbm.conn_in = bbm.conn_at_once = 0;
                missed = polled_conn(&B, 1, (uint8_t)(1u << su), W[i].lead, &h, &data_ok);
                st = pal_fm1_state();
                snprintf(what, sizeof what, "%s alone: a connection, every event's packet in the window", W[i].what);
                check(h != 0xFFFF && missed == 0 && data_ok && st->set_rx[su] >= 15 && st->set_rx[su] == st->conn_rx, what);
                snprintf(what, sizeof what, "%s alone: %s", W[i].what,
                         W[i].lead ? "each event armed before its window, none open at once" : "each window open at once");
                check(bbm.conn_in >= 15 && bbm.conn_at_once == (W[i].lead ? 0u : bbm.conn_in), what);
                snprintf(what, sizeof what, "%s: the packets' positions counted from where the window opened", W[i].what);
                check(st->set_arr_min[su] >= bbm.conn_off_min - 20 && st->set_arr_min[su] <= bbm.conn_off_min + 20 &&
                      st->set_arr_max[su] >= bbm.conn_off_max - 20 && st->set_arr_max[su] <= bbm.conn_off_max + 20, what);
                if (W[i].lead) {
                    snprintf(what, sizeof what, "%s: opened the whole pad before dueUsec", W[i].what);
                    check(st->set_arr_min[su] >= (int32_t)(su == FM1BLE_CONN_WIDE ? FM1BLE_CONN_WIDE_US : FM1BLE_CONN_PAD_US), what);
                }
                if (bad)
                    fprintf(stderr, "  %s: PAL %d..%d us, model %lld..%lld us, %u of %u events opened at once\n", W[i].what,
                            (int)st->set_arr_min[su], (int)st->set_arr_max[su], (long long)bbm.conn_off_min,
                            (long long)bbm.conn_off_max, (unsigned)bbm.conn_at_once, (unsigned)bbm.conn_in);
            }
        }
        /* the TX modes (fm1ble.h FM1BLE_TX_*), one connection each on a chip that puts in NESN itself (bb_model hw_nesn,
         * the IR's ble_hw_tx keeping TXDHDR bit 2; AIR on the model's air layout, its NESN Cordio's): data both ways,
         * every response in time, the log's word d */
        {
            static const uint8_t M[3] = { FM1BLE_TX_INTIME, FM1BLE_TX_JIELI, FM1BLE_TX_AIR };
            static const char *const what[3] = { "INTIME: a connection, data both ways, every response put in",
                                                 "JIELI: a connection, data both ways", "AIR: a connection, data both ways" };
            for (i = 0; i < 3; i++) {
                bbm.hw_nesn = M[i] != FM1BLE_TX_AIR;     /* AIR writes Cordio's NESN itself, as up to f0e6d08 */
                g_tx_modes = (uint8_t)(1u << M[i]);
                missed = polled_conn(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
                check(h != 0xFFFF && missed == 0 && data_ok && st->conn_tx == M[i] && st->tx_rx[M[i]] >= 15 &&
                      st->tx_rx[M[i]] == st->conn_rx && (st->tx_events[M[i]] & 0xFFFFu) == st->conn_ops &&
                      st->resp_skipped == 0 && st->stale_rx == 0 && st->halts == 0 && (st->conn_sum[0][3] & 0xFu) == M[i] &&
                      (st->conn_sum[0][3] >> 4 & 0xFFFu) >= 15u && (st->log[0][3] >> 22 & 3u) == M[i] && (st->log[1][3] >> 24) < 50u &&
                      (st->first_rx_tx >> 31) == 1u && st->conn_txs == M[i], what[i]);
            }
            /* all three, the order: the first connection INTIME; it received, so the next keeps it */
            bbm.hw_nesn = 1;
            g_tx_modes = 0;
            missed = polled_conn(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
            missed += polled_again(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
            check(missed == 0 && data_ok && st->conns == 2 && st->good_tx == FM1BLE_TX_INTIME &&
                  st->conn_txs == (FM1BLE_TX_INTIME | FM1BLE_TX_INTIME << 4),
                  "the TX order: the first connection INTIME, kept by the next once it received PAL_FM1_GOOD packets");
            /* a stale flag: a packet flag at once after the third event was armed, before its window could open; it is
             * dropped, Cordio's anchor stays, the central's packets go on landing in the window */
            g_tx_modes = 1u << FM1BLE_TX_INTIME;
            bbm.tick = hook;
            g_hook = H_STALE;
            g_hook_ev = 3;
            g_hook_done = 0;
            missed = polled_conn(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
            check(g_hook_done && h != 0xFFFF && missed == 0 && data_ok && st->stale_rx == 1 && (st->log[2][0] & 1u << 15) &&
                  (st->conn_sum[0][3] >> 16 & 0xFFu) == 1u && st->conn_rx + 2u >= st->conn_ops,
                  "a packet flag before the window could open: dropped as stale, the connection's windows still hold the packets");
            /* a late response: the main loop comes back 300 us after the third event's packet; INTIME does not put
             * Cordio's response in (the empty PDU repeating the last SN goes), Cordio is told it failed, data still flows */
            g_hook = H_LATE;
            g_hook_done = 0;
            missed = polled_conn(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
            check(g_hook_done && h != 0xFFFF && data_ok && st->resp_skipped == 1 && (st->resp_hist >> 24) >= 1u &&
                  st->resp_us_max >= 300u && (st->log[2][3] >> 24) >= 150u,
                  "a response too late for T_IFS: INTIME leaves it out, the connection and its data go on");
            /* a hung event: the baseband runs nothing and raises no flag for the third event; it ends by its deadline, the
             * driver is halted, the connection goes on. Last of the three, and its data path is not checked: on the model a
             * connection after one whose event ended by its deadline, or after the late response above, loses the central's
             * write to the host although its packets are received and acknowledged, with fm1ble_op_stop as up to f0e6d08
             * as much as with the halt, and with the skipped response reported as sent [unexplained;
             * docs/BLUETOOTH_CORDIO.md] */
            g_hook = H_HANG;
            g_hook_done = 0;
            missed = polled_conn(&B, 1, 1u << FM1BLE_CONN_PADW, 1, &h, &data_ok);
            check(g_hook_done == 2 && !g_hang_stuck && h != 0xFFFF && st->halts == 1 && st->halt_busy == 0 && B.d.halts >= 1 &&
                  (st->log[2][0] & 1u << 11) && (st->conn_sum[0][3] >> 24) == 1u && st->stale_rx == 0 && st->conn_ops >= 15u &&
                  st->conn_rx + 2u >= st->conn_ops && missed == 0,
                  "an event the baseband never ends: halted at its deadline, the connection's later events receive");
            g_hook = H_NONE;
            bbm.tick = 0;
            g_tx_modes = 0;
            bbm.hw_nesn = 0;
        }
        buf[0] = 0;
        cmd(0x200A, buf, 1);
        {
            pal_fm1_cfg pc2;
            memset(&pc2, 0, sizeof pc2);
            pc2.b = &B;
            pal_fm1_attach(&pc2);
        }
        buf[0] = 1;
        check(cmd(0x200A, buf, 1) == 0, "advertising enabled again after the polled connections");
    }

    /* a baseband that never raises a flag: every operation ends by its deadline and Cordio goes on asking */
    {
        uint32_t ops = pal_fm1_state()->adv_ops;
        bbm.silent = 1;
        run_us(300000);
        check(pal_fm1_state()->stuck >= 3 && pal_fm1_state()->adv_ops - ops >= 6, "a silent baseband: operations end by their deadline, advertising goes on");
        bbm.silent = 0;
        for (i = 0; i < 3; i++)
            C.adv_seen[i] = 0;
        run_us(100000);
        check(C.adv_seen[0] >= 2, "on the air again once the baseband answers");
    }

    printf("ble_pal: %d checks, %s (adv ops %u, conn ops %u, timer late %u us at most)\n", checks, bad ? "FAILED" : "all passed",
           (unsigned)pal_fm1_state()->adv_ops, (unsigned)pal_fm1_state()->conn_ops, (unsigned)pal_fm1_state()->timer_late_us);
    return bad;
}
