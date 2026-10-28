/* SPDX-License-Identifier: GPL-3.0-only
 * Cordio's PAL on the FM-1's baseband (pal_fm1.c): what the app gives it and what it reports. See pal_fm1.c. */
#ifndef OPLBOX_BLE_PAL_FM1_H
#define OPLBOX_BLE_PAL_FM1_H

#include <stdint.h>
#include "fm1ble.h"

typedef struct {
    fm1ble *b;                          /* the driver, powered up and its block open (fm1ble_radio_up, fm1ble_bb_init) */
    uint32_t (*rand32)(void *ctx);      /* random numbers (LE Rand, the controller's seeds); NULL: a counter-fed generator */
    void *ctx;
    uint16_t sch_setup_us;              /* how long before an operation the scheduler's timer fires: 0 = 2000 us (main-loop
                                           polling, not an interrupt: a guess at the loop's latency [measure on FM-1]) */
    uint8_t no_busy_wait;               /* 1: a connection event is polled like the rest (up to 58cadc5), not waited through */
    uint8_t no_timer_lead;              /* 1: the scheduler's timer fires when Cordio asks (up to a7c1738), not
                                           PAL_FM1_TIMER_LEAD_US earlier */
    uint8_t conn_setups;                /* the driver's window setups to try (bit n: FM1BLE_CONN_* n); 0: all seven */
    uint8_t tx_modes;                   /* the driver's TX modes to try (bit n: FM1BLE_TX_* n); 0: all four */
    uint8_t arq_ways;                   /* ARQ's ways to try (PAL_FM1_ARQ_TIFS, PAL_FM1_ARQ_AHEAD); 0: both, in turn */
} pal_fm1_cfg;

/* The order the window setups are tried in: each connection runs one setup for all its events, the next connection the
 * next enabled one in this order (the first connection the first), so a run whose central connects again after each drop
 * (tools/ble_central.py, the test run's MIDI_IN step) tries one per connection. At a7c1738 a move after every missed event
 * gave no setup two events in a row; at 5474159 a move after 3 in a row left PAD, which had received, for KEEP before the
 * anchor settled. A connection in which a setup received PAL_FM1_GOOD packets makes it the one every later connection runs.
 * PADW, PADB and PADJ first: each adds one fact of the IR to PAD (fm1ble.h), PAD and WIDE after them to compare. */
#define PAL_FM1_SETUP_ORDER { FM1BLE_CONN_PADW, FM1BLE_CONN_PADB, FM1BLE_CONN_PADJ, FM1BLE_CONN_PAD, FM1BLE_CONN_WIDE, \
                              FM1BLE_CONN_KEEP, FM1BLE_CONN_WIDEN }
#define PAL_FM1_GOOD        8u
#define PAL_FM1_NO_SETUP    0xFFu

/* The TX modes (fm1ble.h FM1BLE_TX_*), one per connection in this order, alongside the window setup: the k-th connection
 * since the attach runs the k-th enabled one. A connection in which the central sent an L2CAP PDU (LLID 2: its controller
 * and host got past the link layer's own procedures, so both directions worked) makes its mode the one every later
 * connection runs; packets alone do not (at a349877 INTIME received 462 and was kept, and no GATT came). ARQ first (the
 * IR's reading, fm1ble.h), AIR (the air's layout written as it is) to compare, then INTIME and JIELI. */
#define PAL_FM1_TX_ORDER    { FM1BLE_TX_ARQ, FM1BLE_TX_AIR, FM1BLE_TX_INTIME, FM1BLE_TX_JIELI }

/* ARQ's two ways of writing Cordio's response (since round 7 of the FM-1 Bluetooth goal), one per ARQ connection, ahead
 * first, then in T_IFS, in turn; a connection on which the central sent PAL_FM1_L2_GOOD L2CAP PDUs Cordio took (an ATT
 * request after the MTU exchange and one after that: Cordio's answers got through) makes its way the one every later ARQ
 * connection runs.
 *   in T_IFS (as up to e3d2a74): into the slot TXTOG names, which the hardware sends T_IFS after the packet; Cordio is told
 *            its PDU was acknowledged when TXTOG has moved since. At e3d2a74 (oplbox-ble-runs/e3d2a74-1) NimBLE's first
 *            L2CAP PDU (its connection parameter request, put in 70 us after the packet) never got an answer from
 *            BlueZ, nor did the 5-byte ATT answer to BlueZ's second request, though the chip moved TXTOG past both: a
 *            write after the hardware read the slot does not go, and the next acknowledgement looks like its own.
 *   ahead:   into the other slot (fm1ble.h fm1ble_op_arq_ahead), which the hardware sends once the central acknowledges
 *            the one it sends now; Cordio is told its PDU was acknowledged when TXTOG names the slot it went into (the
 *            chip sends it from then on until the central takes it), so its next one goes in behind it. No write races
 *            the hardware, and no response is too late: one after the event goes in for the next. */
#define PAL_FM1_L2_GOOD     3u
#define PAL_FM1_ARQ_TIFS    1u
#define PAL_FM1_ARQ_AHEAD   2u

/* The chip's SN (fm1ble.h fm1ble_op_arq_flip). A new connection's first packet has NESN 0 (Vol 6 Part B 4.5.9): it
 * acknowledges nothing, so Cordio is told so; when TXTOG moved at it anyway, the chip's own SN state was left at 1 by the
 * last connection (at e3d2a74 the second connection's first packet moved it, and the central took every one of Cordio's
 * PDUs after it as a repeat: 244 sent again, RXTOG moving at the central's packets, TXTOG never), so bit 2 of both slots
 * is changed. And PAL_FM1_ARQ_STUCK packets in a row at which RXTOG moved (the central heard the chip's NESN) and TXTOG
 * did not (it took nothing the chip sent) change it too. */
#define PAL_FM1_ARQ_STUCK   3u

/* The PDU capture: the first PAL_FM1_CAP data PDUs with a payload since pal_fm1_attach, either way, PAL_FM1_CAP_BYTES
 * each: the central's as the chip received them (new to Cordio, good CRC) and Cordio's as it handed them over (one entry
 * for a PDU handed again unchanged). For the bytes of what went each way: at e3d2a74 the header trace showed BlueZ's two
 * L2CAP PDUs (3 and 7 bytes) and NimBLE's three (12, 3 and 5), and GATT never resolved. An entry:
 *   0: bit 0 Cordio's (1) or the central's (0); bit 1 the central's with a good CRC, or Cordio's written ahead; bit 2 the
 *      central's new to Cordio, or Cordio's put in; bit 3 the central's a later packet of its event; bits 7-4 the
 *      connection since the attach (0-15, 15 and later 15);
 *   1: the header bits 4-0: RXDHDR's as the chip wrote them, or Cordio's header byte's;
 *   2: the length; 3-4: the connection's event (u16, the first 0);
 *   5: Cordio's: its response time after the packet was seen, 2 us units (0xFF: no packet, or 510 us and more); the
 *      central's: bit 0 RXTOG changed at it, bit 1 TXTOG changed at it;
 *   6-19: the payload's first 14 bytes (the L2CAP header and 10 bytes of ATT or signalling). */
#define PAL_FM1_CAP         32
#define PAL_FM1_CAP_BYTES   20

/* FM1BLE_TX_INTIME: Cordio's response goes in only this many us after the PAL saw the packet. The flag comes at the
 * packet's end [unchecked]; T_IFS is 150 us and the hardware reads the slot before its preamble. Cordio took 42 us at most
 * at a7c1738 and 251 at f0e6d08. */
#define PAL_FM1_TIFS_US     100u

/* A connection packet that starts this many us or more before its window opened (dueUsec less the setup's early arm) is
 * not the central's packet for this event: it is a flag an earlier event raised late. It is counted (stale_rx) and not
 * given to Cordio, which would take it as the anchor. At f0e6d08 one came 1192 us before dueUsec (567 us before PADW's
 * window) and every later window of the connection missed; PADW's window opened up to 236 us early in the probe. */
#define PAL_FM1_STALE_US    400u

/* How much earlier than Cordio asks the scheduler's timer fires. Cordio starts an operation SCH_LOAD_DELAY_US (300 us)
 * before its dueUsec, less the timer's lateness from the main loop (up to 210 us at a7c1738), so a connection event was
 * armed 90-300 us ahead and the PAD and WIDE setups could not open their windows earlier than at once. With the lead the
 * event is armed about FM1BLE_CONN_WIDE_US + 300 us ahead. Cordio loads a BOD whose due time is in the future
 * (sch_main.c schLoadBod), so an early load only moves the PAL's arm and busy wait earlier. */
#define PAL_FM1_TIMER_LEAD_US   1250u

/* The event log: the first PAL_FM1_LOG_PER events of each connection since pal_fm1_attach, PAL_FM1_LOG in all, four
 * words each.
 *   a: the channel (bits 5-0), the setup (7-6 its low two bits, 14 its third: FM1BLE_CONN_PADJ and up), flags (8 a
 *      packet came, 9 its CRC was good, 10 TXTOG changed in the event, 11 the event ended by its deadline not the
 *      baseband's flag, 12 Cordio went on before it ended, 13 the first event of a connection, 15 a stale packet flag
 *      came and was dropped, PAL_FM1_STALE_US), Cordio's receive timeout in us (31-16, at most 65535);
 *   b: when the packet started (15-0) and when the PAL saw the event's end (31-16), us after Cordio's dueUsec, signed
 *      16-bit (PAL_FM1_LOG_NONE: no packet, or no end seen);
 *   c: the delay the event was armed with, us from now to dueUsec (15-0, at most 65535), the packet's RXDHDR (31-16);
 *   d: the TX side (fm1ble_op_txstate) at the event's end: TXDHDR0 (7-0) and TXDHDR1 (15-8) low bytes, TXTOG bit 0 at the
 *      arm (16) and at the end (17), RXTOG bit 0 at the arm (18) and at the end (19), TXBUF0CNTL (20) and TXBUF1CNTL (21)
 *      bit 0 at the end, the TX mode (23-22), Cordio's response in 2 us units after the packet was seen (31-24, 0xFF:
 *      none, or 510 us and more). */
#define PAL_FM1_LOG         12
#define PAL_FM1_LOG_PER     4
#define PAL_FM1_LOG_NONE    0x8000u

/* Each connection's summary, for the first PAL_FM1_CONNS since pal_fm1_attach, four words:
 *   a: the setup (bits 3-0), the connection events (15-4, at most 4095), the packets received in them (31-16);
 *   b: the first packet's start (15-0) and the last one's (31-16), us after Cordio's dueUsec of its event, signed 16-bit;
 *   c: of the events that missed their packet, the earliest (15-0) and latest (31-16) the baseband's end flag came, us
 *      after dueUsec, signed 16-bit (where the chip really closed the window);
 *   d: the TX mode (3-0), the events in which TXTOG changed (15-4, at most 4095), stale packets dropped (23-16) and events
 *      ended by their deadline (31-24), each at most 255;
 * PAL_FM1_LOG_NONE where there is none. */
#define PAL_FM1_CONNS       8

/* The header trace: for the first PAL_FM1_TRACE_PER connection events with a packet of each of the first
 * PAL_FM1_TRACE_CONNS connections since pal_fm1_attach, one word:
 *   bits 4-0 RXDHDR bits 4-0 as the chip wrote them; 5 the packet's CRC was good; 6 RXTOG changed at it; 7 TXTOG had
 *   changed since Cordio's last response went in; 15-8 the payload's first byte (a control PDU's opcode; 0 when empty);
 *   20-16 Cordio's response's header bits 4-0 (LLID, NESN, SN, MD as Cordio built them); 21 Cordio handed a response;
 *   22 it went into a slot; 23 the header Cordio got acknowledged its last PDU (its NESN differs from Cordio's SN);
 *   24 it was new to Cordio (its SN equals Cordio's NESN); 31-25 the response's first payload byte, low 7 bits.
 * And for each of the first PAL_FM1_CONNS connections: ll_ops, the control opcodes 0-31 the central sent (bit n: opcode n,
 * good CRC); ll_cnt, the central's L2CAP PDUs (LLID 2) Cordio took as new (7-0), Cordio's non-empty responses put in
 * (15-8), the packets Cordio was told did not acknowledge its last PDU (23-16), RXTOG's changes (31-24), each at most 255.
 * arq_dup and arq_unwritten (ARQ only): packets Cordio was told were repeats (RXTOG unchanged once it had moved), and
 * responses that did not go in (too late, or after the event), so Cordio was told its PDU was not acknowledged.
 * Packets after the first in a connection event (since round 6 of the FM-1 Bluetooth goal: at 1b251ea BlueZ sent
 * LL_LENGTH_REQ with MD set and its first L2CAP PDU T_IFS after the response, in the same event; the chip's ARQ
 * acknowledged it on the air, Cordio had closed the event and never got it, so no ATT request reached NimBLE):
 * later_rx such packets, later_given the ones given to Cordio. */
#define PAL_FM1_TRACE_CONNS 2
#define PAL_FM1_TRACE_PER   20

typedef struct {
    uint32_t adv_ops, conn_ops;         /* Cordio's operations: advertising packets, connection events */
    uint32_t adv_events;                /* hardware advertising events armed (37, 38 and 39 each) */
    uint32_t adv_covered;               /* advertising packets that went in an event already armed (38 and 39 of each) */
    uint32_t adv_ended;                 /* advertising events the baseband ended (its event flag) */
    uint32_t adv_conn_ind;              /* advertising events a CONNECT_IND ended */
    uint32_t adv_done[3];               /* advertising packets completed to Cordio (txCback), per channel 37, 38, 39 */
    uint32_t conn_ended, conn_rx;       /* connection events the baseband ended; packets received in them */
    uint32_t conn_timeout_us;           /* Cordio's receive timeout for the last connection event (the window it got) */
    uint32_t tx_done, rx_ok, rx_bad, rx_timeout;   /* completions given to Cordio */
    uint32_t resp_set, resp_late;       /* responses put in for T_IFS; ones asked for after the event had ended */
    uint32_t timer_fired, timer_late_us;/* the scheduler's timer: fired, the most it was late */
    uint32_t refused;                   /* operations the driver refused (a bad channel, a PDU too long) */
    uint32_t late_ops;                  /* operations whose due time had passed: sent at the next slot */
    uint32_t stuck;                     /* operations the baseband never ended: ended by their deadline */
    uint32_t aes_calls;                 /* PalCryptoAesEcb reached (no AES here: privacy is off, it should stay 0) */
    uint32_t asserts;
    /* connection events: waited through (busy wait), the longest wait; the most microseconds from a packet seen to
     * Cordio's response put in (T_IFS is 150); packets received with a bad CRC */
    uint32_t busy_waits, busy_us_max, resp_us_max, conn_rx_bad;
    /* the window setups (fm1ble_op_conn_setup): the one in use, how often a connection started on another setup than the
     * last one did; per setup the
     * events, the packets received, and the earliest and latest packet start after the window's start (Cordio's
     * dueUsec: the anchor minus the window widening), in us */
    uint32_t conn_setup, setup_moves;
    uint32_t set_events[FM1BLE_CONN_SETUPS], set_rx[FM1BLE_CONN_SETUPS];
    int32_t set_arr_min[FM1BLE_CONN_SETUPS], set_arr_max[FM1BLE_CONN_SETUPS];
    /* how long before the CONNECT_IND was seen the PAL had last looked (us): its timestamp, which places the connection's
     * first window, can be that much late (the advertising event is not waited through) */
    uint32_t cind_gap_us;
    uint32_t log_n;                     /* connection events logged, at most PAL_FM1_LOG */
    uint32_t log[PAL_FM1_LOG][4];
    uint32_t conns;                     /* connections started (their first event armed) */
    uint32_t good;                      /* the setup a connection received PAL_FM1_GOOD packets on, PAL_FM1_NO_SETUP before */
    uint32_t conn_sum[PAL_FM1_CONNS][4];
    /* the TX side: packets dropped as stale (PAL_FM1_STALE_US); events ended by their deadline, then halted
     * (fm1ble_op_halt), and the halts the hardware stayed busy through; INTIME's responses not put in (too late);
     * Cordio's response after the packet was seen in four buckets of 8 bits, under 50, 50-99, 100-149, 150 us and more
     * (each at most 255), and the least; per TX mode the events, the packets received, the events in which TXTOG changed
     * and the packets with RXSTAT bit 8; the TX mode in use, the one a connection received PAL_FM1_GOOD packets on
     * (PAL_FM1_NO_SETUP before), the first PAL_FM1_CONNS connections' modes (4 bits each, the first in 3-0), and the
     * driver's TX words (fm1ble_op_txstate) when the first connection packet was seen, before Cordio's response */
    uint32_t stale_rx, halts, halt_busy, resp_skipped, resp_hist, resp_us_min;
    uint32_t tx_events[FM1BLE_TX_MODES], tx_rx[FM1BLE_TX_MODES], tx_sent[FM1BLE_TX_MODES], tx_acks[FM1BLE_TX_MODES];
    uint32_t conn_tx, good_tx, conn_txs, first_rx_tx;
    /* the header trace and each connection's link-layer counts (PAL_FM1_TRACE_CONNS above) */
    uint32_t trace[PAL_FM1_TRACE_CONNS][PAL_FM1_TRACE_PER];
    uint32_t ll_ops[PAL_FM1_CONNS], ll_cnt[PAL_FM1_CONNS], arq_dup, arq_unwritten;
    uint32_t later_rx, later_given;
    /* ARQ's ways (PAL_FM1_L2_GOOD above): the ARQ connections started, the first 16 connections that ran ahead (bit k:
     * connection k), the way kept (1 ahead, 0 in T_IFS, PAL_FM1_NO_SETUP before), the responses written ahead; the chip's
     * SN (PAL_FM1_ARQ_STUCK): bit 2 changed at a connection's first packet, and after PAL_FM1_ARQ_STUCK; the capture */
    uint32_t arq_conns, ahead_conns, good_ahead, ahead_put, sn_flips_first, sn_flips_stuck;
    uint32_t cap_n;
    uint8_t cap[PAL_FM1_CAP][PAL_FM1_CAP_BYTES];
} pal_fm1_stats;

void pal_fm1_attach(const pal_fm1_cfg *cfg);

/* The baseband's interrupts (IRQs 45 and 29, D.4) and the scheduler's timer: completions to Cordio, the timer's callback
 * when due. Call it from the main loop as often as possible (and from the interrupts once they are attached). */
void pal_fm1_service(void);

const pal_fm1_stats *pal_fm1_state(void);

#endif
