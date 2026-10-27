/* SPDX-License-Identifier: GPL-3.0-only
 * Cordio's LE controller (firmware/ble/cordio/, Apache-2.0, unchanged) as one object the app drives: init, a poll from the
 * main loop, and the HCI function pair to the host (docs/BLUETOOTH_CORDIO.md). Own code.
 *
 * The controller side of firmware/ble/hci.h's transport: the host sends with `cordio_ctrl_from_host(type, packet, size)`
 * (H4 type beside the packet, as hci_transport.send gives it) and the controller sends with `to_host.send`, which takes
 * the packet (0) or refuses it, and a refused packet is offered again by the next poll. The controller runs inside
 * cordio_ctrl_poll and inside the baseband's interrupt (the chip's PalBb* code calls back into it); nothing else calls it.
 */
#ifndef OPLBOX_BLE_CORDIO_CTRL_H
#define OPLBOX_BLE_CORDIO_CTRL_H

#include <stdint.h>
#include "hci.h"

typedef struct {
    hci_transport to_host;              /* controller to host: send(ctx, type, packet, size), 0 when taken */
    uint32_t (*now_ms)(void *ctx);      /* a clock, wraps at 2^32 */
    void *ctx;                          /* its argument */
    uint8_t bdaddr[6];                  /* the public device address, least significant byte first (Cordio's order) */
    uint8_t max_acl_len;                /* the ACL payload size the controller offers: 27..251, 0: 27 */
} cordio_ctrl_cfg;

typedef struct {
    uint32_t heap_used, heap_size;      /* controller memory taken by LlInit and the buffer pools */
    uint32_t cmds, events, acl_in, acl_out;
    uint32_t host_refused;              /* packets the host's transport did not take at first */
    uint32_t rx_dropped;                /* packets from the host thrown away: no buffer, too long, bad type */
} cordio_ctrl_stats;

/* Brings the controller up. Returns 0, or -1 when the heap is too small or the config is bad. Call once. */
int cordio_ctrl_init(const cordio_ctrl_cfg *cfg);

/* A packet from the host: HCI_CMD (opcode, length, parameters) or HCI_ACL. Returns 0 when taken, -1 when dropped. */
int cordio_ctrl_from_host(uint8_t type, const uint8_t *p, uint16_t n);

/* One main-loop step: timers up to the clock, every ready handler, a held packet to the host. */
void cordio_ctrl_poll(void);

const cordio_ctrl_stats *cordio_ctrl_state(void);

/* The port's own setup, called by cordio_ctrl_init (kept apart for cordio_wsf.c) */
void cordio_wsf_heap(void *mem, uint32_t size);
void cordio_wsf_clock(uint32_t (*now_ms)(void *ctx), void *ctx);

#endif
