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
    uint8_t conn_setups;                /* the driver's window setups to try (bit n: FM1BLE_CONN_* n); 0: all three */
} pal_fm1_cfg;

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
    /* the window setups (fm1ble_op_conn_setup): the one in use, how often a missed event moved to the next; per setup the
     * events, the packets received, and the earliest and latest packet start after the window's start (Cordio's
     * dueUsec: the anchor minus the window widening), in us */
    uint32_t conn_setup, setup_moves;
    uint32_t set_events[3], set_rx[3];
    int32_t set_arr_min[3], set_arr_max[3];
} pal_fm1_stats;

void pal_fm1_attach(const pal_fm1_cfg *cfg);

/* The baseband's interrupts (IRQs 45 and 29, D.4) and the scheduler's timer: completions to Cordio, the timer's callback
 * when due. Call it from the main loop as often as possible (and from the interrupts once they are attached). */
void pal_fm1_service(void);

const pal_fm1_stats *pal_fm1_state(void);

#endif
