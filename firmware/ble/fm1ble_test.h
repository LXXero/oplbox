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
    FM1BLE_STEP_ADV_AIR,    /* the baseband ran advertising events: the interrupt count grew over a second */
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

#define FM1BLE_N_CTR    24      /* counters behind the steps; names in fm1ble_ctr_name */

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
