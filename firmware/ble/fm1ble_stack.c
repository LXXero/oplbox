/* SPDX-License-Identifier: GPL-3.0-only
 * See fm1ble_stack.h. Own code. */
#include <string.h>
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "fm1ble_stack.h"
#include "cordio_ctrl.h"
#include "pal_fm1.h"
#include "ble_nimble.h"
#include "ble_midi.h"
#include "hci.h"

static fm1ble_stack_info g_st;
static ble_midi_in g_in;
static uint8_t g_sx[64];                /* SysEx from a central is not used by the test run: a short one fits, a long one is dropped */

static uint32_t now_ms(void *ctx) { (void)ctx; return fm1ble_port_now_us() / 1000u; }

/* The ATT MTU NimBLE offers, connection by connection in turn (ble_nimble.h cfg mtus; since round 8 of the FM-1 Bluetooth
 * goal). At 8ff3c5e (~/oplbox-ble-runs/8ff3c5e-1) BlueZ asked for 517, NimBLE offered 185, the link took 27-octet data
 * PDUs (Cordio's LL_LENGTH_RSP), and GATT discovery went through ten ATT exchanges until the first response longer than
 * one data PDU: the 37-byte Read By Type Response, handed over as a start (27 bytes) and a continuation (14). BlueZ never
 * sent its next request and left 754 events later. With 23 no L2CAP PDU is ever fragmented, either way: the first
 * connection runs that (the likeliest to resolve GATT), the next one 185 (the fragmented responses, the PDU capture's
 * air entries: pal_fm1.h PAL_FM1_CAP), and so on in turn. */
static const uint16_t g_mtus[] = { 23, 185 };

/* HCI as two function calls: the host's commands and ACL to Cordio, Cordio's events and ACL to NimBLE */
static int to_ctrl(void *ctx, uint8_t type, const uint8_t *p, uint16_t n) { (void)ctx; return cordio_ctrl_from_host(type, p, n); }

static int to_host(void *ctx, uint8_t type, const uint8_t *p, uint16_t n)
{
    (void)ctx;
    if (type == HCI_EVT && n && p[0] == HCI_EV_NUM_COMPLETED)
        g_st.nocp++;
    ble_nimble_receive(type, p, n);
    return 0;
}

static const hci_transport g_t = { to_ctrl, 0 };

/* while NimBLE waits for a command's answer: the PAL and the controller, 50 us at a time */
static void pump(void *ctx)
{
    (void)ctx;
    pal_fm1_service();
    cordio_ctrl_poll();
    fm1ble_port_delay_us(50);
}

static uint32_t rand32(void *ctx) { (void)ctx; return fm1ble_port_now_us() * 2654435761u; }

int fm1ble_stack_start(fm1ble *b, const uint8_t addr[6])
{
    pal_fm1_cfg pc;
    cordio_ctrl_cfg cc;
    ble_nimble_cfg nc;

    memset(&pc, 0, sizeof pc);
    pc.b = b;
    pc.rand32 = rand32;
    /* PADW only: the one setup whose window follows the register on the FM-1 (oplbox-ble-run-9c47113-1's probe: its end
     * moved with WINCNTL0, 449..811 us at 60 us and 3019..3709 at 2500, where KEEP, PAD, WIDE, PADJ and PADB ignored the
     * window) and the only one that received past a connection's first event. ARQ only (since round 7): on the chip's
     * own SN and NESN (fm1ble.h) the other three lose the link layer, as at 2e6d430; its connections run its two ways
     * in turn (pal_fm1.h PAL_FM1_L2_GOOD), ahead first */
    pc.conn_setups = 1u << FM1BLE_CONN_PADW;
    pc.tx_modes = 1u << FM1BLE_TX_ARQ;
    pal_fm1_attach(&pc);
    if (g_st.started)
        return 0;
    memset(&cc, 0, sizeof cc);
    cc.to_host.send = to_host;
    cc.now_ms = now_ms;
    memcpy(cc.bdaddr, addr, 6);
    if (cordio_ctrl_init(&cc) != 0) {
        g_st.err = -1;
        return -1;
    }
    ble_midi_in_init(&g_in, g_sx, sizeof g_sx, NULL, NULL);
    memset(&nc, 0, sizeof nc);
    nc.t = &g_t;
    nc.now_ms = now_ms;
    nc.pump = pump;
    nc.name = "OPLBOX";
    nc.in = &g_in;
    nc.mtus = g_mtus;
    nc.nmtus = (uint8_t)(sizeof g_mtus / sizeof g_mtus[0]);
    if (ble_nimble_init(&nc) != 0) {
        g_st.err = -2;
        return -1;
    }
    g_st.started = 1;
    return 0;
}

void fm1ble_stack_poll(void)
{
    const cordio_ctrl_stats *c;
    if (!g_st.started)
        return;
    pal_fm1_service();
    cordio_ctrl_poll();
    ble_nimble_poll();
    c = cordio_ctrl_state();
    g_st.hci_cmds = c->cmds;
    g_st.hci_events = c->events;
    g_st.acl_in = c->acl_in;
    g_st.acl_out = c->acl_out;
    g_st.heap_used = c->heap_used;
}

const fm1ble_stack_info *fm1ble_stack_state(void) { return &g_st; }

ble_midi_in *fm1ble_stack_midi(void) { return &g_in; }
