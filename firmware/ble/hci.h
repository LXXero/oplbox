/* SPDX-License-Identifier: GPL-3.0-only
 * The host side of HCI for a BLE peripheral (docs/BLUETOOTH.md stage 1): command, event and ACL data packets,
 * the H4 packet-type framing, and a small host that starts the controller, advertises, takes one connection,
 * moves L2CAP PDUs over ACL with the controller's flow control, and advertises again after a disconnection.
 *
 * Own code, written from the Bluetooth Core Specification (v5.4 section numbers below; the opcodes, event codes and
 * field layouts were cross-checked against Apache NimBLE's include/nimble/hci_common.h, values only). Nothing here is
 * from JieLi's btstack.a or its headers. Host build and firmware build alike: no register access, no allocation, no
 * waiting; the caller drives it from its main loop (hci_host_poll) and hands it every packet the controller sends
 * (hci_host_receive).
 *
 * The transport is a function pair, the shape of the controller's function-call interface in BLUETOOTH.md "The
 * controller's interface: HCI": the host sends with hci_transport.send(type, packet, size) and the controller calls
 * back with (type, packet, size). The type is the H4 packet indicator (Vol 4 Part A 2); over a function call it
 * travels beside the packet, over a byte stream it is the byte in front of it (hci_h4_pack, hci_h4_feed).
 */
#ifndef OPLBOX_BLE_HCI_H
#define OPLBOX_BLE_HCI_H

#include <stdint.h>

/* H4 packet indicators (Vol 4 Part A 2) */
#define HCI_CMD 0x01
#define HCI_ACL 0x02
#define HCI_SCO 0x03
#define HCI_EVT 0x04
#define HCI_ISO 0x05

/* Opcodes: OGF << 10 | OCF (Vol 4 Part E 5.4.1, 7) */
#define HCI_OP(ogf, ocf)              ((uint16_t)((ogf) << 10 | (ocf)))
#define HCI_OP_NOP                    0x0000   /* a Command Complete with this opcode only gives command credits */
#define HCI_OP_DISCONNECT             0x0406   /* 7.1.6 */
#define HCI_OP_SET_EVENT_MASK         0x0C01   /* 7.3.1 */
#define HCI_OP_RESET                  0x0C03   /* 7.3.2 */
#define HCI_OP_READ_BUFFER_SIZE       0x1005   /* 7.4.5: when LE has no buffers of its own */
#define HCI_OP_READ_BD_ADDR           0x1009   /* 7.4.6 */
#define HCI_OP_LE_SET_EVENT_MASK      0x2001   /* 7.8.1 */
#define HCI_OP_LE_READ_BUFFER_SIZE    0x2002   /* 7.8.2 */
#define HCI_OP_LE_SET_ADV_PARAMS      0x2006   /* 7.8.5 */
#define HCI_OP_LE_SET_ADV_DATA        0x2008   /* 7.8.7 */
#define HCI_OP_LE_SET_SCAN_RSP_DATA   0x2009   /* 7.8.8 */
#define HCI_OP_LE_SET_ADV_ENABLE      0x200A   /* 7.8.9 */

/* Event codes (Vol 4 Part E 7.7) */
#define HCI_EV_DISCONN_COMPLETE       0x05     /* 7.7.5 */
#define HCI_EV_CMD_COMPLETE           0x0E     /* 7.7.14 */
#define HCI_EV_CMD_STATUS             0x0F     /* 7.7.15 */
#define HCI_EV_HW_ERROR               0x10     /* 7.7.16 */
#define HCI_EV_NUM_COMPLETED          0x13     /* 7.7.19 */
#define HCI_EV_LE_META                0x3E     /* 7.7.65 */
#define HCI_LE_CONN_COMPLETE          0x01     /* 7.7.65.1 */
#define HCI_LE_CONN_UPDATE_COMPLETE   0x03     /* 7.7.65.3 */

/* The two event masks the host sets (7.3.1, 7.8.1): Disconnection Complete (bit 4), Hardware Error (bit 15), LE Meta
 * (bit 61); LE Connection Complete (bit 0) and LE Connection Update Complete (bit 2). Command Complete, Command Status
 * and Number Of Completed Packets cannot be masked. */
#define HCI_EVENT_MASK                0x2000000000008010ull
#define HCI_LE_EVENT_MASK             0x0000000000000005ull

/* ACL data packet (5.4.2): the Packet_Boundary flag in bits 13-12 of the first word, Broadcast in 15-14 */
#define HCI_PB_FIRST_NOFLUSH          0        /* host to controller: the first fragment of an L2CAP PDU */
#define HCI_PB_CONT                   1        /* either way: a continuing fragment */
#define HCI_PB_FIRST_FLUSH            2        /* controller to host: the first fragment */

/* Error codes the host reads (Vol 1 Part F) */
#define HCI_ERR_UNKNOWN_COMMAND       0x01
#define HCI_ERR_INVALID_PARAMS        0x12
#define HCI_ERR_REMOTE_USER_TERM      0x13
#define HCI_ERR_LOCAL_HOST_TERM       0x16

/* L2CAP fixed channels on LE (Vol 3 Part A 2.1) */
#define L2CAP_CID_ATT                 0x0004
#define L2CAP_CID_LE_SIG              0x0005
#define L2CAP_CID_SMP                 0x0006

#define HCI_CMD_MAX                   (3 + 255)
#define HCI_EVT_MAX                   (2 + 255)
#define HCI_ACL_DATA_MAX              251      /* the largest LE ACL payload (LE Data Length Extension) */
#define HCI_ACL_MAX                   (4 + HCI_ACL_DATA_MAX)
#define HCI_L2CAP_MAX                 (4 + 256) /* basic header and an information payload of 256: ATT MTU 256 */
#define HCI_H4_MAX                    (1 + HCI_ACL_MAX)
#define HCI_NO_CONN                   0xFFFFu
#define HCI_CMD_TIMEOUT_MS            2000u    /* a command the controller never answers fails the start */

#define HCI_OK       0
#define HCI_EBUSY   -1   /* not now: no credit, a command or a PDU still going out, not connected */
#define HCI_EINVAL  -2   /* malformed packet, or too long */

/* ---- packets ---- */

/* A command packet: opcode, parameter length, parameters. Returns its size, 0 when n is over 255. */
uint16_t hci_cmd_pack(uint8_t *out, uint16_t op, const uint8_t *par, uint16_t n);

/* An ACL data packet: handle and flags, data length, data. Returns its size, 0 when n is over HCI_ACL_DATA_MAX. */
uint16_t hci_acl_pack(uint8_t *out, uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t n);

/* An ACL data packet's header. HCI_EINVAL when its length field disagrees with n. */
int hci_acl_parse(const uint8_t *p, uint16_t n, uint16_t *handle, uint8_t *pb, const uint8_t **data, uint16_t *len);

/* An event packet, decoded into the fields the host uses. Pointers point into the packet. */
typedef struct {
    uint8_t code, sub;              /* event code; for LE Meta, its subevent */
    uint8_t ncmd;                   /* Command Complete / Status: Num_HCI_Command_Packets */
    uint16_t opcode;                /* Command Complete / Status: the command it answers */
    uint8_t status;                 /* Command Complete's first return byte (0 when it has none), Command Status,
                                       Disconnection Complete, the LE connection events; Hardware Error: its code */
    const uint8_t *ret;             /* Command Complete: the return parameters after the status byte */
    uint8_t nret;
    uint16_t handle;                /* Disconnection Complete, the LE connection events */
    uint8_t reason;                 /* Disconnection Complete */
    uint8_t role, peer_type, peer[6];   /* LE Connection Complete (peer[0] is the address's least significant byte) */
    uint16_t interval, latency, timeout;  /* LE Connection Complete and Update Complete: 1.25 ms, events, 10 ms */
    uint8_t npairs;                 /* Number Of Completed Packets: how many (handle, count) pairs */
    const uint8_t *pairs;           /* npairs x 4 bytes, little endian */
} hci_evt;

/* HCI_EINVAL when the length byte disagrees with n or the event is shorter than its fields. Events the host does not
 * use decode to their code only. */
int hci_evt_parse(const uint8_t *p, uint16_t n, hci_evt *e);

/* Name of a command the host sends, for logs and the test page ("RESET", "LE SET ADV PARAMS"); "?" otherwise. */
const char *hci_op_name(uint16_t op);

/* ---- H4 framing over a byte stream ---- */

/* The indicator byte, then the packet. Returns 1 + n. */
uint32_t hci_h4_pack(uint8_t *out, uint8_t type, const uint8_t *p, uint16_t n);

/* Takes a byte stream in any pieces and calls emit once per whole packet. A byte that is no packet indicator, or a
 * packet longer than HCI_H4_MAX, is skipped and counted in dropped (bytes); the reader then looks for the next
 * indicator. */
typedef struct {
    uint8_t buf[HCI_H4_MAX];
    uint16_t have, need;            /* bytes in buf (indicator included); bytes the packet has in all, 0: unknown yet */
    uint16_t skip;                  /* the rest of an over-long packet still to throw away */
    uint32_t dropped;
} hci_h4;

void hci_h4_init(hci_h4 *r);
void hci_h4_feed(hci_h4 *r, const uint8_t *b, uint32_t n,
                 void (*emit)(void *ctx, uint8_t type, const uint8_t *p, uint16_t n), void *ctx);

/* ---- the host ---- */

typedef struct {
    int (*send)(void *ctx, uint8_t type, const uint8_t *p, uint16_t n);   /* 0: taken; otherwise try again later */
    void *ctx;
} hci_transport;

typedef struct {
    uint16_t int_min, int_max;      /* advertising interval, 0.625 ms units (0x0020-0x4000) */
    uint8_t adv[31], nadv;          /* advertising data (AD structures) */
    uint8_t rsp[31], nrsp;          /* scan response data */
} hci_adv_cfg;

/* Appends one AD structure (Vol 3 Part C 11, the types in the Assigned Numbers) to an advertising or scan response
 * payload. HCI_EINVAL when it does not fit in 31 bytes. */
int hci_ad_add(uint8_t *buf, uint8_t *n, uint8_t type, const void *data, uint8_t len);

enum { HCI_ST_OFF, HCI_ST_STARTING, HCI_ST_ADVERTISING, HCI_ST_CONNECTED, HCI_ST_FAILED };
enum { HCI_FAIL_NONE, HCI_FAIL_STATUS, HCI_FAIL_TIMEOUT, HCI_FAIL_HW_ERROR, HCI_FAIL_BAD_EVENT };

/* The start: reset, both event masks, the buffer size, the address, the advertising parameters, both payloads and
 * advertising on, one command at a time, each after the previous one's Command Complete with status 0. */
enum { HCI_STEP_RESET, HCI_STEP_EVENT_MASK, HCI_STEP_LE_EVENT_MASK, HCI_STEP_LE_BUFFER_SIZE, HCI_STEP_BUFFER_SIZE,
       HCI_STEP_BD_ADDR, HCI_STEP_ADV_PARAMS, HCI_STEP_ADV_DATA, HCI_STEP_SCAN_RSP, HCI_STEP_ADV_ENABLE, HCI_STEP_DONE };

typedef void (*hci_l2cap_fn)(void *ctx, uint16_t handle, uint16_t cid, const uint8_t *p, uint16_t n);

typedef struct {
    const hci_transport *t;
    hci_l2cap_fn on_l2cap;          /* a whole L2CAP PDU from the central: its channel and information payload */
    void *ctx;
    hci_adv_cfg adv;

    uint8_t state, step;
    uint8_t fail, fail_status;      /* why the start failed: HCI_FAIL_*, and the status or hardware error code */
    uint16_t fail_op;               /* the command it failed at (HCI_FAIL_STATUS, HCI_FAIL_TIMEOUT) */

    uint8_t ncmd;                   /* commands the controller takes now */
    uint16_t wait_op;               /* the command sent and not yet answered, HCI_OP_NOP for none */
    uint32_t wait_ms;
    uint8_t cmd[HCI_CMD_MAX];       /* a command packed but not yet taken by the transport */
    uint16_t ncmd_bytes;

    uint16_t acl_len;               /* the controller's ACL buffers: payload size and count, and how many are free */
    uint8_t acl_total, acl_free;
    uint8_t bdaddr[6];

    uint16_t conn;                  /* the connection's handle, HCI_NO_CONN for none */
    uint8_t peer_type, peer[6];
    uint16_t interval, latency, timeout;
    uint8_t disc_reason;            /* the last Disconnection Complete's reason */

    uint8_t rx[HCI_L2CAP_MAX];      /* an L2CAP PDU coming in, fragment by fragment */
    uint16_t nrx;
    uint8_t rx_on;                  /* a PDU's first fragment has come and it is not whole yet */
    uint8_t tx[HCI_L2CAP_MAX];      /* an L2CAP PDU going out, fragment by fragment */
    uint16_t ntx, tx_off;

    uint32_t cmds, events, acl_in, acl_out, l2cap_in, l2cap_out;
    uint32_t rx_dropped;            /* incoming PDUs or fragments thrown away (no start, too long, other handle) */
    uint32_t bad_events;            /* malformed events, and events for nothing the host did */
    uint32_t connects, disconnects;
} hci_host;

void hci_host_init(hci_host *h, const hci_transport *t, hci_l2cap_fn on_l2cap, void *ctx);

/* Starts the controller and advertising with these settings. HCI_EBUSY when already started (not OFF or FAILED),
 * HCI_EINVAL when a payload is over 31 bytes. */
int hci_host_start(hci_host *h, const hci_adv_cfg *adv);

/* The controller's packet handler: the transport calls it with every packet the controller sends. */
void hci_host_receive(hci_host *h, uint8_t type, const uint8_t *p, uint16_t n);

/* From the main loop: sends what waits for a credit or for the transport, and times out a silent command. */
void hci_host_poll(hci_host *h, uint32_t elapsed_ms);

/* Queues one L2CAP PDU (channel, information payload) on the connection; it goes out in fragments of the
 * controller's ACL size as buffers free up. HCI_EBUSY while not connected or while the previous PDU is still going
 * out; HCI_EINVAL when longer than HCI_L2CAP_MAX - 4. */
int hci_host_send_l2cap(hci_host *h, uint16_t cid, const uint8_t *p, uint16_t n);

/* Ends the connection with this reason (HCI_ERR_REMOTE_USER_TERM: the user ended it). HCI_EBUSY when not connected or
 * a command is still waiting. Advertising starts again on the Disconnection Complete. */
int hci_host_disconnect(hci_host *h, uint8_t reason);

#endif
