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
 * flag at the end of either, or, when conn_next gives the central's next packet (it sent MD), that packet T_IFS after the
 * response and the same again, the flag after the last. Times are on the model's clock, in us. */
typedef struct {
    int (*adv)(void *ctx, uint64_t t, uint8_t chan, const uint8_t *pdu, int n, uint8_t *req);    /* returns the request's length, 0: none */
    void (*scan_rsp)(void *ctx, uint64_t t, const uint8_t *pdu, int n);
    int (*conn)(void *ctx, uint64_t t, uint32_t window, uint8_t chan, uint32_t aa, uint32_t crc, uint8_t *pdu, uint64_t *at);
    void (*conn_rsp)(void *ctx, uint64_t t, const uint8_t *pdu, int n);
    void (*irq)(void *ctx);
    void *ctx;
    int (*conn_next)(void *ctx, uint8_t *pdu);    /* the central's next packet in the event, 0: the event ends; may be NULL */
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
    uint32_t later_in;                  /* packets after the first in a connection event (bb_air conn_next) */
    /* hw_nesn: a data PDU's NESN (header bit 2) is the hardware's, not TXDHDR bit 2: the received packet's SN plus one
     * (fm1ble.h FM1BLE_TX_JIELI: ble_hw_tx keeps TXDHDR bit 2 as it is, so the chip writes it [unchecked what it holds]);
     * rx_sn the last received data packet's SN. jump_reads, jump_us: after a connection packet's flag, the clock jumps
     * jump_us forward at the jump_reads-th read of fm1ble_port_now_us (once; 0: never), a main loop that comes back late */
    int hw_nesn;
    uint8_t rx_sn;
    /* hw_arq: the chip keeps SN and NESN itself, as fm1ble.h FM1BLE_TX_ARQ reads the IR: TXDHDR bits 1-0 LLID, bit 2 the
     * slot's SN, bit 3 MD; a central's packet whose NESN differs from the SN of the slot TXTOG names acknowledges it and
     * TXTOG changes (the other slot goes at T_IFS); one whose SN is rx_exp is new: rx_exp and RXTOG change; RXDHDR bits
     * 4-0 as on the air (LLID, NESN, SN, MD: the FM-1's header traces at 1b251ea and 807d70f; up to round 2 of the resumed
     * goal the model wrote the IR's reading, MD in bit 3 and bits 2 and 4 clear); the response's NESN is rx_exp, nothing
     * changes after it.
     * drop_rsp: the central does not hear every drop_rsp-th response (0: hears all), so it sends its packet again;
     * drop_data: it does not hear the first response with a payload (once); jump_drop: nor the response of the event the
     * clock jumps in (jump_us, once) */
    int hw_arq, drop_data, jump_drop, drop_one;
    uint8_t rx_exp;
    /* hold_l2 (with hw_arq): the chip takes the next new L2CAP PDU (LLID 2) as new on the air (rx_exp changes, the
     * central's packet is acknowledged) but RXTOG does not change (once), so the PAL can only judge it a repeat: the shape
     * 1907280 may have had after NimBLE's fragmented response (pal_fm1.h PAL_FM1_CAP) */
    int hold_l2;
    /* crc_bad (with hw_arq): every crc_bad-th packet of the central in a connection arrives with a bad CRC (RXSTAT 2, 0:
     * none): the chip's ARQ leaves rx_exp and TXTOG alone, so the central hears its packet not acknowledged and sends it
     * again, and RXTOG changes, as at every packet with a bad CRC in the FM-1's header trace at 807d70f (5 of 5) */
    uint32_t crc_bad, crc_n;
    /* crc_ack (with crc_bad): the chip's ARQ takes a packet with a bad CRC as it would a good one (the central's packet
     * acknowledged, rx_exp changed when its SN is rx_exp), a chip whose ARQ does not look at the CRC [unchecked on the
     * FM-1] */
    int crc_ack;
    /* ir_rxdhdr (with hw_arq): RXDHDR as the IR reads it (LLID, MD in bit 3, bits 2 and 4 clear), the model's layout up to
     * round 2 of the resumed goal, for the TX modes that lost the link layer at a349877 */
    int ir_rxdhdr;
    /* own_sn (with hw_arq): the chip judges an acknowledgement by an SN state of its own, arq_sn (it changes with TXTOG at
     * each one), not by the slot's bit 2, which stays the SN on the air; nothing resets it at a CONNECT_IND, so a test
     * sets arq_sn to what a last connection left (the second connection at e3d2a74). tx_latch_us (0: at T_IFS): the
     * hardware reads the TX slot it sends this many us after the packet's end, so a response put in later is not sent */
    int own_sn;
    uint8_t arq_sn;
    uint32_t tx_latch_us;
    int latch_pending;
    uint64_t latch_at;
    uint8_t latched[260];
    int latched_n;
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
