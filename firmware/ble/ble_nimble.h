/* SPDX-License-Identifier: GPL-3.0-only
 * The BLE-MIDI peripheral on the NimBLE host (docs/BLUETOOTH.md "Cordio path as built", step 1). Own code: it sits on
 * NimBLE's public API (firmware/ble/nimble/, Apache-2.0, unchanged) and on this directory's profile (ble_midi.c).
 *
 * NimBLE is a singleton (global state), so there is one of these. It is driven from the main loop: ble_nimble_poll
 * runs NimBLE's events and timers, ble_nimble_receive takes every packet the controller sends. The transport to the
 * controller is the same function pair as hci.h's: send(type, packet, size), 0 when taken.
 *
 * What it does: starts the host (Reset ... Read BD_ADDR through the controller), registers the GAP service (the device
 * name) and the BLE-MIDI service (one characteristic: read returns no payload, write without response is a BLE-MIDI
 * packet for ble_midi_in, notify carries ours), advertises the service UUID with the name in the scan response, takes
 * one connection, asks for a connection interval of 15 ms or less once connected, and advertises again after a
 * disconnection. No pairing: a central that wants bonding is refused by the host (docs/BLUETOOTH.md stage 7).
 */
#ifndef OPLBOX_BLE_NIMBLE_H
#define OPLBOX_BLE_NIMBLE_H

#include <stdint.h>
#include "hci.h"
#include "ble_midi.h"

typedef struct {
    const hci_transport *t;             /* host to controller */
    uint32_t (*now_ms)(void *ctx);      /* a clock, wraps at 2^32 */
    void (*pump)(void *ctx);            /* runs the controller (and the clock) while a command waits for its answer */
    void *ctx;
    const char *name;                   /* the device name: at most 29 characters; "OPLBOX" */
    ble_midi_in *in;                    /* BLE-MIDI packets written by the central go here */
    uint16_t adv_itvl_min, adv_itvl_max;/* advertising interval, 0.625 ms units; 0: 30 and 60 ms */
} ble_nimble_cfg;

enum { BLE_NIMBLE_OFF, BLE_NIMBLE_STARTING, BLE_NIMBLE_ADVERTISING, BLE_NIMBLE_CONNECTED, BLE_NIMBLE_FAILED };

typedef struct {
    uint8_t state;
    int fail;                           /* why FAILED: 0x100 + NimBLE's reset reason (BLE_HS_E*: a command timed out is
                                           BLE_HS_ETIMEOUT_HCI, an answer it did not understand BLE_HS_ECONTROLLER), or a
                                           NimBLE error from setting up advertising */
    uint16_t conn;                      /* the connection handle, 0xFFFF for none */
    uint16_t mtu;                       /* the ATT MTU of the connection (23 until the central asks for more) */
    uint16_t interval, latency, timeout;/* the connection's parameters, 1.25 ms, events, 10 ms */
    uint8_t notify_on;                  /* the central enabled notifications on the characteristic */
    uint8_t bdaddr[6];                  /* the public address, most significant byte first */
    uint32_t connects, disconnects, writes, notified, notify_failed, update_asked, hci_refused, failures, rx_bad;
} ble_nimble_status;

/* Starts the host. Returns 0, or a NimBLE error; the state is FAILED then. The host's start-up (a few commands) runs
 * inside a poll, not here. */
int ble_nimble_init(const ble_nimble_cfg *cfg);

/* The controller's packet handler: every packet it sends, by type (HCI_EVT or HCI_ACL). An event whose length byte
 * disagrees with its size, a type that is neither, or a packet NimBLE has no buffer for is refused and counted (rx_bad). */
void ble_nimble_receive(uint8_t type, const uint8_t *p, uint16_t n);

/* One main-loop step: runs NimBLE's queued events and due timers and puts a held packet on the transport. */
void ble_nimble_poll(void);

/* One BLE-MIDI packet (ble_midi_enc's send function: ctx unused) as a notification. 0, or -1 when not connected, the
 * central has not enabled notifications, the packet is longer than MTU - 3, or NimBLE had no buffer. */
int ble_nimble_notify(void *ctx, const uint8_t *packet, uint32_t n);

const ble_nimble_status *ble_nimble_state(void);

/* After FAILED: lets NimBLE start over (its own retry timer runs the Reset again within a few seconds). The state is
 * STARTING until the host syncs, or FAILED again. Does nothing in another state. */
void ble_nimble_retry(void);

/* Ends the connection (the central is told "remote user terminated"). 0, or -1 when not connected. */
int ble_nimble_disconnect(void);

#endif
