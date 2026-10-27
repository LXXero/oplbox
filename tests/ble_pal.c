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

static const uint8_t PERIPH[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };   /* least significant first */


int main(void)
{
    static uint8_t blk[FM1BLE_BLOCK_SIZE] __attribute__((aligned(4)));
    static fm1ble B;
    cordio_ctrl_cfg c;
    pal_fm1_cfg pc;
    uint8_t buf[40], data[31], nd;
    uint16_t handle = 0xFFFF;
    int i, k;

    bb_model_reset();
    check(fm1ble_radio_up(&B, 0, NULL) == FM1BLE_OK && fm1ble_bb_init(&B, blk) == FM1BLE_OK, "the driver comes up on the model");
    bbm.air = &AIR;
    memset(&pc, 0, sizeof pc);
    pc.b = &B;
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
    /* inside an event Cordio puts the next channel the scheduler's setup delay (2 ms here) after the last one's end: the
     * packet goes when Cordio said, not when the PAL was asked, and well inside the 10 ms the specification allows */
    check(C.nchan >= 3 && C.chan_t[1] - C.chan_t[0] >= 2000u && C.chan_t[1] - C.chan_t[0] < 10000u &&
          C.chan_t[2] - C.chan_t[1] >= 2000u && C.chan_t[2] - C.chan_t[1] < 10000u, "the packets of an event 2 to 10 ms apart, at Cordio's times");
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
    check(pal_fm1_state()->resp_late == 0 && pal_fm1_state()->stuck == 0, "every response in time, no operation stuck");

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

    /* a baseband that never raises a flag: every operation ends by its deadline and Cordio goes on asking */
    {
        uint32_t ops = pal_fm1_state()->adv_ops;
        bbm.silent = 1;
        run_us(200000);
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
