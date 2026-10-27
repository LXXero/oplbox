/* SPDX-License-Identifier: GPL-3.0-only
 * OPLBOX's driver for the FM-1's Bluetooth LE baseband (docs/BLUETOOTH.md "Cordio path as built" step 3): power-up of the
 * Bluetooth side of the radio, the analog and modem block, the baseband's control block and RAM block, one advertising
 * link (ADV_IND or ADV_NONCONN_IND with an optional scan response), the receive path's raw capture, and the event
 * counters. Own code, written from docs/BLUETOOTH_RADIO.md alone (addresses, bit fields, write order and RAM layouts that
 * tools/ble_ir.py read out of JieLi's btctrler.a as facts; no function of it is translated). It was never run on an FM-1:
 * every [measure on FM-1] in that file is a place where this driver guesses, and tests/ble/bb_model.c only checks the
 * driver against the same facts.
 *
 * The baseband is a small per-link engine: the CPU fills a parameter block in RAM and a few registers and the hardware
 * runs whole advertising events itself, raising one pending flag per event and one per received packet. The driver polls
 * those flags from the main loop (fm1ble_poll); there is no interrupt hookup.
 *
 * Library only: the register accesses are the port's (fm1ble_port.h).
 */
#ifndef OPLBOX_BLE_FM1BLE_H
#define OPLBOX_BLE_FM1BLE_H

#include <stdint.h>

#define FM1BLE_OK       0
#define FM1BLE_EARG     (-1)    /* a bad argument */
#define FM1BLE_ESTATE   (-2)    /* not in the state this call needs */
#define FM1BLE_ETIMEOUT (-3)    /* a bounded wait on the hardware ran out */

/* The calibration the analog block starts from. JieLi keeps five values from the chip's calibration store
 * (keep_BT_CMPUK1..3, keep_BT_OFSI, keep_BT_OFSQ); an FM-1 may not have them. With `have` 0 the constants the
 * power-up writes stand (0x80 compensation, no offset). */
typedef struct {
    uint8_t have;
    uint8_t cmpuk[3];           /* TX I/Q compensation: 0x2FC10 bits 7-0, 15-8, and 23-16 as (-cmpuk[2]) & 0xFF */
    int16_t ofsi, ofsq;         /* TX I/Q offsets: 0x2FC08 bits 9-0 and 19-10 */
} fm1ble_cal;

/* One advertising link's settings. */
typedef struct {
    uint8_t type;               /* PDU type: 0 ADV_IND, 2 ADV_NONCONN_IND, 6 ADV_SCAN_IND (E.4) */
    uint8_t own_random;         /* TxAdd: 1 when the advertiser address is a random one */
    uint8_t chmap;              /* advertising channels, bit 0 = 37, 1 = 38, 2 = 39; 0 means all three */
    uint8_t addr[6];            /* the advertiser address, as sent: least significant byte first */
    uint16_t interval_slots;    /* the advertising interval in 625 us slots (32 = 20 ms) */
    uint8_t filter;             /* the advertising filter policy, 0-3 */
    uint8_t adv[31], nadv;      /* advertising data */
    uint8_t rsp[31], nrsp;      /* scan response data; buffer 1, used for ADV_IND and ADV_SCAN_IND */
} fm1ble_adv;

/* What happened, for the diagnostic page: the readbacks, the interrupt counts, the first things that went wrong. */
typedef struct {
    int err;
    const char *stage;          /* where fm1ble_radio_up or fm1ble_bb_init stopped: "RF", "ANALOG", "BB", "BLOCK", "DONE" */
    uint8_t variant;            /* 0: without the shared radio's common init, 1: with it (fm1ble_port_rf_common) */
    int rf_common;              /* what fm1ble_port_rf_common returned */
    uint32_t rb[8];             /* readbacks after the power-up: WL_CON0, 0x2FC40, 0x2FC78, 0x2FC00, 0x2FC04, 0x2FC20, 0x2FC98, 0x2FC9C */
    uint32_t rb_want[8];        /* what each was written to (after masking what reads back as it was written) */
    uint32_t rb_ok, rb_bad;     /* how many of them read back as written */
    uint32_t bb[8];             /* after fm1ble_bb_init: 0x28000, 0x28008, 0x28010, 0x28034, 0x2FC84, 0x2FCBC, 0x2FC80, busy */
    uint32_t events;            /* event-done flags seen (pending bit id) */
    uint32_t rx_packets;        /* packet-received flags seen (pending bit id + 8) */
    uint32_t rearms;            /* times the driver armed the next event itself (fm1ble_set_rearm) */
    uint32_t rx_conn_ind;       /* CONNECT_IND PDUs seen (PDU type 5) */
    uint32_t rx_scan_req;       /* SCAN_REQ PDUs seen (type 3) */
    uint32_t rx_other;
    uint16_t rx_hdr[2], rx_stat[2];   /* the last packet's RXAHDR/RXDHDR words and RXSTAT (buffer 0 then 1) */
    uint8_t rx_last[48];        /* the last received PDU's first bytes (the payload), rx_last_n of them */
    uint8_t rx_last_n, rx_last_type, rx_last_len;
    uint32_t pending_last;      /* the last value of the pending register */
    uint32_t tx_flips;          /* events after which TXTOG had changed: the hardware's record that it sent a buffer (E.4) */
    uint16_t txtog;             /* TXTOG as last read */
    uint32_t ram_end;           /* 0x2FCBC as read back after fm1ble_bb_init (JieLi writes it and never reads it: D.1) */
    uint32_t op_adv_done;       /* fm1ble_op_adv events (37, 38 and 39) the baseband ended */
    uint32_t op_conn_done;      /* fm1ble_op_conn events the baseband ended */
} fm1ble_diag;

/* The driver's state. The caller owns it and the RAM block (word aligned, at least FM1BLE_BLOCK_SIZE bytes; the whole
 * block must be below 64 KB of its start, which the size guarantees). */
#define FM1BLE_BLOCK_SIZE   2304u
/* The transmit power level (E.7: TXPWR = 0x10 + level): JieLi's SDK sets 6 unless the configuration store says otherwise,
 * and documents 0-11 (docs/BLUETOOTH_RADIO.md "The TX power level"). Level 0 is the lowest. */
#define FM1BLE_TXPWR_LEVEL  6u
#define FM1BLE_TXPWR_MAX    11u
typedef struct {
    uint8_t *blk;               /* the RAM block */
    uint32_t blk_bus;           /* its address as the baseband takes it */
    uint16_t entity;            /* offset of entity 0 in the block */
    uint16_t rx[2], tx[2];      /* offsets of the receive and transmit buffers */
    uint8_t on, link_open, adv_on;
    uint8_t rearm, hop;
    uint8_t op_on, op_chan, op_state;   /* fm1ble_op_*: an operation armed, its channel, its state code (2 or 7) */
    uint8_t op_slot;            /* the TX slot (TXTOG & 1 when armed) the advertising PDU went into; the response takes the other */
    uint8_t op_rsp;             /* TX buffer 1 holds the scan response fm1ble_op_resp put in */
    uint8_t txpwr;              /* the transmit power level in both TXPWR words */
    uint8_t adv_type;           /* fm1ble_adv_start's PDU type, for re-pointing the buffers after each event */
    uint16_t slots;             /* the interval in slots, for re-arming */
    uint32_t ana_pll[243];      /* the PLL table (F.2): 81 entries of 3 words, the analog block reads it */
    fm1ble_cal cal;
    fm1ble_diag d;
} fm1ble;

/* The Bluetooth side of the radio: B.2, B.1, the analog and modem block (C.1) and the AGC block (C.3), with delays.
 * variant 1 first calls fm1ble_port_rf_common (docs/BLUETOOTH_RADIO.md "not settled" 1: which is needed is a
 * measurement). Fills b->d.rb. A step whose readback differs is counted, not an error: the baseband may change a bit
 * that was written. Returns FM1BLE_OK. */
int fm1ble_radio_up(fm1ble *b, int variant, const fm1ble_cal *cal);

/* The baseband's control block and RAM block (D.1, E.1) with entity 0 opened and left in standby (E.3). */
int fm1ble_bb_init(fm1ble *b, void *block);

/* Starts advertising on the open link (G): the parameter block, the buffers, the interval, the state, the channel
 * index, the interrupts and the first event armed. FM1BLE_ESTATE before fm1ble_bb_init or while advertising, FM1BLE_EARG
 * for a bad setting. */
int fm1ble_adv_start(fm1ble *b, const fm1ble_adv *a);

/* Stops advertising (the link goes to standby: anchor counter off, wait for the hardware to go idle, interrupts off). */
int fm1ble_adv_stop(fm1ble *b);

/* After each event the next one is armed by the driver (D.4 says JieLi's event handler does bookkeeping; whether the
 * hardware repeats events by itself is [unchecked]). Off by default; the test run turns it on if events do not repeat. */
void fm1ble_set_rearm(fm1ble *b, int on);

/* From the main loop: reads the pending flags, clears what it saw, counts events and received packets (and keeps
 * the last received PDU's raw bytes), re-arms the next event when asked. After each event it reads TXTOG (counting a
 * change in tx_flips) and puts the advertising PDU back in the TX slot TXTOG & 1 names and the scan response in the
 * other, as JieLi's event handler does (docs/BLUETOOTH_RADIO.md "TX and RX toggles"). Returns the number of flags handled. */
int fm1ble_poll(fm1ble *b);

/* Power-down (B.5) and the baseband's close. */
void fm1ble_radio_down(fm1ble *b);

/* The last received PDU's payload and type, or 0 bytes. */
const uint8_t *fm1ble_rx_last(const fm1ble *b, uint8_t *n, uint8_t *type);

/* ---- One packet at a time, for Cordio's PAL (firmware/ble/cordio_port/pal_fm1.c, PAL_NEEDED.md)
 *
 * Cordio hands the baseband one packet with a time; the baseband runs a whole event from a parameter block. On the open
 * link (fm1ble_bb_init), our design from the facts:
 *   - an advertising event: state 2 over channels 37, 38 and 39 (the hardware hops), armed as fm1ble_adv_start arms it
 *     (the only advertising a central heard from the FM-1, at 122afe3; one-channel events, hop 0, were never heard, and
 *     on 38 and 39 never ended), the PDU in TX buffer 0 and the response (SCAN_RSP) in buffer 1. The PAL serves Cordio's
 *     three per-channel packets with one such event;
 *   - a connection event as the peripheral (channel 0-36): state 7 on a fixed channel (index 6 = channel | 0x2000, as
 *     the direct test sets one, K), the master's packet into an RX buffer, the response from TX buffer 0.
 * The hardware turns around by itself after T_IFS (150 us) and sends the response buffer: fm1ble_op_resp must fill it
 * before then, which is why the PAL fills it from the receive interrupt. [measure on FM-1] every one of: state 7 with a
 * fixed channel, the response read at T_IFS rather than at the start of the event, the header
 * words of a data PDU (TXDHDR, RXDHDR: written as RXDHDR's layout in E.5 says), the window's unit (taken as us).
 * PDUs are Cordio's: two header bytes (the first: type or LLID/NESN/SN/MD, the second: the length), then the payload. */

#define FM1BLE_EV_DONE  1u      /* the event ended (pending bit id) */
#define FM1BLE_EV_RX    2u      /* a packet came in (pending bit id + 8) */

typedef struct {
    uint8_t ok;                 /* the packet's status says good (RXSTAT bits 3-0 = 1, E.5 [unchecked]) */
    uint8_t hdr[2];             /* its header as on the air: the first byte and the length */
    const uint8_t *payload;     /* in the RX buffer: copy it out before the next packet */
    uint16_t stat;              /* the RXSTAT word, raw */
} fm1ble_pkt;

/* The access address, CRC init (24 bits) and channel (0-39) of the next operation. FM1BLE_EARG for a channel over 39. */
int fm1ble_op_chan(fm1ble *b, uint32_t aa, uint32_t crc, uint8_t chan);
/* The transmit power level for both TX buffers (E.7: 0x10 + level, at most FM1BLE_TXPWR_MAX; FM1BLE_TXPWR_LEVEL from
 * fm1ble_bb_init on). For fm1ble_adv_start's advertising as much as for the operations. */
void fm1ble_op_power(fm1ble *b, uint8_t level);
/* An advertising event on 37, 38 and 39, its first packet in the slot delay_us falls in (whole 625 us slots, no bit
 * offset, as fm1ble_adv_start arms). The PDU (it starts with AdvA, which becomes the local address the hardware matches
 * requests against) goes into the TX slot TXTOG & 1 names (the one the hardware sends next); the other slot holds the
 * last scan response fm1ble_op_resp put in, or the PDU again until there is one. A channel must be set (37-39: which one
 * does not matter, the event covers all three). FM1BLE_ESTATE before fm1ble_bb_init or while fm1ble_adv_start's
 * advertising runs, FM1BLE_EARG for a data channel or a PDU under 8 or over 39 bytes. */
int fm1ble_op_adv(fm1ble *b, uint32_t delay_us, const uint8_t *pdu, uint16_t n);
/* A connection event as the peripheral on the channel set (0-36): listen from delay_us from now for window_us. */
int fm1ble_op_conn(fm1ble *b, uint32_t delay_us, uint32_t window_us);
/* The packet the hardware sends after T_IFS: TX buffer 1 in an advertising event (kept for the next events' SCAN_REQs),
 * buffer 0 in both TX slots in a connection event. At most 37 bytes of payload (the buffers' size, E.4). */
int fm1ble_op_resp(fm1ble *b, const uint8_t *pdu, uint16_t n);
/* The event off: anchor counter stopped, the link's interrupts off. No wait (it may run from an interrupt). */
void fm1ble_op_stop(fm1ble *b);
/* Reads and clears the pending flags of the operation in flight: FM1BLE_EV_DONE and FM1BLE_EV_RX, with the packet in
 * *p for RX. 0 when nothing is pending. Polled, or called from IRQ 29 / 45 (D.4). */
uint32_t fm1ble_op_service(fm1ble *b, fm1ble_pkt *p);

#endif
