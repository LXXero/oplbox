/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth stack on the FM-1 as one piece (docs/BLUETOOTH.md "Cordio path as built"): Cordio's PAL on the radio driver
 * (cordio_port/pal_fm1.c), Cordio's controller (cordio_port/cordio_ctrl.c) and the NimBLE host with the BLE-MIDI service
 * (ble_nimble.c), joined by HCI as a pair of function calls. Own code. Never run on an FM-1.
 *
 * Cordio and NimBLE are singletons that start once: fm1ble_stack_start brings them up the first time and, on later calls
 * (a second test run on a radio that was powered down and up again), hands the PAL the driver again and keeps the rest
 * [unchecked: Cordio's scheduler was not polled in between; its late operations are dropped and rescheduled].
 * Everything runs from the main loop through fm1ble_stack_poll; no interrupt is attached.
 */
#ifndef OPLBOX_BLE_FM1BLE_STACK_H
#define OPLBOX_BLE_FM1BLE_STACK_H

#include <stdint.h>
#include "fm1ble.h"
#include "ble_midi.h"

typedef struct {
    uint8_t started;            /* Cordio and NimBLE were brought up (once a boot) */
    int8_t err;                 /* why fm1ble_stack_start failed: -1 Cordio's init, -2 NimBLE's */
    uint32_t nocp;              /* Number Of Completed Packets events the controller sent the host */
    uint32_t hci_cmds, hci_events, acl_in, acl_out, heap_used;   /* Cordio's side (cordio_ctrl_state) */
} fm1ble_stack_info;

/* The driver b is powered up with its block open (fm1ble_radio_up, fm1ble_bb_init) and not advertising on its own.
 * addr: the controller's public address, least significant byte first. 0, or -1. */
int fm1ble_stack_start(fm1ble *b, const uint8_t addr[6]);

/* One main-loop step: the PAL (the baseband's flags, the scheduler's timer), the controller, the host. */
void fm1ble_stack_poll(void);

const fm1ble_stack_info *fm1ble_stack_state(void);

/* What a connected central wrote to the BLE-MIDI characteristic (ble_midi_get on it). */
ble_midi_in *fm1ble_stack_midi(void);

#endif
