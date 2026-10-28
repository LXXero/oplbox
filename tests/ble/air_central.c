/* SPDX-License-Identifier: GPL-3.0-only
 * See air_central.h. */
#include <string.h>
#include "air_central.h"

const uint8_t CENTRAL[6] = { 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC6 };

central C;

static int c_adv(void *ctx, uint64_t t, uint8_t chan, const uint8_t *pdu, int n, uint8_t *req)
{
    (void)ctx;
    if ((pdu[0] & 0xF) == 0 && (n < 8 || pdu[1] != n - 2))
        C.adv_short++;
    if (chan >= 37 && chan <= 39 && n >= 8 && (pdu[0] & 0xF) == 0) {
        C.adv_seen[chan - 37]++;
        C.adv_from[pdu[2]]++;
        memcpy(C.last_adv, pdu, (size_t)n);
        C.last_adv_n = n;
        if (C.nchan < 16) {
            C.chan_t[C.nchan] = t;
            C.chan_order[C.nchan++] = chan;
        }
    }
    if (C.want_scan || (C.scan_all && n >= 8 && (pdu[0] & 0xF) == 0)) {   /* SCAN_REQ: ScanA, AdvA (Vol 6 Part B 2.3.2.1) */
        C.want_scan = 0;
        C.scan_req_to[pdu[2]]++;
        req[0] = 3;
        req[1] = 12;
        memcpy(req + 2, CENTRAL, 6);
        memcpy(req + 8, pdu + 2, 6);
        return 14;
    }
    if (C.want_conn) {                  /* CONNECT_IND: InitA, AdvA, LLData (2.3.3.1) */
        uint8_t *d = req + 14;
        uint64_t end;
        C.want_conn = 0;
        req[0] = 5;
        req[1] = 34;
        memcpy(req + 2, CENTRAL, 6);
        memcpy(req + 8, pdu + 2, 6);
        d[0] = (uint8_t)CONN_AA; d[1] = (uint8_t)(CONN_AA >> 8); d[2] = (uint8_t)(CONN_AA >> 16); d[3] = (uint8_t)(CONN_AA >> 24);
        d[4] = (uint8_t)CONN_CRC; d[5] = (uint8_t)(CONN_CRC >> 8); d[6] = (uint8_t)(CONN_CRC >> 16);
        d[7] = CONN_WINSIZE;
        d[8] = CONN_WINOFF; d[9] = 0;
        d[10] = CONN_ITVL; d[11] = 0;
        d[12] = 0; d[13] = 0;                               /* latency */
        d[14] = 200; d[15] = 0;                             /* supervision timeout 2 s */
        d[16] = 0xFF; d[17] = 0xFF; d[18] = 0xFF; d[19] = 0xFF; d[20] = 0x1F;   /* every data channel */
        d[21] = CONN_HOP | 0 << 5;                          /* hop, SCA 0 (251-500 ppm) */
        /* the transmit window starts 1.25 ms + WinOffset after the end of the CONNECT_IND (4.5.3); send 400 us into it */
        end = t + bb_model_air_us(n) + 150u + bb_model_air_us(36);
        C.anchor0 = end + 1250u + CONN_WINOFF * 1250u + 400u;
        C.connected = 1;
        C.next_k = 0;
        C.unmapped = 0;
        C.sn = C.nesn = 0;
        return 36;
    }
    return 0;
}

static void c_scan_rsp(void *ctx, uint64_t t, const uint8_t *pdu, int n)
{
    (void)ctx; (void)t;
    C.scan_rsp++;
    memcpy(C.rsp, pdu, (size_t)n);
    C.rsp_n = n;
}

static int c_conn(void *ctx, uint64_t t, uint32_t win, uint8_t chan, uint32_t aa, uint32_t crc, uint8_t *pdu, uint64_t *at)
{
    uint64_t a;
    uint8_t want;
    (void)ctx; (void)win;
    if (!C.connected)
        return 0;
    /* the events the peripheral did not listen to went by: the central sent into the air and hopped on */
    for (;;) {
        a = C.anchor0 + (uint64_t)C.next_k * CONN_ITVL * 1250u;
        C.unmapped = (C.unmapped + CONN_HOP) % 37u;         /* CSA #1 with all 37 channels used (4.5.8.2) */
        want = (uint8_t)C.unmapped;
        if (a + 1250u >= t)
            break;
        C.next_k++;
    }
    C.next_k++;
    C.events++;
    if (C.events <= 8)
        C.chan_seq[C.events - 1] = chan;
    if (chan != want) {
        C.chan_bad++;
        return 0;
    }
    if (aa != CONN_AA || crc != CONN_CRC) {
        C.aa_bad++;
        return 0;
    }
    if (C.nq) {
        pdu[0] = C.q[0][0];
        pdu[1] = C.q[0][1];
        memcpy(pdu + 2, C.q[0] + 2, pdu[1]);
    } else {
        pdu[0] = 1;                                          /* an empty PDU: LLID 1, length 0 */
        pdu[1] = 0;
    }
    pdu[0] = (uint8_t)((pdu[0] & 3u) | C.nesn << 2 | C.sn << 3);
    *at = a;
    return 2 + pdu[1];
}

static void c_conn_rsp(void *ctx, uint64_t t, const uint8_t *pdu, int n)
{
    uint8_t sn = (pdu[0] >> 3) & 1u, nesn = (pdu[0] >> 2) & 1u;
    (void)ctx; (void)t;
    C.heard++;
    if (nesn != C.sn) {                                     /* our packet was acknowledged (4.5.9) */
        C.acked++;
        C.sn ^= 1u;
        if (C.nq) {
            memmove(C.q, C.q + 1, sizeof C.q[0] * 15);
            C.nq--;
        }
    }
    if (sn == C.nesn) {                                     /* new data from the peripheral */
        C.nesn ^= 1u;
        if (pdu[1] && C.ngot < 64) {
            memcpy(C.got[C.ngot], pdu, (size_t)n);
            C.gotn[C.ngot++] = n;
            C.data_in++;
        }
    }
}

void c_send(uint8_t llid, const uint8_t *p, uint8_t n)
{
    if (C.nq >= 16)
        return;
    C.q[C.nq][0] = llid;
    C.q[C.nq][1] = n;
    memcpy(C.q[C.nq] + 2, p, n);
    C.nq++;
}

const bb_air AIR = { c_adv, c_scan_rsp, c_conn, c_conn_rsp, air_central_irq, 0 };
