/* SPDX-License-Identifier: GPL-3.0-only
 * A central on the register model's air (bb_model.h, bb_air), for the host tests of Cordio on the PAL (tests/ble_pal.c,
 * tests/ble_stack.c): it hears the peripheral's advertising packets, answers one with a SCAN_REQ or a CONNECT_IND when
 * asked, then runs the connection as the Core Specification says (Vol 6 Part B: the transmit window 4.5.3, channel
 * selection algorithm #1 4.5.8.2, SN and NESN 4.5.9): an empty PDU or the next queued data PDU at each anchor, the
 * peripheral's answers kept in order. With burst set it sends its queue in one event as BlueZ did at 1b251ea (LL_LENGTH_REQ
 * with MD set, its first L2CAP PDU T_IFS after the response): MD set while more is queued after the packet, the next packet
 * after a response it heard (4.5.6). Own code. Host only. */
#ifndef OPLBOX_TESTS_BLE_AIR_CENTRAL_H
#define OPLBOX_TESTS_BLE_AIR_CENTRAL_H

#include <stdint.h>
#include "bb_model.h"

extern const uint8_t CENTRAL[6];
#define CONN_AA         0x50654A9Bul    /* an access address the specification's rules allow (Vol 6 Part B 2.1.2) */
#define CONN_CRC        0x3A5C21ul
#define CONN_ITVL       24u             /* 30 ms */
#define CONN_HOP        7u
#define CONN_WINOFF     2u              /* 2.5 ms */
#define CONN_WINSIZE    2u              /* 2.5 ms */

typedef struct {
    int want_scan, want_conn;           /* answer the next advertising packet with a SCAN_REQ / CONNECT_IND */
    int scan_all;                       /* an active scan (BlueZ's scan le): a SCAN_REQ to every ADV_IND heard */
    uint32_t scan_req_to[256];          /* SCAN_REQs sent, by the first byte of the advertiser address */
    uint32_t adv_seen[3];               /* ADV_IND heard on 37, 38, 39 */
    uint32_t adv_from[256];             /* ADV_IND heard, by the first (least significant) byte of the advertiser address */
    uint32_t adv_short;                 /* ADV_IND too short for its address, or its length byte not the PDU's */
    uint32_t adv_scan_rsp;              /* a SCAN_RSP sent where an advertising PDU belongs */
    uint8_t last_adv[40];
    int last_adv_n;
    uint8_t chan_order[16];
    uint64_t chan_t[16];
    int nchan;
    uint32_t scan_rsp;
    uint8_t rsp[40];
    int rsp_n;
    /* the connection */
    int connected;
    uint64_t anchor0;                   /* the central's first anchor */
    uint32_t next_k, unmapped;
    uint8_t sn, nesn;
    uint8_t last[40];                   /* the PDU last sent (LLID, length, payload), sent again until acknowledged (4.5.9) */
    int inflight, last_data;            /* it was sent and not acknowledged yet; it is the queue's head (not an empty PDU) */
    int burst;                          /* MD set while more is queued: the queue in one event (above) */
    uint8_t md;                         /* the last packet's MD */
    int heard_last;                     /* the response to the last packet was heard */
    int in_event;                       /* packets sent in this event (at most 8: the event's length) */
    uint8_t q[16][40];                  /* data PDUs to send (header byte LLID only, length, payload) */
    int nq;
    uint32_t events, heard, acked, chan_bad, aa_bad, data_in;
    uint8_t got[64][40];                /* data PDUs heard (the whole PDU), in order */
    int gotn[64], ngot;
    uint8_t chan_seq[8];
} central;

extern central C;
/* the air with the central on it; irq is the test's interrupt handler (pal_fm1_service) */
extern const bb_air AIR;
void air_central_irq(void *ctx);
/* queue a data PDU: LLID (1 continuation, 2 start of an L2CAP frame, 3 control), payload */
void c_send(uint8_t llid, const uint8_t *p, uint8_t n);

#endif
