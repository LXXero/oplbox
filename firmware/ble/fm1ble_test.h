/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run (docs/BLUETOOTH.md "Cordio path as built" step 5): the steps a diagnostic build runs to find
 * out how far the stack gets on an FM-1, one result and its raw evidence per step, in the way the WiFi test page reports
 * stages 4-8. Own code. The run is a state machine the app calls from its loop: fm1ble_test_begin, then fm1ble_test_poll
 * with the time until it returns 1. The page, the USB SysEx report and tools/fm1_ble_report.py only read fm1ble_run.
 *
 * A step that fails says in `note` what the next fix needs (a register that did not read back, the HCI status, the event
 * that never came); a step that cannot run because an earlier one failed is SKIP.
 */
#ifndef OPLBOX_BLE_FM1BLE_TEST_H
#define OPLBOX_BLE_FM1BLE_TEST_H

#include <stdint.h>

/* the steps, in the order they run */
enum {
    FM1BLE_STEP_RADIO,      /* the Bluetooth side of the radio powered up: the registers read back */
    FM1BLE_STEP_BLOCK,      /* the baseband's RAM block set up and the baseband started (no interrupt storm) */
    FM1BLE_STEP_HCI,        /* the host reset the controller: Reset, Read Local Version, Read BD_ADDR, LE Read Buffer Size */
    FM1BLE_STEP_ADV_SET,    /* advertising set up and enabled: the controller accepted all four commands */
    FM1BLE_STEP_ADV_AIR,    /* the baseband ran advertising events and says it sent: TXTOG changed, or a request came in */
    FM1BLE_STEP_CONNECT,    /* a central connected within the wait: LE Connection Complete, the interval, the MTU */
    FM1BLE_STEP_MIDI_IN,    /* a BLE-MIDI packet written by the central reached the app's MIDI input */
    FM1BLE_STEP_MIDI_OUT,   /* a notification went out and the controller handed its buffer back */
    FM1BLE_STEPS
};

enum { FM1BLE_NOT_RUN, FM1BLE_PASS, FM1BLE_FAIL, FM1BLE_SKIP };

#define FM1BLE_RAW      48      /* bytes of raw evidence kept per step */
#define FM1BLE_NOTE     48      /* the step's one-line result, ASCII, NUL-terminated */

typedef struct {
    uint8_t result;             /* FM1BLE_NOT_RUN, PASS, FAIL or SKIP */
    uint8_t nraw;               /* bytes used in raw */
    uint8_t raw[FM1BLE_RAW];    /* the step's evidence as bytes: register readbacks (u32 little-endian) or HCI event bytes */
    char note[FM1BLE_NOTE];
    uint32_t ms;                /* milliseconds the step took, from the run's start to its end */
} fm1ble_step;

#define FM1BLE_N_CTR    80      /* counters behind the steps; names in fm1ble_ctr_name, FM1BLE_CTR_NAMES */

/* The counters, in the report's order. tools/fm1_ble_report.py has the same list (tests/ble_report_tool.py holds it to
 * FM1BLE_CTR_NAMES); the firmware at 48ea034 and before sent 24 others (the tool reads those by their count), up to
 * 122afe3 the first 32 of these, with pal_done_37-39 counting the driver's one-channel events that ended, up to 3b39ab4
 * the first 40, up to 58cadc5 the first 64. The RX trials' four each (rxtN_*) and the receive setup the stack ran on
 * follow, then the connection events' (pal_fm1.h: the busy wait, the window setups; fm1ble.h's connection words). */
enum {
    FM1BLE_C_RADIO_REGS_OK, FM1BLE_C_RADIO_REGS_BAD, FM1BLE_C_VARIANT, FM1BLE_C_RF_COMMON,
    FM1BLE_C_EVENTS, FM1BLE_C_TX_FLIPS, FM1BLE_C_RX_PACKETS, FM1BLE_C_RX_SCAN_REQ,
    FM1BLE_C_RX_CONN_IND, FM1BLE_C_RX_OTHER, FM1BLE_C_PENDING_LAST, FM1BLE_C_BB_ERR,
    FM1BLE_C_AIR_TRIALS, FM1BLE_C_TXTOG, FM1BLE_C_REARM_USED, FM1BLE_C_ADV_ERR,
    FM1BLE_C_RX_HDR, FM1BLE_C_RX_STAT, FM1BLE_C_RX_TYPE, FM1BLE_C_RX_LEN,
    FM1BLE_C_PAL_ADV_OPS, FM1BLE_C_PAL_CONN_OPS, FM1BLE_C_PAL_RX_OK, FM1BLE_C_PAL_RX_TIMEOUT,
    FM1BLE_C_PAL_RESP_LATE, FM1BLE_C_PAL_STUCK, FM1BLE_C_PAL_TIMER_LATE_US, FM1BLE_C_PAL_DONE_37,
    FM1BLE_C_PAL_DONE_38, FM1BLE_C_PAL_DONE_39, FM1BLE_C_TXPWR, FM1BLE_C_RAM_END,
    FM1BLE_C_PAL_ADV_EVENTS, FM1BLE_C_PAL_ADV_COVERED, FM1BLE_C_PAL_ADV_ENDED, FM1BLE_C_PAL_ADV_CONN_IND,
    FM1BLE_C_PAL_CONN_ENDED, FM1BLE_C_PAL_CONN_RX, FM1BLE_C_PAL_REFUSED, FM1BLE_C_EVTCOUNT,
    FM1BLE_C_RXT,                                   /* RX trial k's four: FM1BLE_C_RXT + 4k + FM1BLE_RXT_* */
    FM1BLE_C_RX_STACK_TRIAL = FM1BLE_C_RXT + 16, FM1BLE_C_RX_AGC_TABLE, FM1BLE_C_RX_WIN, FM1BLE_C_AGC_CTL,
    FM1BLE_C_AGC_RSSI, FM1BLE_C_CONN_WIN, FM1BLE_C_PAL_CONN_TIMEOUT_US, FM1BLE_C_RX_STAT1,
    FM1BLE_C_PAL_BUSY_WAITS, FM1BLE_C_PAL_BUSY_US_MAX, FM1BLE_C_PAL_RESP_US_MAX, FM1BLE_C_PAL_CONN_RX_BAD,
    FM1BLE_C_PAL_CONN_SETUP, FM1BLE_C_PAL_SETUP_MOVES, FM1BLE_C_PAL_LATE_OPS, FM1BLE_C_CONN_FLIPS,
    FM1BLE_C_PAL_SET_COUNTS,                        /* setup k's: FM1BLE_C_PAL_SET_COUNTS + k */
    FM1BLE_C_CONN_ACKS = FM1BLE_C_PAL_SET_COUNTS + 3,
    FM1BLE_C_PAL_SET_ARR,                           /* setup k's: FM1BLE_C_PAL_SET_ARR + k */
    FM1BLE_C_CONN_RX_WORDS = FM1BLE_C_PAL_SET_ARR + 3
};
/* the connection events' counters: pal_setN_counts = setup N's events (bits 15-0) and packets received (31-16), each at
 * most 65535; pal_setN_arr = the earliest (bits 15-0) and latest (31-16) packet start after the window's start, us, as
 * signed 16-bit (0 with no packet); conn_rx_words = the last connection packet's RXSTAT (15-0, bit 8 "acknowledges ours")
 * and RXDHDR (31-16) */
/* an RX trial's counters: counts = events (bits 7-0), TXTOG changes (15-8), packets received (23-16), SCAN_REQs (31-24),
 * each at most 255; stat = the first packet's RXSTAT (15-0) and RXAHDR (31-16), or with none the words RXSTAT0 and RXAHDR0
 * as the trial left them; pkt = that packet's RXDHDR (15-0), the RX buffer it was in (23-16) and LASTCHMAP's low byte
 * (31-24); bytes = its payload's first four bytes, the first in bits 7-0 */
enum { FM1BLE_RXT_COUNTS, FM1BLE_RXT_STAT, FM1BLE_RXT_PKT, FM1BLE_RXT_BYTES };
#define FM1BLE_CTR_NAMES \
    "radio_regs_ok", "radio_regs_bad", "variant", "rf_common", \
    "events", "tx_flips", "rx_packets", "rx_scan_req", \
    "rx_conn_ind", "rx_other", "pending_last", "bb_err", \
    "air_trials", "txtog", "rearm_used", "adv_err", \
    "rx_hdr", "rx_stat", "rx_type", "rx_len", \
    "pal_adv_ops", "pal_conn_ops", "pal_rx_ok", "pal_rx_timeout", \
    "pal_resp_late", "pal_stuck", "pal_timer_late_us", "pal_done_37", \
    "pal_done_38", "pal_done_39", "txpwr", "ram_end", \
    "pal_adv_events", "pal_adv_covered", "pal_adv_ended", "pal_adv_conn_ind", \
    "pal_conn_ended", "pal_conn_rx", "pal_refused", "evtcount", \
    "rxt0_counts", "rxt0_stat", "rxt0_pkt", "rxt0_bytes", \
    "rxt1_counts", "rxt1_stat", "rxt1_pkt", "rxt1_bytes", \
    "rxt2_counts", "rxt2_stat", "rxt2_pkt", "rxt2_bytes", \
    "rxt3_counts", "rxt3_stat", "rxt3_pkt", "rxt3_bytes", \
    "rx_stack_trial", "rx_agc_table", "rx_win", "agc_ctl", \
    "agc_rssi", "conn_win", "pal_conn_timeout_us", "rx_stat1", \
    "pal_busy_waits", "pal_busy_us_max", "pal_resp_us_max", "pal_conn_rx_bad", \
    "pal_conn_setup", "pal_setup_moves", "pal_late_ops", "conn_flips", \
    "pal_set0_counts", "pal_set1_counts", "pal_set2_counts", "conn_acks", \
    "pal_set0_arr", "pal_set1_arr", "pal_set2_arr", "conn_rx_words"

/* The air trials (adv_air): the driver's own ADV_IND, each trial from its own static random address, so a central's
 * scan (tools/ble_central.py) says which one reached the air. Trial 0: without the shared radio's init (variant 0), at
 * FM1BLE_TXPWR_LEVEL; trial 1: with it (variant 1), the same level; trial 2: variant 1 at FM1BLE_TXPWR_MAX. The address
 * is C2:46:4D:31:00:<FM1BLE_TRIAL_ADDR0 + trial>; the stack (Cordio, NimBLE) advertises from C2:46:4D:31:00:01. */
#define FM1BLE_TRIALS       3
#define FM1BLE_TRIAL_MS     3000u
#define FM1BLE_TRIAL_ADDR0  0x10u

/* The RX trials, after the air trials and before the stack: the same ADV_IND on variant 1 at FM1BLE_TXPWR_LEVEL, each from
 * C2:46:4D:31:00:<FM1BLE_RX_ADDR0 + k> with its own receive setup (fm1ble_rx_setup), so a central's active scan (its
 * SCAN_REQs) shows in each trial's counters which setup lets the radio receive after its own TX. Up to 3b39ab4 the driver
 * wrote neither the AGC block's gain table nor WINCNTL2, and the air trials received nothing (the trials above still run
 * that way, to compare):
 *   0 (:13): no gain table, WINCNTL2 = 0x32 (le_hw_advertising's)
 *   1 (:14): the gain table (the radio powered up again with it), WINCNTL2 = 0
 *   2 (:15): the gain table, WINCNTL2 = 0x32: JieLi's setup, the driver's default
 *   3 (:16): the gain table, WINCNTL2 = 0x96 (the direct-test receiver's)
 * The stack then runs on the window of the first of trials 2, 3, 0, 1 that received a packet (2 when none did), with the
 * gain table (rx_stack_trial, rx_agc_table and rx_win say which). */
#define FM1BLE_RX_TRIALS    4
#define FM1BLE_RX_ADDR0     (FM1BLE_TRIAL_ADDR0 + FM1BLE_TRIALS)

enum { FM1BLE_IDLE, FM1BLE_RUNNING, FM1BLE_DONE };

typedef struct {
    uint8_t state;              /* FM1BLE_IDLE, RUNNING, DONE */
    uint8_t cur;                /* the step now running */
    uint8_t bdaddr[6];          /* the controller's public address, most significant byte first (valid after HCI) */
    uint32_t t0, now;           /* the clock at begin and at the last poll, ms */
    fm1ble_step step[FM1BLE_STEPS];
    uint32_t ctr[FM1BLE_N_CTR]; /* see fm1ble_ctr_name */
} fm1ble_run;

/* Starts a run (clears r). The central has `wait_ms` to connect once advertising is on; 0: skip the connect and MIDI steps. */
void fm1ble_test_begin(fm1ble_run *r, uint32_t now_ms, uint32_t wait_ms);

/* One step of the loop: 1 when the run is DONE. Never blocks for more than a few milliseconds, except where the radio's
 * own start-up must (the page shows that: audio is muted around it, as for WiFi). */
int fm1ble_test_poll(fm1ble_run *r, uint32_t now_ms);

/* Names for the report: lowercase words, no spaces. NULL past the last. */
const char *fm1ble_step_name(int step);
const char *fm1ble_ctr_name(int ctr);

#endif
