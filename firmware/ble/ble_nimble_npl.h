/* SPDX-License-Identifier: GPL-3.0-only
 * The port's own functions beside NimBLE's NPL (docs/BLUETOOTH.md "Cordio path as built"): a clock in
 * milliseconds, the pump a blocking HCI command waits with, and the one call the main loop makes. Own code. */
#ifndef OPLBOX_BLE_NIMBLE_NPL_H
#define OPLBOX_BLE_NIMBLE_NPL_H

#include <stdint.h>

/* The clock (ms, wraps at 2^32) and the pump: what runs while the host waits for a controller's answer (the
 * transport's poll). Both are set once, before nimble_port_init. */
void ble_npl_port_set(uint32_t (*now_ms)(void *ctx), void (*pump)(void *ctx), void *ctx);

/* One main-loop step: arms the callouts that are due, then runs every queued event (including those the events
 * queue). Returns the number of events run. */
int ble_npl_port_run(void);

#endif
