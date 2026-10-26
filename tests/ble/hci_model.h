/* SPDX-License-Identifier: GPL-3.0-only
 * A fake BLE controller for the host build (docs/BLUETOOTH.md stage 1), in the style of tests/wifi/sdio_model.c: it is
 * the other end of firmware/ble/hci.h's transport, answers commands as the Bluetooth Core Specification says a
 * controller does (with values from a script), plays a central that connects, sends L2CAP PDUs and disconnects,
 * checks what the host sends, and logs every packet both ways in a plain-text fixture format. Own code. Host only:
 * never built into the firmware. Nothing in it is measured off JieLi's controller.
 *
 * What it checks (each failure counted in bad, the first one's reason in why): a known command with the wrong
 * parameter length, a command sent without a credit (Num_HCI_Command_Packets), a packet type other than command or
 * ACL, ACL data with no connection or for another handle, longer than the buffer size it reported, beyond the
 * buffers it has free, or with a Packet_Boundary flag out of order. It answers like a controller: LE Set Advertising
 * Parameters with out-of-range values and advertising data over 31 bytes with Invalid HCI Command Parameters (0x12),
 * advertising on while connected with Command Disallowed (0x0C), an unknown command with Unknown HCI Command (0x01).
 * It sends an event only when the host's event masks let it (after Reset: the default masks, which leave LE Meta
 * events off).
 *
 * Answers are queued and delivered on the next hci_model_tick, as from another task; with "sync" they are delivered
 * inside the host's send call instead (the host must cope with both: which one JieLi's controller does is not
 * known).
 *
 * Log and fixture format (plain text, one packet per line; "#" starts a comment; blank lines are ignored; numbers are
 * hex; "??" in a fixture matches any one byte):
 *
 *   cmd 0c03                          host to controller: a command's opcode, then its parameter bytes
 *   evt 0e 01 03 0c 00                controller to host: an event's code, then its parameter bytes
 *   acl> 0040 0 07 00 04 00 ...       host to controller: ACL data: handle, Packet_Boundary flag, data bytes
 *   acl< 0040 2 07 00 04 00 ...       controller to host
 *
 * Script (same lexical rules). Settings, any order:
 *
 *   bufsize 27 2                      LE Read Buffer Size's answer: ACL payload length and count (0 0: none of its
 *                                     own, so the host must ask Read Buffer Size)
 *   shared 27 4                       Read Buffer Size's answer: ACL payload length and count
 *   bdaddr 02:46:4d:31:00:01          its public address, written most significant byte first
 *   ncmd 1                            Num_HCI_Command_Packets in every answer
 *   status 2006 12                    answer this command with this status instead
 *   silent 0c03                       never answer this command
 *   sync                              answer inside the host's send call
 *
 * Steps, run in order, each when the one before it has run:
 *
 *   wait adv | noadv                  until advertising is on (off)
 *   wait ms 100                       100 ms after the step before
 *   wait l2cap 1                      until the host has sent this many whole L2CAP PDUs in all
 *   nop                               Command Complete with opcode 0 and Num_HCI_Command_Packets 1: one credit
 *   connect handle=0040 peer=c0:ff:ee:00:00:01 type=1 interval=24 latency=0 timeout=72
 *                                     LE Connection Complete as peripheral; advertising goes off
 *   update interval=12 latency=0 timeout=72        LE Connection Update Complete
 *   l2cap cid=0004 frag=27 02 17 00   an L2CAP PDU from the central, in ACL fragments of frag bytes (default 27)
 *   acl pb=1 [handle=0099] 01 02 03   one raw ACL fragment, on the connection or on the handle given
 *   disconnect reason=13              Disconnection Complete: the central left
 *   hwerror 01                        Hardware Error
 *   nocp auto | hold | release        Number Of Completed Packets after each ACL packet from the host (auto, the
 *                                     default), none until release, then one for all held
 */
#ifndef OPLBOX_TESTS_BLE_HCI_MODEL_H
#define OPLBOX_TESTS_BLE_HCI_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include "hci.h"

#define HCI_MODEL_LOG   512
#define HCI_MODEL_LINE  200
#define HCI_MODEL_STEPS 64
#define HCI_MODEL_QUEUE 32

typedef struct {
    uint8_t kind;
    uint32_t a, b, c, d;
    uint8_t addr[6];
    uint8_t data[HCI_L2CAP_MAX];
    uint16_t n;
} hci_model_step;

typedef struct {
    /* settings */
    uint16_t buf_len, shared_len;
    uint8_t buf_count, shared_count;
    uint8_t bdaddr[6];
    uint8_t ncmd;
    uint16_t status_op, silent_op;
    uint8_t status;
    uint8_t sync;
    /* steps */
    hci_model_step steps[HCI_MODEL_STEPS];
    int nsteps, cur;
    uint32_t step_t;
    /* the controller */
    uint32_t now;
    uint8_t credits;                 /* commands it takes now */
    uint64_t evmask, lemask;
    uint8_t adv_on;
    uint16_t conn;
    uint8_t nocp_mode;               /* 0 auto, 1 hold */
    uint16_t held;
    uint16_t outstanding;            /* ACL packets from the host not yet given back */
    /* the central's view of what the host sent */
    uint8_t l2[HCI_L2CAP_MAX];
    uint16_t nl2;
    uint8_t l2_on;
    uint16_t last_cid;
    uint8_t last[HCI_L2CAP_MAX];
    uint16_t nlast;
    uint32_t l2cap_from_host, acl_from_host, cmds;
    /* answers waiting for the next tick */
    uint8_t q[HCI_MODEL_QUEUE][HCI_EVT_MAX > HCI_ACL_MAX ? HCI_EVT_MAX : HCI_ACL_MAX];
    uint16_t qn[HCI_MODEL_QUEUE];
    uint8_t qt[HCI_MODEL_QUEUE];
    int nq;
    /* where its packets go: the host's hci_host_receive */
    void (*deliver)(void *ctx, uint8_t type, const uint8_t *p, uint16_t n);
    void *ctx;
    /* log and checks */
    char log[HCI_MODEL_LOG][HCI_MODEL_LINE];
    int nlog;
    int bad;
    char why[200];
} hci_model;

/* Defaults: bufsize 27 2, shared 27 4, bdaddr 02:46:4d:31:00:01, ncmd 1, no steps. */
void hci_model_init(hci_model *m, void (*deliver)(void *ctx, uint8_t type, const uint8_t *p, uint16_t n), void *ctx);

/* Reads a script file. Returns 0, or -1 with the reason in m->why. */
int hci_model_load(hci_model *m, const char *path);

/* The host's transport: hci_transport.send with ctx = the model. Always takes the packet. */
int hci_model_send(void *ctx, uint8_t type, const uint8_t *p, uint16_t n);

/* Advances its clock: delivers what is queued, then runs the steps whose time has come. */
void hci_model_tick(hci_model *m, uint32_t ms);

/* 1 when every step has run. */
int hci_model_done(const hci_model *m);

/* Compares the log with a fixture file. Returns 0 when they match, else 1 with the first difference in why. */
int hci_model_diff(const hci_model *m, const char *path, char *why, size_t n);

/* Writes the log in the fixture format (to keep a run as a fixture). */
int hci_model_write(const hci_model *m, const char *path);

#endif
