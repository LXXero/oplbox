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
    uint8_t conn_setups;                /* the driver's window setups to try (bit n: FM1BLE_CONN_* n); 0: all four */
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

/* How much earlier than Cordio asks the scheduler's timer fires. Cordio starts an operation SCH_LOAD_DELAY_US (300 us)
 * before its dueUsec, less the timer's lateness from the main loop (up to 210 us at a7c1738), so a connection event was
 * armed 90-300 us ahead and the PAD and WIDE setups could not open their windows earlier than at once. With the lead the
 * event is armed about FM1BLE_CONN_WIDE_US + 300 us ahead. Cordio loads a BOD whose due time is in the future
 * (sch_main.c schLoadBod), so an early load only moves the PAL's arm and busy wait earlier. */
#define PAL_FM1_TIMER_LEAD_US   1250u

/* The event log: the first PAL_FM1_LOG_PER events of each connection since pal_fm1_attach, PAL_FM1_LOG in all, three
 * words each.
 *   a: the channel (bits 5-0), the setup (7-6 its low two bits, 14 its third: FM1BLE_CONN_PADJ and up), flags (8 a
 *      packet came, 9 its CRC was good, 10 TXTOG changed in the event, 11 the event ended by its deadline not the
 *      baseband's flag, 12 Cordio went on before it ended, 13 the first event of a connection), Cordio's receive timeout
 *      in us (31-16, at most 65535);
 *   b: when the packet started (15-0) and when the PAL saw the event's end (31-16), us after Cordio's dueUsec, signed
 *      16-bit (PAL_FM1_LOG_NONE: no packet, or no end seen);
 *   c: the delay the event was armed with, us from now to dueUsec (15-0, at most 65535), the packet's RXDHDR (31-16). */
#define PAL_FM1_LOG         12
#define PAL_FM1_LOG_PER     4
#define PAL_FM1_LOG_NONE    0x8000u

/* Each connection's summary, for the first PAL_FM1_CONNS since pal_fm1_attach, three words:
 *   a: the setup (bits 3-0), the connection events (15-4, at most 4095), the packets received in them (31-16);
 *   b: the first packet's start (15-0) and the last one's (31-16), us after Cordio's dueUsec of its event, signed 16-bit;
 *   c: of the events that missed their packet, the earliest (15-0) and latest (31-16) the baseband's end flag came, us
 *      after dueUsec, signed 16-bit (where the chip really closed the window);
 * PAL_FM1_LOG_NONE where there is none. */
#define PAL_FM1_CONNS       8

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
    uint32_t log[PAL_FM1_LOG][3];
    uint32_t conns;                     /* connections started (their first event armed) */
    uint32_t good;                      /* the setup a connection received PAL_FM1_GOOD packets on, PAL_FM1_NO_SETUP before */
    uint32_t conn_sum[PAL_FM1_CONNS][3];
} pal_fm1_stats;

void pal_fm1_attach(const pal_fm1_cfg *cfg);

/* The baseband's interrupts (IRQs 45 and 29, D.4) and the scheduler's timer: completions to Cordio, the timer's callback
 * when due. Call it from the main loop as often as possible (and from the interrupts once they are attached). */
void pal_fm1_service(void);

const pal_fm1_stats *pal_fm1_state(void);

#endif
