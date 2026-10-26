/* SPDX-License-Identifier: GPL-3.0-only
 * The BLE-MIDI profile (docs/BLUETOOTH.md stage 2): the service and characteristic, the packet encoder and decoder,
 * and the bridge to the MIDI input path the app already reads.
 *
 * Own code, written from "Specification for MIDI over Bluetooth Low Energy (BLE-MIDI)" 1.0, MMA/AMEI RP-052,
 * November 1, 2015 (the MIDI Association). The section numbers below are that document's. Nothing here is from JieLi's
 * btstack.a, from Apple's headers or from another BLE-MIDI implementation. The document has field tables and
 * figures, no packets in hex: the test's byte-for-byte vectors are worked out by hand from those rules.
 *
 * A BLE-MIDI packet is the value of one GATT write (central to us) or one notification (us to the central), at most
 * ATT MTU - 3 bytes (section 7). It is a header byte, then timestamp bytes and MIDI messages:
 *   header     1 0 tttttt        bit 6 reserved 0; timestampHigh, the top 6 bits of a 13-bit millisecond time
 *   timestamp  1 ttttttt         timestampLow, the low 7 bits; before every status byte
 *   message    status, data...   or, with the status left out, a running-status message (data bytes only)
 * A timestamp low that is smaller than the one before it in the same packet means the 13-bit time wrapped past a
 * multiple of 128 ms: the receiver adds one to timestampHigh (section 9). Only SysEx spans packets: a continuation
 * packet is a header followed by data bytes with no timestamp byte (section 8).
 *
 * No allocation, no waiting, no register access; the caller owns every buffer and calls from one context (the main
 * loop). Host build and firmware build alike. Includes only <stdint.h>.
 */
#ifndef OPLBOX_BLE_MIDI_H
#define OPLBOX_BLE_MIDI_H

#include <stdint.h>

/* ---- the GATT database (section 3) ---- */
#define BLE_MIDI_SERVICE_UUID   "03B80E5A-EDE8-4B33-A751-6CE34EC4C700"
#define BLE_MIDI_CHAR_UUID      "7772E5DB-3868-4112-A1A9-F2669D106BF3"
/* the same, as the 16 bytes ATT and the advertising data carry: least significant byte first */
extern const uint8_t ble_midi_service_uuid[16];
extern const uint8_t ble_midi_char_uuid[16];
/* the characteristic's properties (Core Specification Vol 3 Part G 3.3.1.1): read 0x02, write without response 0x04
 * (required by section 3), notify 0x10. Encryption is "recommended", not required: stage 3 decides what pairing does. */
#define BLE_MIDI_CHAR_PROPS     0x16
/* the connection interval the device must request, in 1.25 ms units: 15 ms or less (section 4) */
#define BLE_MIDI_CONN_INTERVAL_MAX 12
/* the largest ATT value a BLE-MIDI packet can need is ATT MTU - 3 (section 7); the smallest MTU is 23 */
#define BLE_MIDI_DEFAULT_PACKET 20

/* The first read of the characteristic must answer with no payload (section 5): the value read is empty. */
uint32_t ble_midi_read_value(uint8_t *out);

/* Advertising data: Flags (LE General Discoverable, BR/EDR not supported) and the Complete List of 128-bit Service
 * UUIDs holding the BLE-MIDI service; 21 bytes. The scan response: the device name, Complete Local Name, or the
 * Shortened one when it does not fit in 31 bytes. Both return the length. */
uint32_t ble_midi_adv_data(uint8_t *out);
uint32_t ble_midi_scan_rsp(uint8_t *out, const char *name);

/* ---- messages ---- */
/* Bytes in the message that starts with this status byte, 0 for one this profile does not carry: a status below 0x80,
 * 0xF0 (SysEx, which has its own call), 0xF4, 0xF5 and 0xF7. */
uint8_t ble_midi_msg_len(uint8_t status);

/* ---- the encoder ---- */
/* Called with a finished packet to send as one notification. 0: sent; else the packet is dropped and counted. */
typedef int (*ble_midi_send_fn)(void *ctx, const uint8_t *packet, uint32_t n);

#define BLE_MIDI_OK     0
#define BLE_MIDI_EBAD   (-1)    /* a message this profile cannot carry, or a bad argument */
#define BLE_MIDI_ESEND  (-2)    /* the send function refused a packet; the packet is dropped */

typedef struct {
    uint8_t *buf;               /* the packet being built, cap bytes */
    uint32_t cap, len;          /* len 0: no packet started */
    ble_midi_send_fn send;
    void *ctx;
    uint8_t high;               /* the timestampHigh the receiver holds now: the header's, plus wraps */
    uint8_t prev_low;           /* the last timestamp byte's low 7 bits in this packet */
    uint8_t have_prev;
    uint8_t rs;                 /* the running status: the last channel message's status in this packet, or 0 */
    uint8_t force_ts;           /* a System Common or Real-Time message went by: the next message needs a timestamp */
    uint32_t packets, dropped;  /* sent, and refused by send */
} ble_midi_enc;

/* buf holds cap bytes: the ATT MTU - 3, at least 5 (a header, a timestamp and a three-byte message). 0, or EBAD. */
int ble_midi_enc_init(ble_midi_enc *e, uint8_t *buf, uint32_t cap, ble_midi_send_fn send, void *ctx);
/* Add one message (status, d1, d2: the bytes the status says it has; the rest are ignored) at time ts, in
 * milliseconds. Only the low 13 bits of ts matter. Messages share a packet while they fit and their timestamps stay
 * within what the receiver can rebuild; an earlier time than the last one starts a new packet. */
int ble_midi_enc_msg(ble_midi_enc *e, uint32_t ts, uint8_t st, uint8_t d1, uint8_t d2);
/* Add a whole System Exclusive message, 0xF0 ... 0xF7, split over as many packets as it needs. */
int ble_midi_enc_sysex(ble_midi_enc *e, uint32_t ts, const uint8_t *data, uint32_t n);
/* Send the packet being built, if any: call it once per connection interval, or when the queue of messages ends. */
int ble_midi_enc_flush(ble_midi_enc *e);

/* ---- the decoder ---- */
typedef struct ble_midi_dec ble_midi_dec;
struct ble_midi_dec {
    /* A channel, System Common or Real-Time message: m[0] is the status, n bytes in all. ts: the 13-bit time. */
    void (*msg)(void *ctx, uint16_t ts, const uint8_t *m, uint8_t n);
    /* Some bytes of a SysEx message, in order: first = the chunk starts with 0xF0, last = it ends with 0xF7. */
    void (*sysex)(void *ctx, uint16_t ts, const uint8_t *p, uint32_t n, int first, int last);
    void *ctx;
    uint8_t in_sysex;           /* a SysEx has started and not ended (it may continue in the next packet) */
    uint32_t packets, bad;      /* packets taken; malformed bytes or messages dropped */
};

/* Forget a SysEx in progress (a new connection). */
void ble_midi_dec_reset(ble_midi_dec *d);
/* Take one packet (the value of a GATT write). Returns how many malformed pieces it dropped: 0 for a clean packet.
 * It drops a piece and goes on, so one bad byte does not lose the rest of the packet. */
uint32_t ble_midi_dec_packet(ble_midi_dec *d, const uint8_t *p, uint32_t n);

/* ---- the bridge to the app's MIDI input ---- */
/* platform.c's midi_get() hands the app one message at a time: (status, data 1, data 2), 1 when there is one. SysEx
 * does not go through it: the USB path gives SysEx to the update and slot code a whole message at a time, so a
 * complete BLE SysEx goes to the same kind of sink. Timestamps are not kept: a message is acted on when it arrives
 * (stage 5 can use them to smooth the delivery jitter). */
#define BLE_MIDI_RING 64        /* messages queued between the Bluetooth callback and the app's main loop */

typedef struct {
    ble_midi_dec dec;
    uint8_t ring[BLE_MIDI_RING][3];
    uint32_t r, w;              /* free-running: r == w is empty */
    uint8_t *sx;                /* the SysEx being put together, sx_cap bytes (0xF0 and 0xF7 included) */
    uint32_t sx_cap, sx_len;
    uint8_t sx_over;            /* this SysEx did not fit: dropped when it ends */
    void (*sysex)(void *ctx, const uint8_t *msg, uint32_t n);   /* a whole SysEx message, 0xF0 ... 0xF7 */
    void *sx_ctx;
    uint32_t dropped;           /* messages lost to a full ring */
    uint32_t sysex_dropped;     /* SysEx messages lost: too long, or with no sink */
} ble_midi_in;

/* sx/sx_cap: room for the longest SysEx to take (the slot push's frame is the largest); sink may be NULL to drop all. */
void ble_midi_in_init(ble_midi_in *in, uint8_t *sx, uint32_t sx_cap,
                      void (*sink)(void *ctx, const uint8_t *msg, uint32_t n), void *ctx);
/* The GATT write handler: one packet in. Returns the decoder's count of malformed pieces. */
uint32_t ble_midi_in_write(ble_midi_in *in, const uint8_t *packet, uint32_t n);
/* The same contract as midi_get(): 1 and a message, or 0. Data bytes a message does not have read as 0. */
int ble_midi_get(ble_midi_in *in, uint8_t *st, uint8_t *d1, uint8_t *d2);
/* A new connection: nothing queued, no SysEx half-received. */
void ble_midi_in_reset(ble_midi_in *in);

#endif
