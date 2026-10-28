/* SPDX-License-Identifier: GPL-3.0-only
 * The Bluetooth test run (docs/BLUETOOTH.md "Cordio path as built" step 5): the steps a diagnostic build runs to find
 * out how far the stack gets on an FM-1, one result and its raw evidence per step, in the way the WiFi test page reports
 * stages 4-8. Own code. The run is a state machine the app calls from its loop: fm1ble_test_begin, then fm1ble_test_poll
 * with the time until it returns 1. The page, the USB SysEx report and tools/fm1_ble_report.py only read fm1ble_run.
 *
 * A step that fails says in `note` what the next fix needs (a register that did not read back, the HCI status, the event
 * that never came); a step that cannot run because an earlier one failed is SKIP.
 */
#ifndef OPLBOX_BLE_FM1BLE_TEST_H
#define OPLBOX_BLE_FM1BLE_TEST_H

#include <stdint.h>

/* the steps, in the order they run */
enum {
    FM1BLE_STEP_RADIO,      /* the Bluetooth side of the radio powered up: the registers read back */
    FM1BLE_STEP_BLOCK,      /* the baseband's RAM block set up and the baseband started (no interrupt storm) */
    FM1BLE_STEP_HCI,        /* the host reset the controller: Reset, Read Local Version, Read BD_ADDR, LE Read Buffer Size */
    FM1BLE_STEP_ADV_SET,    /* advertising set up and enabled: the controller accepted all four commands */
    FM1BLE_STEP_ADV_AIR,    /* the baseband ran advertising events and says it sent: TXTOG changed, or a request came in */
    FM1BLE_STEP_CONNECT,    /* a central connected within the wait: LE Connection Complete, the interval, the MTU */
    FM1BLE_STEP_MIDI_IN,    /* a BLE-MIDI packet written by the central reached the app's MIDI input */
    FM1BLE_STEP_MIDI_OUT,   /* a notification went out and the controller handed its buffer back */
    FM1BLE_STEPS
};

enum { FM1BLE_NOT_RUN, FM1BLE_PASS, FM1BLE_FAIL, FM1BLE_SKIP };

#define FM1BLE_RAW      48      /* bytes of raw evidence kept per step */
#define FM1BLE_NOTE     48      /* the step's one-line result, ASCII, NUL-terminated */

typedef struct {
    uint8_t result;             /* FM1BLE_NOT_RUN, PASS, FAIL or SKIP */
    uint8_t nraw;               /* bytes used in raw */
    uint8_t raw[FM1BLE_RAW];    /* the step's evidence as bytes: register readbacks (u32 little-endian) or HCI event bytes */
    char note[FM1BLE_NOTE];
    uint32_t ms;                /* milliseconds the step took, from the run's start to its end */
} fm1ble_step;

#define FM1BLE_N_CTR    248     /* counters behind the steps; names in fm1ble_ctr_name, FM1BLE_CTR_NAMES */

/* The counters, in the report's order. tools/fm1_ble_report.py has the same list (tests/ble_report_tool.py holds it to
 * FM1BLE_CTR_NAMES); the firmware at 48ea034 and before sent 24 others (the tool reads those by their count), up to
 * 122afe3 the first 32 of these, with pal_done_37-39 counting the driver's one-channel events that ended, up to 3b39ab4
 * the first 40, up to 58cadc5 the first 64, up to a7c1738 the first 80, up to 5474159 the first 120 (the event log then
 * the first 12 events, the setup in bits 7-6 alone). The RX trials' four each (rxtN_*) and the receive
 * setup the stack ran on follow, then the connection events' (pal_fm1.h: the busy wait, the window setups; fm1ble.h's
 * connection words), then the fourth setup's, the CONNECT_IND's poll gap and the PAL's event log (evN_a/b/c); then the
 * fifth to seventh setups', the connections' summaries (connN_a/b/c), NimBLE's connects; up to f0e6d08 the window
 * probe's, since then the TX side's (below); up to a349877 the first 192, since then the header trace's and the link
 * layer's after them (below). */
enum {
    FM1BLE_C_RADIO_REGS_OK, FM1BLE_C_RADIO_REGS_BAD, FM1BLE_C_VARIANT, FM1BLE_C_RF_COMMON,
    FM1BLE_C_EVENTS, FM1BLE_C_TX_FLIPS, FM1BLE_C_RX_PACKETS, FM1BLE_C_RX_SCAN_REQ,
    FM1BLE_C_RX_CONN_IND, FM1BLE_C_RX_OTHER, FM1BLE_C_PENDING_LAST, FM1BLE_C_BB_ERR,
    FM1BLE_C_AIR_TRIALS, FM1BLE_C_TXTOG, FM1BLE_C_REARM_USED, FM1BLE_C_ADV_ERR,
    FM1BLE_C_RX_HDR, FM1BLE_C_RX_STAT, FM1BLE_C_RX_TYPE, FM1BLE_C_RX_LEN,
    FM1BLE_C_PAL_ADV_OPS, FM1BLE_C_PAL_CONN_OPS, FM1BLE_C_PAL_RX_OK, FM1BLE_C_PAL_RX_TIMEOUT,
    FM1BLE_C_PAL_RESP_LATE, FM1BLE_C_PAL_STUCK, FM1BLE_C_PAL_TIMER_LATE_US, FM1BLE_C_PAL_DONE_37,
    FM1BLE_C_PAL_DONE_38, FM1BLE_C_PAL_DONE_39, FM1BLE_C_TXPWR, FM1BLE_C_RAM_END,
    FM1BLE_C_PAL_ADV_EVENTS, FM1BLE_C_PAL_ADV_COVERED, FM1BLE_C_PAL_ADV_ENDED, FM1BLE_C_PAL_ADV_CONN_IND,
    FM1BLE_C_PAL_CONN_ENDED, FM1BLE_C_PAL_CONN_RX, FM1BLE_C_PAL_REFUSED, FM1BLE_C_EVTCOUNT,
    FM1BLE_C_RXT,                                   /* RX trial k's four: FM1BLE_C_RXT + 4k + FM1BLE_RXT_* */
    FM1BLE_C_RX_STACK_TRIAL = FM1BLE_C_RXT + 16, FM1BLE_C_RX_AGC_TABLE, FM1BLE_C_RX_WIN, FM1BLE_C_AGC_CTL,
    FM1BLE_C_AGC_RSSI, FM1BLE_C_CONN_WIN, FM1BLE_C_PAL_CONN_TIMEOUT_US, FM1BLE_C_RX_STAT1,
    FM1BLE_C_PAL_BUSY_WAITS, FM1BLE_C_PAL_BUSY_US_MAX, FM1BLE_C_PAL_RESP_US_MAX, FM1BLE_C_PAL_CONN_RX_BAD,
    FM1BLE_C_PAL_CONN_SETUP, FM1BLE_C_PAL_SETUP_MOVES, FM1BLE_C_PAL_LATE_OPS, FM1BLE_C_CONN_FLIPS,
    FM1BLE_C_PAL_SET_COUNTS,                        /* setup k's: FM1BLE_C_PAL_SET_COUNTS + k */
    FM1BLE_C_CONN_ACKS = FM1BLE_C_PAL_SET_COUNTS + 3,
    FM1BLE_C_PAL_SET_ARR,                           /* setup k's: FM1BLE_C_PAL_SET_ARR + k */
    FM1BLE_C_CONN_RX_WORDS = FM1BLE_C_PAL_SET_ARR + 3,
    FM1BLE_C_PAL_SET3_COUNTS, FM1BLE_C_PAL_SET3_ARR, FM1BLE_C_PAL_CIND_GAP_US, FM1BLE_C_PAL_LOG_N,
    FM1BLE_C_PAL_LOG,                               /* event k's word w (0-2, a-c): FM1BLE_C_PAL_LOG + 3k + w */
    FM1BLE_C_PAL_SET4 = FM1BLE_C_PAL_LOG + 36,      /* setup k's (4-6): counts at FM1BLE_C_PAL_SET4 + 2(k - 4), arr + 1 */
    FM1BLE_C_PAL_CONNS = FM1BLE_C_PAL_SET4 + 6, FM1BLE_C_PAL_GOOD,
    FM1BLE_C_CONN_SUM,                              /* connection k's word w (0-2, a-c): FM1BLE_C_CONN_SUM + 3k + w */
    FM1BLE_C_NIMBLE_CONNS = FM1BLE_C_CONN_SUM + 24,
    FM1BLE_C_PAL_STALE_RX, FM1BLE_C_PAL_HALTS, FM1BLE_C_PAL_RESP_SKIPPED, FM1BLE_C_PAL_RESP_HIST,
    FM1BLE_C_PAL_TX_COUNTS,                         /* TX mode k's (fm1ble.h FM1BLE_TX_*): FM1BLE_C_PAL_TX_COUNTS + k */
    FM1BLE_C_PAL_TX_SENT = FM1BLE_C_PAL_TX_COUNTS + 3,   /* TX mode k's: FM1BLE_C_PAL_TX_SENT + k */
    FM1BLE_C_PAL_GOOD_TX = FM1BLE_C_PAL_TX_SENT + 3, FM1BLE_C_PAL_CONN_TXS, FM1BLE_C_PAL_FIRST_RX_TX, FM1BLE_C_CONN_TXTOG,
    FM1BLE_C_CONN_TXDHDR, FM1BLE_C_CONN_TXBUF,
    FM1BLE_C_PAL_LOG_D,                             /* event k's word d: FM1BLE_C_PAL_LOG_D + k */
    FM1BLE_C_CONN_SUM_D = FM1BLE_C_PAL_LOG_D + 12,  /* connection k's word d: FM1BLE_C_CONN_SUM_D + k */
    FM1BLE_C_PAL_CFG = FM1BLE_C_CONN_SUM_D + 8, FM1BLE_C_PAL_TX_ORDER, FM1BLE_C_PAL_RESP_US_MIN,
    FM1BLE_C_TRACE,                                 /* connection c's trace word n: FM1BLE_C_TRACE + 20c + n */
    FM1BLE_C_CONN_OPS = FM1BLE_C_TRACE + 40,        /* connection k's (the first 4): FM1BLE_C_CONN_OPS + k */
    FM1BLE_C_CONN_LL = FM1BLE_C_CONN_OPS + 4,       /* connection k's (the first 4): FM1BLE_C_CONN_LL + k */
    FM1BLE_C_PAL_TX3_COUNTS = FM1BLE_C_CONN_LL + 4, FM1BLE_C_PAL_TX3_SENT, FM1BLE_C_PAL_ARQ_DUP, FM1BLE_C_PAL_ARQ_UNWRITTEN,
    FM1BLE_C_STACK_ACL, FM1BLE_C_STACK_NOCP, FM1BLE_C_NIMBLE_MTU, FM1BLE_C_PAL_RXTOG
};
/* Since round 4 (the hardware's ARQ, fm1ble.h FM1BLE_TX_ARQ): trC_N the PAL's header trace (pal_fm1.h PAL_FM1_TRACE_*),
 * connection C's N-th connection event with a packet (0: none); connK_ops and connK_ll the first four connections'
 * link-layer counts (pal_fm1.h ll_ops, ll_cnt); pal_tx3_counts and pal_tx3_sent the fourth TX mode's (ARQ) as pal_txK_*; pal_arq_dup and
 * pal_arq_unwritten pal_fm1.h's arq_dup and arq_unwritten (15-0, each at most 65535) and, since round 6 (0 in an earlier
 * report: not counted), later_rx and later_given (31-16): the packets after the first in a connection event, the ones
 * given to Cordio; stack_acl the HCI ACL packets the host sent the controller
 * (15-0) and the controller the host (31-16), each at most 65535 (did an ATT PDU reach NimBLE, did NimBLE answer);
 * stack_nocp the Number Of Completed Packets events; nimble_mtu NimBLE's ATT MTU (15-0) and the central's writes (31-16);
 * pal_rxtog RXTOG (15-0) and TXTOG (31-16) as the run's last poll read them (the connection's end). 248 in all: the
 * report's count is a byte. */
/* the fifth to seventh setups' counters (pal_set4-6_counts/_arr) as the first four's; pal_conns the connections the PAL
 * started, pal_good the setup a connection received PAL_FM1_GOOD packets on (0xFF: none); connN_a/b/c the PAL's summary
 * of connection N (pal_fm1.h conn_sum); nimble_conns NimBLE's connects (bits 15-0) and disconnects (31-16).
 * The TX side (pal_fm1.h, fm1ble.h FM1BLE_TX_*), in place of f0e6d08's window probe (its 39 counters; the probe settled
 * that only PADW's window follows the register): pal_stale_rx the stale packets dropped; pal_halts the events halted
 * (15-0) and the halts the hardware stayed busy through (31-16); pal_resp_skipped INTIME's responses not put in;
 * pal_resp_hist Cordio's response latency in four 8-bit buckets (under 50, 50-99, 100-149, 150 us and more);
 * pal_txK_counts TX mode K's events (15-0) and packets (31-16); pal_txK_sent its events in which TXTOG changed (15-0) and
 * packets with RXSTAT bit 8 (31-16); pal_good_tx the mode a connection received PAL_FM1_GOOD packets on (0xFF: none);
 * pal_conn_txs the first eight connections' modes (4 bits each, the first in 3-0); pal_first_rx_tx the TX words
 * (fm1ble_op_txstate) when the first connection packet was seen, bit 31 set once it was; conn_txtog TXTOG (15-0) and RXTOG
 * (31-16), conn_txdhdr TXDHDR0 (15-0) and TXDHDR1 (31-16), conn_txbuf TXBUF0CNTL (7-0), TXBUF1CNTL (15-8) and IFSCNT
 * (31-16), as the run's last poll read them; evN_d and connN_d the event log's and the summaries' fourth words;
 * pal_cfg PAL_FM1_TIFS_US (15-0) and PAL_FM1_STALE_US (31-16); pal_tx_order PAL_FM1_TX_ORDER (4 bits each, the first in
 * 3-0); pal_resp_us_min the least response latency (0xFFFF: none) */
/* the connection events' counters: pal_setN_counts = setup N's events (bits 15-0) and packets received (31-16), each at
 * most 65535; pal_setN_arr = the earliest (bits 15-0) and latest (31-16) packet start after the window's start, us, as
 * signed 16-bit (0 with no packet); conn_rx_words = the last connection packet's RXSTAT (15-0, bit 8 "acknowledges ours")
 * and RXDHDR (31-16); pal_set3_* the same for the fourth setup (FM1BLE_CONN_WIDE); pal_cind_gap_us pal_fm1.h's cind_gap_us;
 * pal_log_n the connection events logged and evN_a, evN_b, evN_c event N's three words (pal_fm1.h PAL_FM1_LOG) */
/* an RX trial's counters: counts = events (bits 7-0), TXTOG changes (15-8), packets received (23-16), SCAN_REQs (31-24),
 * each at most 255; stat = the first packet's RXSTAT (15-0) and RXAHDR (31-16), or with none the words RXSTAT0 and RXAHDR0
 * as the trial left them; pkt = that packet's RXDHDR (15-0), the RX buffer it was in (23-16) and LASTCHMAP's low byte
 * (31-24); bytes = its payload's first four bytes, the first in bits 7-0 */
enum { FM1BLE_RXT_COUNTS, FM1BLE_RXT_STAT, FM1BLE_RXT_PKT, FM1BLE_RXT_BYTES };
#define FM1BLE_CTR_NAMES \
    "radio_regs_ok", "radio_regs_bad", "variant", "rf_common", \
    "events", "tx_flips", "rx_packets", "rx_scan_req", \
    "rx_conn_ind", "rx_other", "pending_last", "bb_err", \
    "air_trials", "txtog", "rearm_used", "adv_err", \
    "rx_hdr", "rx_stat", "rx_type", "rx_len", \
    "pal_adv_ops", "pal_conn_ops", "pal_rx_ok", "pal_rx_timeout", \
    "pal_resp_late", "pal_stuck", "pal_timer_late_us", "pal_done_37", \
    "pal_done_38", "pal_done_39", "txpwr", "ram_end", \
    "pal_adv_events", "pal_adv_covered", "pal_adv_ended", "pal_adv_conn_ind", \
    "pal_conn_ended", "pal_conn_rx", "pal_refused", "evtcount", \
    "rxt0_counts", "rxt0_stat", "rxt0_pkt", "rxt0_bytes", \
    "rxt1_counts", "rxt1_stat", "rxt1_pkt", "rxt1_bytes", \
    "rxt2_counts", "rxt2_stat", "rxt2_pkt", "rxt2_bytes", \
    "rxt3_counts", "rxt3_stat", "rxt3_pkt", "rxt3_bytes", \
    "rx_stack_trial", "rx_agc_table", "rx_win", "agc_ctl", \
    "agc_rssi", "conn_win", "pal_conn_timeout_us", "rx_stat1", \
    "pal_busy_waits", "pal_busy_us_max", "pal_resp_us_max", "pal_conn_rx_bad", \
    "pal_conn_setup", "pal_setup_moves", "pal_late_ops", "conn_flips", \
    "pal_set0_counts", "pal_set1_counts", "pal_set2_counts", "conn_acks", \
    "pal_set0_arr", "pal_set1_arr", "pal_set2_arr", "conn_rx_words", \
    "pal_set3_counts", "pal_set3_arr", "pal_cind_gap_us", "pal_log_n", \
    "ev0_a", "ev0_b", "ev0_c", "ev1_a", \
    "ev1_b", "ev1_c", "ev2_a", "ev2_b", \
    "ev2_c", "ev3_a", "ev3_b", "ev3_c", \
    "ev4_a", "ev4_b", "ev4_c", "ev5_a", \
    "ev5_b", "ev5_c", "ev6_a", "ev6_b", \
    "ev6_c", "ev7_a", "ev7_b", "ev7_c", \
    "ev8_a", "ev8_b", "ev8_c", "ev9_a", \
    "ev9_b", "ev9_c", "ev10_a", "ev10_b", \
    "ev10_c", "ev11_a", "ev11_b", "ev11_c", \
    "pal_set4_counts", "pal_set4_arr", "pal_set5_counts", "pal_set5_arr", \
    "pal_set6_counts", "pal_set6_arr", "pal_conns", "pal_good", \
    "conn0_a", "conn0_b", "conn0_c", "conn1_a", \
    "conn1_b", "conn1_c", "conn2_a", "conn2_b", \
    "conn2_c", "conn3_a", "conn3_b", "conn3_c", \
    "conn4_a", "conn4_b", "conn4_c", "conn5_a", \
    "conn5_b", "conn5_c", "conn6_a", "conn6_b", \
    "conn6_c", "conn7_a", "conn7_b", "conn7_c", \
    "nimble_conns", "pal_stale_rx", "pal_halts", "pal_resp_skipped", \
    "pal_resp_hist", "pal_tx0_counts", "pal_tx1_counts", "pal_tx2_counts", \
    "pal_tx0_sent", "pal_tx1_sent", "pal_tx2_sent", "pal_good_tx", \
    "pal_conn_txs", "pal_first_rx_tx", "conn_txtog", "conn_txdhdr", \
    "conn_txbuf", "ev0_d", "ev1_d", "ev2_d", \
    "ev3_d", "ev4_d", "ev5_d", "ev6_d", \
    "ev7_d", "ev8_d", "ev9_d", "ev10_d", \
    "ev11_d", "conn0_d", "conn1_d", "conn2_d", \
    "conn3_d", "conn4_d", "conn5_d", "conn6_d", \
    "conn7_d", "pal_cfg", "pal_tx_order", "pal_resp_us_min", \
    "tr0_0", "tr0_1", "tr0_2", "tr0_3", \
    "tr0_4", "tr0_5", "tr0_6", "tr0_7", \
    "tr0_8", "tr0_9", "tr0_10", "tr0_11", \
    "tr0_12", "tr0_13", "tr0_14", "tr0_15", \
    "tr0_16", "tr0_17", "tr0_18", "tr0_19", \
    "tr1_0", "tr1_1", "tr1_2", "tr1_3", \
    "tr1_4", "tr1_5", "tr1_6", "tr1_7", \
    "tr1_8", "tr1_9", "tr1_10", "tr1_11", \
    "tr1_12", "tr1_13", "tr1_14", "tr1_15", \
    "tr1_16", "tr1_17", "tr1_18", "tr1_19", \
    "conn0_ops", "conn1_ops", "conn2_ops", "conn3_ops", \
    "conn0_ll", "conn1_ll", "conn2_ll", "conn3_ll", \
    "pal_tx3_counts", "pal_tx3_sent", "pal_arq_dup", "pal_arq_unwritten", \
    "stack_acl", "stack_nocp", "nimble_mtu", "pal_rxtog"

/* The air trials (adv_air): the driver's own ADV_IND, each trial from its own static random address, so a central's
 * scan (tools/ble_central.py) says which one reached the air. Trial 0: without the shared radio's init (variant 0), at
 * FM1BLE_TXPWR_LEVEL; trial 1: with it (variant 1), the same level; trial 2: variant 1 at FM1BLE_TXPWR_MAX. The address
 * is C2:46:4D:31:00:<FM1BLE_TRIAL_ADDR0 + trial>; the stack (Cordio, NimBLE) advertises from C2:46:4D:31:00:01. */
#define FM1BLE_TRIALS       3
#define FM1BLE_TRIAL_MS     3000u
#define FM1BLE_TRIAL_ADDR0  0x10u

/* The RX trials, after the air trials and before the stack: the same ADV_IND on variant 1 at FM1BLE_TXPWR_LEVEL, each from
 * C2:46:4D:31:00:<FM1BLE_RX_ADDR0 + k> with its own receive setup (fm1ble_rx_setup), so a central's active scan (its
 * SCAN_REQs) shows in each trial's counters which setup lets the radio receive after its own TX. Up to 3b39ab4 the driver
 * wrote neither the AGC block's gain table nor WINCNTL2, and the air trials received nothing (the trials above still run
 * that way, to compare):
 *   0 (:13): no gain table, WINCNTL2 = 0x32 (le_hw_advertising's)
 *   1 (:14): the gain table (the radio powered up again with it), WINCNTL2 = 0
 *   2 (:15): the gain table, WINCNTL2 = 0x32: JieLi's setup, the driver's default
 *   3 (:16): the gain table, WINCNTL2 = 0x96 (the direct-test receiver's)
 * The stack then runs on the window of the first of trials 2, 3, 0, 1 that received a packet (2 when none did), with the
 * gain table (rx_stack_trial, rx_agc_table and rx_win say which). */
#define FM1BLE_RX_TRIALS    4
#define FM1BLE_RX_ADDR0     (FM1BLE_TRIAL_ADDR0 + FM1BLE_TRIALS)

/* A central that leaves during MIDI_IN (the connection dropped) may connect again: the step waits on while NimBLE has
 * seen fewer than FM1BLE_CONN_TRIES connects, within the step's wait. Each connection runs the PAL's next TX mode
 * (pal_fm1.h PAL_FM1_TX_ORDER; the window setup is PADW for all since f0e6d08) until one carries L2CAP, so a run whose
 * central connects again (tools/ble_central.py --tries) tries them in turn. */
#define FM1BLE_CONN_TRIES   7u

/* The PDU capture (since round 7 of the FM-1 Bluetooth goal): the PAL's (pal_fm1.h PAL_FM1_CAP, PAL_FM1_CAP_BYTES, 32 of
 * 20 bytes) and ARQ's ways, after the steps' frames in the report as one more frame, index FM1BLE_STEPS
 * (tools/fm1_ble_report.py prints it as PDU CAPTURE). Its bytes: a head of FM1BLE_CAP_HEAD,
 *   0 the layout (3; 2 up to 807d70f: bytes 0-127 only, a 128-byte head; 1 up to round 7: bytes 0-14 only, a 16-byte head); 1 the entries; 2 an entry's bytes; 3 the ARQ
 *   connections started (at most 255); 4-5 the first 16 connections that ran ARQ ahead (bit k: connection k); 6 the way
 *   kept (1 ahead, 0 in T_IFS, 0xFF none); 7 PAL_FM1_ARQ_STUCK; 8-9 and 10-11 bit 2 changed at a first packet and after
 *   PAL_FM1_ARQ_STUCK; 12-13 the responses written ahead; 14 PAL_FM1_L2_GOOD; 15 the connection the fragment trace is of
 *   (1 + since the attach, 0: none) (u16 little-endian, each at most 65535);
 *   16-23 the ATT MTU NimBLE offered on each of the first four connections and 24-31 the one their MTU exchange settled
 *   on (u16 each, 0: none; ble_nimble.h pref_mtu, conn_mtu; since round 8);
 *   32-127 the fragment trace (pal_fm1.h PAL_FM1_FRAG): 12 packets of two u32 words, 0 for none;
 *   128-139 the cross-check's six counts (pal_fm1.h sn_x, u16 each) and 140-159 its first five words (PAL_FM1_SNX, u32
 *   each, 0 for none; since round 2 of the resumed goal);
 * then the entries as pal_fm1.h lays them out. */
#define FM1BLE_CAP_HEAD     160u
#define FM1BLE_CAP_MAX      (FM1BLE_CAP_HEAD + 32u * 20u)

enum { FM1BLE_IDLE, FM1BLE_RUNNING, FM1BLE_DONE };

typedef struct {
    uint8_t state;              /* FM1BLE_IDLE, RUNNING, DONE */
    uint8_t cur;                /* the step now running */
    uint8_t bdaddr[6];          /* the controller's public address, most significant byte first (valid after HCI) */
    uint32_t t0, now;           /* the clock at begin and at the last poll, ms */
    fm1ble_step step[FM1BLE_STEPS];
    uint32_t ctr[FM1BLE_N_CTR]; /* see fm1ble_ctr_name */
    uint16_t ncap;              /* the PDU capture's bytes (FM1BLE_CAP_MAX above) */
    uint8_t cap[FM1BLE_CAP_MAX];
} fm1ble_run;

/* Starts a run (clears r). The central has `wait_ms` to connect once advertising is on; 0: skip the connect and MIDI steps. */
void fm1ble_test_begin(fm1ble_run *r, uint32_t now_ms, uint32_t wait_ms);

/* One step of the loop: 1 when the run is DONE. Never blocks for more than a few milliseconds, except where the radio's
 * own start-up must (the page shows that: audio is muted around it, as for WiFi). */
int fm1ble_test_poll(fm1ble_run *r, uint32_t now_ms);

/* Names for the report: lowercase words, no spaces. NULL past the last. */
const char *fm1ble_step_name(int step);
const char *fm1ble_ctr_name(int ctr);

#endif
