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

#define BB_LOG 4096

/* The air, for the PAL's tests (air set): what a central does in each event, and the interrupt handler the baseband
 * raises (IRQs 45 and 29, called at once: an interrupt that runs within T_IFS, the assumption fm1ble.h names). The
 * model runs an advertising event (state 2) as: on each channel of anchor index 6 (from the first, by the hop increment,
 * to the map's end) the PDU of the TX slot TXTOG & 1 names (length from TXDHDR), then, when the central answers, its
 * request in RX buffer 0 (the receive flag), for a SCAN_REQ the SCAN_RSP from the other slot T_IFS after it, a
 * CONNECT_IND ending the event; TXTOG changes once after the event. The receiver hears a request only when the AGC block's
 * gain table was written whole (0x2FD98, then fm1ble_agc.h's words to 0x2FD9C) and WINCNTL2 is not 0: at 3b39ab4 the
 * driver wrote neither and the FM-1 received nothing (which of the two it needs is what the FM-1's RX trials say; the
 * model needs both). A one-channel event (hop 0) does what the FM-1 did at
 * 122afe3: on 37 it ends having sent nothing, on 38 and 39 it never ends; a connection event (state 7) as: the central's packet received when it starts inside the window
 * (WINCNTL2, us, as le_hw_ioctrl's __set_winsize puts it; the receiver set up as above), then T_IFS after its end the PDU of the TX slot TXTOG & 1 names (header TXDHDR bits 4-0, length bits 15-8), TXTOG
 * changed after it: the slot is read at that time, so a response put in after T_IFS is not the one sent; the event
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
    /* how the chip places a connection event (the open questions of fm1ble.h FM1BLE_CONN_PADJ, PADB, PADW): cnt_jieli, a
     * slot later than op_arm's guess (the counter as JieLi writes it); bit15, the bit offset only with its bit 15; win0,
     * the window from WINCNTL0/1 instead of WINCNTL2 */
    int cnt_jieli, bit15, win0;
    uint64_t now_us, armed_at_us;
    int armed;
    uint32_t period_us;
    uint32_t clk_calls, rf_common_calls, sync_calls;
    uint32_t clk_bits;
    uint8_t *blk;                       /* the RAM block the driver gave the baseband (from 0x2FC84) */
    uintptr_t hi;                       /* the high half of host pointers, since the bus address is 32 bits */
    const bb_air *air;                  /* set: events run on the air (above), one-shot, at slots x 625 + bit offset */
    uint32_t air_events, air_missed;    /* events run; connection events whose packet fell outside the window */
    uint32_t tx_sent;                   /* advertising PDUs the air sent (TXTOG changes once an event) */
    uint32_t one_chan;                  /* one-channel advertising events (hop 0): the shape the FM-1 never sent (122afe3) */
    int agc_n, agc_bad;                 /* the gain table's words written since 0x2FD98 = 0; how many differ from fm1ble_agc.h */
    uint32_t rx_deaf;                   /* requests the receiver did not hear: no gain table, or WINCNTL2 0 */
    /* connection events whose packet fell inside the window: how many, how many of them were armed with the counter at
     * 0 (the window open at the write), and the earliest and latest packet start after the window's start, in us */
    uint32_t conn_in, conn_at_once;
    int64_t conn_off_min, conn_off_max;
    int resp_pending;                   /* a connection event waits for T_IFS to send its response (conn_respond) */
    uint64_t resp_at;
    /* hw_nesn: a data PDU's NESN (header bit 2) is the hardware's, not TXDHDR bit 2: the received packet's SN plus one
     * (fm1ble.h FM1BLE_TX_JIELI: ble_hw_tx keeps TXDHDR bit 2 as it is, so the chip writes it [unchecked what it holds]);
     * rx_sn the last received data packet's SN. jump_reads, jump_us: after a connection packet's flag, the clock jumps
     * jump_us forward at the jump_reads-th read of fm1ble_port_now_us (once; 0: never), a main loop that comes back late */
    int hw_nesn;
    uint8_t rx_sn;
    /* hw_arq: the chip keeps SN and NESN itself, as fm1ble.h FM1BLE_TX_ARQ reads the IR: TXDHDR bits 1-0 LLID, bit 2 the
     * slot's SN, bit 3 MD; a central's packet whose NESN differs from the SN of the slot TXTOG names acknowledges it and
     * TXTOG changes (the other slot goes at T_IFS); one whose SN is rx_exp is new: rx_exp and RXTOG change; RXDHDR in the
     * chip's layout (LLID, MD in bit 3, bits 2 and 4 clear); the response's NESN is rx_exp, nothing changes after it.
     * drop_rsp: the central does not hear every drop_rsp-th response (0: hears all), so it sends its packet again;
     * drop_data: it does not hear the first response with a payload (once); jump_drop: nor the response of the event the
     * clock jumps in (jump_us, once) */
    int hw_arq, drop_data, jump_drop, drop_one;
    uint8_t rx_exp;
    uint32_t drop_rsp, rsp_n;
    uint32_t jump_reads, jump_us, jump_left;
    void (*tick)(void);                 /* set: called each time the clock is advanced (a test's hook inside a busy wait) */
} bb_model;

extern bb_model bbm;

void bb_model_reset(void);
/* Raise pending flags (bit id: the event's end, id + 8: a packet), as the hardware would. */
void bb_model_pend(uint32_t bits);
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
