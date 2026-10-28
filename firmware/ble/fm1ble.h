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
    uint8_t rx_last_buf;        /* the RX buffer (0 or 1) the last packet was read from */
    uint16_t rx_last_ahdr, rx_last_dhdr;   /* its RXAHDR and RXDHDR words */
    uint8_t agc_table;          /* 1 when the last power-up wrote the AGC gain table (fm1ble_rx_setup) */
    uint32_t pending_last;      /* the last value of the pending register */
    uint32_t tx_flips;          /* events after which TXTOG had changed: the hardware's record that it sent a buffer (E.4) */
    uint16_t txtog;             /* TXTOG as last read */
    uint32_t ram_end;           /* 0x2FCBC as read back after fm1ble_bb_init (JieLi writes it and never reads it: D.1) */
    uint32_t op_adv_done;       /* fm1ble_op_adv events (37, 38 and 39) the baseband ended */
    uint32_t op_conn_done;      /* fm1ble_op_conn events the baseband ended */
    uint32_t conn_flips;        /* fm1ble_op_conn events after which TXTOG had changed (a buffer sent, E.4) */
    uint32_t conn_acks;         /* packets received in them with RXSTAT bit 8 ("acknowledges ours", E.5 [unchecked]) */
    uint16_t conn_stat, conn_dhdr;   /* the last one's RXSTAT and RXDHDR (NESN bit 2, SN bit 3, MD bit 4 [unchecked]) */
    uint32_t halts, halt_busy;  /* fm1ble_op_halt calls; ones whose wait for idle ran out (the baseband stayed busy) */
} fm1ble_diag;

/* The driver's state. The caller owns it and the RAM block (word aligned, at least FM1BLE_BLOCK_SIZE bytes; the whole
 * block must be below 64 KB of its start, which the size guarantees). */
#define FM1BLE_BLOCK_SIZE   2304u
/* The transmit power level (E.7: TXPWR = 0x10 + level): JieLi's SDK sets 6 unless the configuration store says otherwise,
 * and documents 0-11 (docs/BLUETOOTH_RADIO.md "The TX power level"). Level 0 is the lowest. */
#define FM1BLE_TXPWR_LEVEL  6u
#define FM1BLE_TXPWR_MAX    11u
/* The receive windows (docs/BLUETOOTH_RADIO.md E.2 WINCNTL2): every JieLi path writes WINCNTL0 = 0x32 and puts the time
 * the receiver listens into WINCNTL2: 0x32 when advertising (le_hw_advertising), the scan window when scanning, the
 * first transmit window of a connection; the direct-test receiver writes 0x96. Taken as microseconds from those uses. */
#define FM1BLE_WIN0         0x32u
#define FM1BLE_ADV_WIN_US   0x32u
typedef struct {
    uint8_t *blk;               /* the RAM block */
    uint32_t blk_bus;           /* its address as the baseband takes it */
    uint16_t entity;            /* offset of entity 0 in the block */
    uint16_t rx[2], tx[2];      /* offsets of the receive and transmit buffers */
    uint8_t on, link_open, adv_on;
    uint8_t rearm, hop;
    uint8_t op_on, op_chan, op_state;   /* fm1ble_op_*: an operation armed, its channel, its state code (2 or 7) */
    uint8_t op_slot;            /* the TX slot (TXTOG & 1 when armed) the advertising PDU went into; the response takes the other */
    uint8_t txpwr;              /* the transmit power level in both TXPWR words */
    uint8_t adv_type;           /* fm1ble_adv_start's PDU type, for re-pointing the buffers after each event */
    uint16_t slots;             /* the interval in slots, for re-arming */
    uint8_t conn_setup;         /* fm1ble_op_conn_setup: how fm1ble_op_conn places its receive window (FM1BLE_CONN_*) */
    uint8_t conn_tx;            /* fm1ble_op_conn_begin: how a connection's TX slots are written (FM1BLE_TX_*) */
    uint8_t tx_sn;              /* the SN of the last data PDU put in (FM1BLE_TX_INTIME's empty PDU repeats it) */
    uint8_t arq_ahead;          /* fm1ble_op_arq_ahead: ARQ's response goes into the slot sent next, not the one sent now */
    uint8_t ahead_pend;         /* with it: the slot (1 + its index, 0: none) a response went into that TXTOG has not named yet */
    uint8_t rx_set, rx_agc;     /* fm1ble_rx_setup was called; with it, whether the power-up writes the AGC gain table */
    uint16_t rx_win;            /* with it, WINCNTL2 for advertising */
    uint32_t ana_pll[243];      /* the PLL table (F.2): 81 entries of 3 words, the analog block reads it */
    fm1ble_cal cal;
    fm1ble_diag d;
} fm1ble;

/* The Bluetooth side of the radio: B.2, B.1, the analog and modem block (C.1) and the AGC block (C.3), with delays.
 * variant 1 first calls fm1ble_port_rf_common (docs/BLUETOOTH_RADIO.md "not settled" 1: which is needed is a
 * measurement). Fills b->d.rb. A step whose readback differs is counted, not an error: the baseband may change a bit
 * that was written. Returns FM1BLE_OK. */
int fm1ble_radio_up(fm1ble *b, int variant, const fm1ble_cal *cal);

/* The receive setup, for the test run's RX trials (fm1ble_test.h): whether fm1ble_radio_up writes the AGC block's gain
 * table (C.3 btagc_ram_init, fm1ble_agc.h: the receiver heard nothing on the FM-1 without it and without WINCNTL2,
 * oplbox-ble-run-3b39ab4), and the WINCNTL2 that fm1ble_adv_start and fm1ble_op_adv write (0: none, as before). Without a
 * call: the table and FM1BLE_ADV_WIN_US, JieLi's setup. The table takes effect at the next fm1ble_radio_up, the window at
 * the next advertising start. */
void fm1ble_rx_setup(fm1ble *b, int agc_table, uint32_t adv_win_us);

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
 * requests against) goes into the TX slot TXTOG & 1 names (the one the hardware sends next); the other slot holds rsp,
 * the scan response the hardware answers this event's SCAN_REQs with (TX buffer 1), or the PDU again when rsp is NULL or
 * nrsp 0 and for ADV_NONCONN_IND. Each call takes the scan response as it is now: nothing is kept from an earlier event.
 * A channel must be set (37-39: which one does not matter, the event covers all three). FM1BLE_ESTATE before
 * fm1ble_bb_init or while fm1ble_adv_start's advertising runs, FM1BLE_EARG for a data channel or a PDU or scan response
 * under 8 or over 39 bytes. */
int fm1ble_op_adv(fm1ble *b, uint32_t delay_us, const uint8_t *pdu, uint16_t n, const uint8_t *rsp, uint16_t nrsp);
/* A connection event as the peripheral on the channel set (0-36): listen from delay_us from now for window_us
 * (WINCNTL2, at most 65535; WINCNTL0 = FM1BLE_WIN0, as le_hw_ioctrl's __set_winsize writes them), the window placed as
 * fm1ble_op_conn_setup says. */
int fm1ble_op_conn(fm1ble *b, uint32_t delay_us, uint32_t window_us);
/* How fm1ble_op_conn places the window. Which one the chip needs is a measurement: the PAL's connection trials
 * (pal_fm1.h) try them on the FM-1. WIDEN0/WIDEN1 (E.2) are the window widening JieLi's peripheral writes with __set_widen
 * (le_hw_ioctrl); the open leaves them 0 and 0x51E7 (487 us), which up to 58cadc5 this driver kept in connections.
 *   FM1BLE_CONN_KEEP   as up to 58cadc5: the counter at delay_us, WINCNTL2 = window_us, WIDEN at the open's values;
 *   FM1BLE_CONN_PAD    no widening (WIDEN0 0, WIDEN1 0x5000), the counter FM1BLE_CONN_PAD_US earlier and the window
 *                      2 x FM1BLE_CONN_PAD_US longer: a slot boundary of the baseband's own (op_arm's guess) cannot
 *                      push the window past the packet;
 *   FM1BLE_CONN_WIDEN  JieLi's shape: the counter at the window's middle (Cordio's anchor: its window is the anchor
 *                      minus the widening, twice the widening long), WIDEN = half the window (__set_widen: slots in
 *                      WIDEN0, the rest | 0x5000 in WIDEN1), WINCNTL2 = window_us;
 *   FM1BLE_CONN_WIDE   as PAD with FM1BLE_CONN_WIDE_US on each side: the counter that much earlier, the window 2 x that
 *                      longer. At a7c1738 only PAD received (1 event of 6, the packet 221 us after Cordio's window start,
 *                      in a window that was still the wide unsynchronised one), KEEP and WIDEN 0 of 5: so op_arm's counter
 *                      may open the window a slot late (JieLi's __set_anchor_cnt writes the slots minus one, op_arm the
 *                      slots) or at a slot boundary of the baseband's own, either of which a 625 us pad only just covers
 *                      and Cordio's later windows (2 x the widening: 286 us after 14 missed events at a7c1738, tens of
 *                      us once in step) do not. 1250 us covers both, and the PAL's event log (pal_fm1.h) says where in
 *                      it the packet lands and when the hardware ended each event.
 * At 5474159 (oplbox-ble-run-5474159-1) PAD received 2 of 9 events and WIDE 2 of 10, KEEP and WIDEN none: the central's
 * packet came 31 us after dueUsec in one PAD event and was missed in three others where PAD's window should have held it,
 * and WIDE's windows ended 262-846 us after dueUsec although WINCNTL2 asked for 5 ms. The next three each take one fact
 * of the IR the setups above leave out, on PAD's shape, so a connection on each says which the chip needs:
 *   FM1BLE_CONN_PADJ   as PAD, the counter written as __set_anchor_cnt writes it: the slots minus one (0 for 0), the bit
 *                      offset (index 7) before it. op_arm writes the slots themselves: if the chip counts one more, as
 *                      JieLi's write says, every window above opened a slot late (PAD's at dueUsec, the packet's edge);
 *   FM1BLE_CONN_PADB   as PADJ, the bit offset with bit 15 set, as __hw_set_widen_drift writes index 7 in a connection
 *                      (`last_bitoff | 0x8000`): without it the chip may ignore the offset and open at a slot boundary;
 *   FM1BLE_CONN_PADW   as PADB, the window also in WINCNTL0/1, where __hw_set_widen_drift puts each connection event's
 *                      window (WINCNTL2 holds le_hw_ioctrl's transmit window): every setup above leaves WINCNTL0 0x32. */
#define FM1BLE_CONN_KEEP    0u
#define FM1BLE_CONN_PAD     1u
#define FM1BLE_CONN_WIDEN   2u
#define FM1BLE_CONN_WIDE    3u
#define FM1BLE_CONN_PADJ    4u
#define FM1BLE_CONN_PADB    5u
#define FM1BLE_CONN_PADW    6u
#define FM1BLE_CONN_SETUPS  7u
#define FM1BLE_CONN_PAD_US  625u
#define FM1BLE_CONN_WIDE_US 1250u
void fm1ble_op_conn_setup(fm1ble *b, uint8_t setup);
/* How a connection's TX slots are written. The IR (tools/ble_ir.py --steps RF_ble __hw_rx_buf_process ble_hw_tx
 * __hw_tx_process, read 2026-10-28) says three things the driver up to f0e6d08 did not do: on a CONNECT_IND JieLi writes
 * TXAHDR0/1 = 0 (the ChSel bit, 0 for algorithm 1), TXDHDR0 = (TXTOG & 1) << 2 | 1 and TXDHDR1 = ((TXTOG & 1) << 2 ^ 4) | 1
 * (two empty PDUs whose bit 2 differs, the one sent first with bit 2 clear); ble_hw_tx keeps TXDHDR bit 2 as it is
 * (`old & 4`) and puts header bit 9 in bit 3, the bit its receive path fills from RXDHDR bit 3 (SN on the air), and a
 * flag in bit 4; and after every packet received __hw_tx_process reads TXTOG bit 0 and TXBUF0CNTL/TXBUF1CNTL bit 0 and
 * refills the TX buffers from its queue (ahead of the event: the slot sent at T_IFS is already filled when the packet
 * comes). So the hardware keeps bit 2 per slot (NESN, or the slot's own sequence bit) and picks the slot by TXTOG, and
 * up to f0e6d08 the driver wrote Cordio's NESN over bit 2 in both slots, and a response after the packet. At f0e6d08
 * (oplbox-ble-run-f0e6d08-1) the one connection that received past its first event received its second event's packet,
 * Cordio's response came (up to 251 us after a packet, T_IFS is 150), and the hardware neither sent (TXTOG unchanged) nor
 * ended the event. The modes, one per connection (pal_fm1.h PAL_FM1_TX_ORDER):
 *   FM1BLE_TX_AIR      as up to f0e6d08: the response in both slots with TXDHDR = length << 8 | the header byte's bits
 *                      4-0 as on the air (LLID, NESN, SN, MD), an empty PDU (LLID 1) in both at every event's arm;
 *   FM1BLE_TX_JIELI    as JieLi: the connection's first event writes the CONNECT_IND words above; a response goes in
 *                      both slots as ble_hw_tx writes it: TXDHDR = length << 8 | (TXDHDR & 4) | SN << 3 | MD << 4 | LLID,
 *                      bit 2 left to the hardware; nothing is written at the arm, so a response too late for T_IFS leaves
 *                      the last one (the same SN: a central takes it as a repeat);
 *   FM1BLE_TX_INTIME   as JIELI, an empty PDU (LLID 1, the last SN again) in both slots at every arm, and the PAL puts a
 *                      response in only while T_IFS can still use it (PAL_FM1_TIFS_US after the packet was seen): no
 *                      write while the hardware may be turning around or sending.
 * At a349877 (the done-check after round 3) INTIME received 462 packets in two connections that stayed up 6.6 and 10 s,
 * and the central never resolved GATT. The IR says why the three modes above cannot work: __hw_tx_buf_proess sets header
 * bit 9 when JieLi's TX queue is not empty (`hw_dma_empty() eq 0`, shifted to 9), that is MD, and ble_hw_tx puts header
 * bit 9 in TXDHDR bit 3. So TXDHDR is the chip's layout, not the air's: bits 1-0 LLID, bit 2 the slot's own (set apart at
 * the CONNECT_IND, clear in the slot sent first: its SN), bit 3 MD, bit 4 a per-link flag; and the receive path builds its
 * header word from RXDHDR the same way (bit 3 to word bit 9, MD). SN and NESN are the hardware's: it sends the slot TXTOG
 * names until the central acknowledges it, then the other [unchecked: from the slot words, not measured]. AIR, JIELI and
 * INTIME put Cordio's SN in MD; and the PAL handed Cordio RXDHDR bits 4-2 as NESN, SN, MD, so Cordio read the central's
 * MD (0) as its SN, took the first packet (LL_FEATURE_REQ) and every later one as a repeat, and dropped them.
 *   FM1BLE_TX_ARQ      the hardware's ARQ: a response goes only into the slot TXTOG & 1 names (each slot its own buffer),
 *                      TXDHDR = length << 8 | bit 2 kept | LLID, MD 0 (Cordio's PDUs one an event); at
 *                      every arm the other slot gets an empty PDU, so an acknowledgement never sends an older PDU again.
 *                      The PAL gives Cordio SN and NESN of its own making (pal_fm1.c "ARQ"). */
#define FM1BLE_TX_AIR       0u
#define FM1BLE_TX_JIELI     1u
#define FM1BLE_TX_INTIME    2u
#define FM1BLE_TX_ARQ       3u
#define FM1BLE_TX_MODES     4u
/* A connection's first event: its TX mode, and for JIELI, INTIME and ARQ the CONNECT_IND words (above). ARQ ahead off. */
void fm1ble_op_conn_begin(fm1ble *b, uint8_t tx_mode);
/* ARQ written ahead (since round 7 of the FM-1 Bluetooth goal): a response goes into the slot the hardware sends after
 * the central's next acknowledgement (the one TXTOG & 1 does not name), as JieLi fills its slots after a packet for the
 * next one, and the arm leaves that slot alone while it holds a response TXTOG has not named yet. Without it (ARQ as up
 * to e3d2a74) the response goes into the slot TXTOG names, which the hardware sends T_IFS after the packet: a write the
 * hardware has already read past is not sent, and the next acknowledgement moves TXTOG on as if it had been. */
void fm1ble_op_arq_ahead(fm1ble *b, int on);
/* ARQ: bit 2 of TXDHDR0 and TXDHDR1 changed (each slot's SN). For a chip whose own SN state (which an acknowledgement is
 * judged by) and the slots' bit 2 (the SN on the air) disagree: the central then takes every PDU as a repeat. */
void fm1ble_op_arq_flip(fm1ble *b);
/* The packet the hardware sends after T_IFS: TX buffer 1 in an advertising event (for the rest of this event; the next
 * fm1ble_op_adv puts in the scan response it is given), buffer 0 in both TX slots in a connection event (ARQ: the slot
 * TXTOG & 1 names, from its own buffer), written as the connection's TX mode says. At most 37 bytes of payload (the buffers' size, E.4). */
int fm1ble_op_resp(fm1ble *b, const uint8_t *pdu, uint16_t n);
/* The TX side's words as they are now, for the PAL's log: TXDHDR0 bits 7-0, TXDHDR1 bits 15-8, TXTOG bit 0 in 16, RXTOG
 * bit 0 in 17, TXBUF0CNTL bit 0 in 18, TXBUF1CNTL bit 0 in 19. 0 before fm1ble_bb_init. */
uint32_t fm1ble_op_txstate(fm1ble *b);
#define FM1BLE_TXS_TXTOG    (1u << 16)
#define FM1BLE_TXS_RXTOG    (1u << 17)
#define FM1BLE_TXS_BUF0     (1u << 18)
#define FM1BLE_TXS_BUF1     (1u << 19)
/* The event off: anchor counter stopped, the link's interrupts off. No wait (it may run from an interrupt). */
void fm1ble_op_stop(fm1ble *b);
/* An event the baseband never ended: fm1ble_op_stop, then the wait for the hardware to go idle that fm1ble_adv_stop
 * (ble_hw_disable) does, so it cannot finish into the next event (at f0e6d08 such an event's packet flag came 364 us
 * after the next event was armed, 1192 us before that event's dueUsec, and Cordio took it as the anchor). Main loop only.
 * FM1BLE_ETIMEOUT when the hardware stayed busy (counted in d.halt_busy). */
int fm1ble_op_halt(fm1ble *b);
/* Reads and clears the pending flags of the operation in flight: FM1BLE_EV_DONE and FM1BLE_EV_RX, with the packet in
 * *p for RX. 0 when nothing is pending. Polled, or called from IRQ 29 / 45 (D.4). */
uint32_t fm1ble_op_service(fm1ble *b, fm1ble_pkt *p);

/* A register of the anchor file (D.2) as the chip reads it back: 0x2801C = index << 10 | hw id << 4 | 2, then 0x28024.
 * JieLi's __get_clkn reads indices 0 and 0xE (the clock) and __hw_set_widen_drift index 7; what they hold when read is
 * [measure on FM-1] (the test run's window probe records them). 0 before fm1ble_bb_init. */
uint32_t fm1ble_anchor_rd(fm1ble *b, uint32_t index);

#endif
