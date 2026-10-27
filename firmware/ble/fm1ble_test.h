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

#define FM1BLE_N_CTR    40      /* counters behind the steps; names in fm1ble_ctr_name, FM1BLE_CTR_NAMES */

/* The counters, in the report's order. tools/fm1_ble_report.py has the same list (tests/ble_report_tool.py holds it to
 * FM1BLE_CTR_NAMES); the firmware at 48ea034 and before sent 24 others (the tool reads those by their count), up to
 * 122afe3 the first 32 of these, with pal_done_37-39 counting the driver's one-channel events that ended. */
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
    FM1BLE_C_PAL_CONN_ENDED, FM1BLE_C_PAL_CONN_RX, FM1BLE_C_PAL_REFUSED, FM1BLE_C_EVTCOUNT
};
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
    "pal_conn_ended", "pal_conn_rx", "pal_refused", "evtcount"

/* The air trials (adv_air): the driver's own ADV_IND, each trial from its own static random address, so a central's
 * scan (tools/ble_central.py) says which one reached the air. Trial 0: without the shared radio's init (variant 0), at
 * FM1BLE_TXPWR_LEVEL; trial 1: with it (variant 1), the same level; trial 2: variant 1 at FM1BLE_TXPWR_MAX. The address
 * is C2:46:4D:31:00:<FM1BLE_TRIAL_ADDR0 + trial>; the stack (Cordio, NimBLE) advertises from C2:46:4D:31:00:01. */
#define FM1BLE_TRIALS       3
#define FM1BLE_TRIAL_MS     3000u
#define FM1BLE_TRIAL_ADDR0  0x10u

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
