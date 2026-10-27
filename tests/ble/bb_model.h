/* SPDX-License-Identifier: GPL-3.0-only
 * A register model of the BLE baseband for the host build (docs/BLUETOOTH.md "Cordio path as built" step 3): the port of
 * firmware/ble/fm1ble.c (fm1ble_port.h) with memory behind every register the driver uses, the indexed anchor register file
 * (D.2), the busy bit, a log of every write, and a baseband that runs advertising events as docs/BLUETOOTH_RADIO.md describes
 * them: when the anchor counter is armed, an event-done flag comes after the slots counted (once, or every interval when
 * `repeat` is set), and a packet can be injected into the receive buffer. Written from the same facts the driver is, so it
 * proves the driver does what the document says, not that the chip does. Own code. Host only. */
#ifndef OPLBOX_TESTS_BLE_BB_MODEL_H
#define OPLBOX_TESTS_BLE_BB_MODEL_H

#include <stdint.h>

#define BB_LOG 1024

/* The air, for the PAL's tests (air set): what a central does in each event, and the interrupt handler the baseband
 * raises (IRQs 45 and 29, called at once: an interrupt that runs within T_IFS, the assumption fm1ble.h names). The
 * model runs an advertising event (state 2) as: the PDU of the TX slot TXTOG & 1 names on the channel in anchor index 6
 * (length from TXDHDR, TXTOG changed after it), then, when the central answers, its request in RX buffer 0 (the receive
 * flag), and for a SCAN_REQ the SCAN_RSP from the other slot T_IFS after it; a connection event (state 7) as: the central's packet received when it starts inside the window
 * (WINCNTL, us), then T_IFS after its end the PDU of TX buffer 0 (header TXDHDR0 bits 4-0, length bits 15-8); the event
 * flag at the end of either. Times are on the model's clock, in us. */
typedef struct {
    int (*adv)(void *ctx, uint64_t t, uint8_t chan, const uint8_t *pdu, int n, uint8_t *req);    /* returns the request's length, 0: none */
    void (*scan_rsp)(void *ctx, uint64_t t, const uint8_t *pdu, int n);
    int (*conn)(void *ctx, uint64_t t, uint32_t window, uint8_t chan, uint32_t aa, uint32_t crc, uint8_t *pdu, uint64_t *at);
    void (*conn_rsp)(void *ctx, uint64_t t, const uint8_t *pdu, int n);
    void (*irq)(void *ctx);
    void *ctx;
} bb_air;

typedef struct { uint32_t addr, val; } bb_write;

typedef struct {
    uint32_t regs[512][2];              /* sparse: addr, value */
    int nregs;
    bb_write log[BB_LOG];
    int nlog;
    uint16_t ax[16];                    /* the anchor register file, hw id 0 */
    uint16_t ax_last;                   /* the last value written to 0x28020 */
    uint32_t busy_reads;                /* BUSY reads as busy this many times after the counter is stopped */
    int never_idle;                     /* BUSY stays set */
    int repeat;                         /* the hardware repeats events by itself */
    int silent;                         /* the hardware raises no flag at all */
    int needs_common;                   /* flags only come after fm1ble_port_rf_common ran (the second variant) */
    int no_tx;                          /* events run, but TXTOG never changes: nothing sent (the first FM-1 run's look) */
    int common_ran;
    uint64_t now_us, armed_at_us;
    int armed;
    uint32_t period_us;
    uint32_t clk_calls, rf_common_calls, sync_calls;
    uint32_t clk_bits;
    uint8_t *blk;                       /* the RAM block the driver gave the baseband (from 0x2FC84) */
    uintptr_t hi;                       /* the high half of host pointers, since the bus address is 32 bits */
    const bb_air *air;                  /* set: events run on the air (above), one-shot, at slots x 625 + bit offset */
    uint32_t air_events, air_missed;    /* events run; connection events whose packet fell outside the window */
    uint32_t tx_sent;                   /* advertising PDUs the air sent (each changes TXTOG) */
} bb_model;

extern bb_model bbm;

void bb_model_reset(void);
/* Advance the baseband's clock; raises pending flags as the armed counter says. */
void bb_model_advance_us(uint64_t us);
/* A packet arrives in receive buffer k: PDU type, payload; sets RXAHDR/RXDHDR/RXSTAT as E.5 says and the receive flag. */
void bb_model_rx(int k, uint8_t type, const uint8_t *payload, uint8_t n);
uint32_t bb_model_reg(uint32_t addr);
/* A PDU's time on the air at 1 Mbit/s: preamble, access address, the PDU (two header bytes and the payload), CRC. */
uint32_t bb_model_air_us(int pdu_len);

/* The k-th write to addr (0 based), or -1; value out. */
int bb_model_nth_write(uint32_t addr, int k, uint32_t *val);
/* Index of the first log entry at or after `from` that writes addr with val, or -1. */
int bb_model_find(int from, uint32_t addr, uint32_t val);

#endif
