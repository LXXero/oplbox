/* SPDX-License-Identifier: GPL-3.0-only
 * Cordio's PAL on the FM-1's BLE baseband (PAL_NEEDED.md, docs/BLUETOOTH_CORDIO.md): the 37 functions of Cordio's six PAL
 * headers, on the radio driver's one-packet operations (fm1ble.h "for Cordio's PAL"). Own code, written from the contracts
 * in platform/include/pal_*.h and from Cordio's callers (bb_ble_adv_slave.c, bb_ble_conn_slave.c), not from any chip's PAL.
 * Never run on an FM-1.
 *
 * The translation. Cordio asks for one packet at a time with an absolute time. A peripheral's connection event
 * (PalBbBleRxData on 0-36) is one hardware event on that channel. An advertising event is three of Cordio's packets
 * (PalBbBleTxData on 37, 38 and 39, bb_ble_adv_slave.c), and the hardware's advertising event covers all three (it hops
 * by itself): the first of them arms one (fm1ble_op_adv, armed as the driver's own advertising, the only advertising a
 * central heard from the FM-1: at 122afe3 the one-channel events this PAL armed per packet were never heard, and the
 * ones on 38 and 39 never ended), and the next ones on a channel not yet asked for in it are served by it ("covered").
 * The baseband reports two things per event, a packet received and the event's end, and they become Cordio's
 * completions:
 *   advertising:  the ADV PDU went (txCback) when a request came in or the event ended; the request (rxCback) with the
 *                 packet, or a timeout at the end; the SCAN_RSP Cordio then hands over (PalBbBleTxTifsData) goes into the
 *                 response buffer for the rest of the event, and its txCback comes at the end; a covered packet
 *                 completes the same way, at once when the event has already ended; a CONNECT_IND ends the event;
 *   connection:   the master's packet (rxCback), or a timeout at the end; the response goes into the response buffer
 *                 and its txCback comes at the end; a second receive in the same event times out (one exchange per event:
 *                 the hardware's own behaviour after the response is [measure on FM-1]).
 * The response must be in its buffer before T_IFS (150 us) after the packet's end: pal_fm1_service has to run from the
 * receive interrupt (IRQ 29) for that, and from the main loop it is too late (counted: resp_late, and Cordio is told the
 * response failed). The baseband's interrupts are not attached in this version: everything runs from the main loop.
 * So each advertising event is armed with the scan response already in its buffer: the one in Cordio's advertising
 * operation (the BOD in progress, bb_ble_adv_slave.c sends the same buffer through PalBbBleTxTifsData), read when the
 * event is armed. Cordio puts a scan response the host changed into that operation between events
 * (lctr_isr_adv_slave.c), so the next event answers with the new one; nothing older is kept. And a connection event is
 * waited through (busy wait, below).
 *
 * Time: PalBbGetCurrentTime is the port's microsecond clock (TIMER4 on the FM-1), not the baseband's slot counter; an
 * operation is armed (due - now) from now. A packet's timestamp (its start, which Cordio uses to place a connection's
 * anchor) is the time pal_fm1_service saw it minus its time on the air: right when the service runs in the interrupt,
 * late by the main loop's latency otherwise. The scheduler's timer is a deadline checked in pal_fm1_service, set
 * PAL_FM1_TIMER_LEAD_US earlier than Cordio asks (pal_fm1.h), so a connection event is armed early enough for the PAD and
 * WIDE windows to open before dueUsec.
 *
 * The busy wait. Polled from the main loop, a connection packet is timestamped as late as the loop came back, and Cordio
 * puts the next anchor that much late: at 58cadc5 the FM-1 received the first packet of a connection (in the transmit
 * window, which is wide) and 0 of the next 17, whose windows were 286 us (oplbox-ble-run-58cadc5: pal_conn_ops 18,
 * pal_conn_rx 1, pal_timer_late_us 215). So once a connection event is armed, pal_fm1_service does not return before
 * its packet or its end: it polls the baseband every BUSY_STEP_US, the timestamp is that close, and Cordio's response
 * goes in at once (resp_us_max says how long Cordio took against T_IFS). It costs the main loop the scheduler's setup
 * time and the window, each connection event (busy_us_max). Called from an interrupt (the receive flag), it does not
 * wait: the packet is there.
 *
 * The window setups. Where the driver's window lands against the packet is not settled on the chip (fm1ble.h
 * FM1BLE_CONN_*): each connection runs one of them for all its events, the k-th connection since the attach the k-th
 * enabled one of PAL_FM1_SETUP_ORDER, so a central that connects again after each drop tries them in turn, one whole
 * connection each, and the counters say which received; once a connection received PAL_FM1_GOOD packets on a setup,
 * every later connection runs that one. The first PAL_FM1_LOG_PER events of each connection are logged one by one
 * (pal_fm1.h), PAL_FM1_LOG in all: the channel, the setup, the arm delay, when the packet started and when the hardware's
 * end flag came against Cordio's dueUsec; and each of the first PAL_FM1_CONNS connections is summed up. For a missed event
 * the end says where the hardware really put its window.
 *
 * What a baseband that never answers does: each operation has a deadline (its start, the window and two packets, plus
 * 5 ms); past it the operation counts as ended, so Cordio is never left waiting (the stub PAL's hang, docs/BLUETOOTH_CORDIO.md).
 * A connection event ended that way is halted (fm1ble_op_halt), so it cannot finish into the next one.
 *
 * The TX side (since f0e6d08). Each connection also runs one of the driver's TX modes (fm1ble.h FM1BLE_TX_*), the k-th
 * connection the k-th enabled one of PAL_FM1_TX_ORDER, kept with the setup once a connection received PAL_FM1_GOOD
 * packets. A packet flag that starts PAL_FM1_STALE_US or more before the event's window could open is an earlier event's,
 * raised late (at f0e6d08 one came 1192 us before dueUsec and Cordio anchored on it): it is dropped, not given to Cordio.
 * In FM1BLE_TX_INTIME a response Cordio hands over more than PAL_FM1_TIFS_US after the packet was seen is not put in
 * (the empty PDU repeating the last SN goes) and Cordio is told the transmit failed. The log's and the summaries' fourth
 * words say what the TX side did (pal_fm1.h).
 */
#include <string.h>
#include "wsf_types.h"
#include "pal_bb.h"
#include "pal_bb_ble.h"
#include "pal_crypto.h"
#include "pal_radio.h"
#include "pal_sys.h"
#include "pal_timer.h"
#include "bb_api.h"
#include "bb_ble_api_op.h"
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "pal_fm1.h"

#define RSSI_NONE       127     /* Cordio's "not available": the RSSI register (C.3) is not read [measure on FM-1] */
#define SLACK_US        5000u
#define PDU_MAX         (2u + 255u)

#define ADV_EVENT_US    25000u  /* three channels, each a packet, a request and a response, and the hardware's random delay
                                   on events (index 8, up to 10 ms [unchecked]) */
#define ADV_SPAN_US     100000u /* a hardware advertising event serves Cordio's packets for this long after it was armed */
#define BUSY_STEP_US    2u      /* the busy wait's poll period */

enum { W_NONE, W_TX, W_RX, W_TIFS_TX };
enum { K_NONE, K_ADV, K_CONN };          /* the hardware event in flight: advertising (37-39) or a connection event */

static pal_fm1_cfg g_cfg;
static pal_fm1_stats g_st;
static uint8_t g_busy;

/* the operation in flight */
static PalBbBleTxIsr_t g_txcb;
static PalBbBleRxIsr_t g_rxcb;
static uint32_t g_due, g_rx_timeout;
static uint8_t g_chan, g_wait, g_fail, g_done, g_skip_done, g_resp_late, g_armed;
static uint32_t g_deadline;
static uint8_t g_kind;
static uint8_t *g_rxbuf;
static uint16_t g_rxlen;

/* the hardware advertising event Cordio's packets go in: armed (g_aev), the channels already asked for in it, when */
static uint8_t g_aev, g_aev_mask;
static uint32_t g_aev_t;

/* a received packet Cordio has not taken yet */
static uint8_t g_held, g_held_ok;
static uint8_t g_held_pdu[PDU_MAX];
static uint16_t g_held_n;
static uint32_t g_held_ts;

/* the scheduler's timer */
static PalTimerCompCback_t g_tmr_cb;
static uint8_t g_tmr_on;
static uint32_t g_tmr_due;

static uint32_t g_rng;

/* the connection event in flight: armed with setup g_set, its packet seen (and when), the busy wait's start */
static uint8_t g_set, g_conn_open, g_conn_rx, g_bw_on;
static uint32_t g_rx_t, g_bw_t0, g_win_early;   /* g_win_early: how much earlier than g_due the window was armed to start */
static uint32_t *g_log;                 /* the event's log entry (pal_fm1.h), NULL past PAL_FM1_LOG */
static uint32_t *g_sum;                 /* the connection's summary (pal_fm1.h), NULL past PAL_FM1_CONNS */
static uint32_t g_cev, g_cpk;           /* the connection's events and packets so far */
static uint32_t g_flips0;               /* the driver's conn_flips when the event was armed */
static uint32_t g_last_poll;            /* when pal_fm1_service last read the baseband */
static const uint8_t g_order[FM1BLE_CONN_SETUPS] = PAL_FM1_SETUP_ORDER;
/* the connection's TX mode (fm1ble.h FM1BLE_TX_*), the TX words at the arm, the connection's events in which TXTOG
 * changed, stale packets dropped and events halted */
static const uint8_t g_order_tx[FM1BLE_TX_MODES] = PAL_FM1_TX_ORDER;
static uint8_t g_tx;
static uint32_t g_txs0, g_csent, g_cstale, g_chalt;

enum { END_FLAG, END_DEADLINE, END_CORDIO };

static uint32_t now(void) { return fm1ble_port_now_us(); }
static uint32_t air_us(uint32_t pdu_len) { return (1u + 4u + pdu_len + 3u) * 8u; }   /* 1M PHY: preamble, AA, PDU, CRC */
static uint32_t sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : v; }
static uint32_t s16(int32_t v) { return (uint32_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v) & 0xFFFFu; }

/* the n-th enabled TX mode of g_order_tx, counting round */
static uint8_t tx_nth(uint32_t n)
{
    uint8_t k, on = 0, x;
    for (k = 0; k < FM1BLE_TX_MODES; k++)
        if (g_cfg.tx_modes & (1u << g_order_tx[k]))
            on++;
    if (!on)
        return FM1BLE_TX_AIR;
    n %= on;
    for (k = 0; k < FM1BLE_TX_MODES; k++) {
        x = g_order_tx[k];
        if ((g_cfg.tx_modes & (1u << x)) && !n--)
            return x;
    }
    return FM1BLE_TX_AIR;
}

/* the n-th enabled setup of g_order, counting round */
static uint8_t setup_nth(uint32_t n)
{
    uint8_t k, on = 0, s;
    for (k = 0; k < FM1BLE_CONN_SETUPS; k++)
        if (g_cfg.conn_setups & (1u << g_order[k]))
            on++;
    if (!on)
        return FM1BLE_CONN_KEEP;
    n %= on;
    for (k = 0; k < FM1BLE_CONN_SETUPS; k++) {
        s = g_order[k];
        if ((g_cfg.conn_setups & (1u << s)) && !n--)
            return s;
    }
    return FM1BLE_CONN_KEEP;
}

/* A second attach (the driver powered down and up again under a running Cordio) keeps the scheduler's timer and the
 * operation Cordio waits for: that one ends by its deadline, since the new radio never ran it. */
void pal_fm1_attach(const pal_fm1_cfg *cfg)
{
    if (!g_cfg.b) {
        g_wait = W_NONE;
        g_held = g_done = g_armed = g_skip_done = 0;
        g_aev = 0;
        g_kind = K_NONE;
        g_tmr_on = 0;
        g_rng = now() ^ 0x9E3779B9u;
        g_conn_open = g_bw_on = 0;
    }
    g_cfg = *cfg;
    if (!g_cfg.sch_setup_us)
        g_cfg.sch_setup_us = 2000u;
    g_cfg.conn_setups &= (1u << FM1BLE_CONN_SETUPS) - 1u;
    if (!g_cfg.conn_setups)
        g_cfg.conn_setups = (1u << FM1BLE_CONN_SETUPS) - 1u;
    g_cfg.tx_modes &= (1u << FM1BLE_TX_MODES) - 1u;
    if (!g_cfg.tx_modes)
        g_cfg.tx_modes = (1u << FM1BLE_TX_MODES) - 1u;
    memset(&g_st, 0, sizeof g_st);
    g_log = g_sum = NULL;
    g_cev = g_cpk = 0;
    g_csent = g_cstale = g_chalt = 0;
    g_set = setup_nth(0);
    g_tx = tx_nth(0);
    g_st.conn_setup = g_set;
    g_st.conn_tx = g_tx;
    g_st.good = g_st.good_tx = PAL_FM1_NO_SETUP;
    g_st.resp_us_min = 0xFFFFu;
}

/* min (lo) and max (hi) of a summary word, two signed 16-bit times (PAL_FM1_LOG_NONE: none yet) */
static void sum_minmax(uint32_t *w, int32_t v)
{
    uint32_t lo = *w & 0xFFFFu, hi = *w >> 16, x = s16(v);
    if (lo == PAL_FM1_LOG_NONE || (int16_t)x < (int16_t)lo)
        lo = x;
    if (hi == PAL_FM1_LOG_NONE || (int16_t)x > (int16_t)hi)
        hi = x;
    *w = lo | hi << 16;
}

/* A new connection's first event: the setup and TX mode it runs (the ones that received PAL_FM1_GOOD packets, else the
 * next in each order), the driver's CONNECT_IND words for the mode, and its summary */
static void conn_start(void)
{
    uint8_t s, x;
    if (g_st.conns && g_cpk >= PAL_FM1_GOOD) {          /* the last connection received on its setup and mode: keep them */
        g_st.good = g_set;
        g_st.good_tx = g_tx;
    } else if (g_st.conns && g_st.good != PAL_FM1_NO_SETUP) {   /* the kept ones failed a connection: back to the orders */
        g_st.good = g_st.good_tx = PAL_FM1_NO_SETUP;
    }
    s = g_st.good != PAL_FM1_NO_SETUP ? (uint8_t)g_st.good : setup_nth(g_st.conns);
    x = g_st.good_tx != PAL_FM1_NO_SETUP ? (uint8_t)g_st.good_tx : tx_nth(g_st.conns);
    if (g_st.conns && s != g_set)
        g_st.setup_moves++;
    g_set = s;
    g_tx = x;
    g_st.conn_setup = s;
    g_st.conn_tx = x;
    fm1ble_op_conn_begin(g_cfg.b, x);
    g_sum = g_st.conns < PAL_FM1_CONNS ? g_st.conn_sum[g_st.conns] : NULL;
    if (g_sum) {
        g_sum[0] = s;
        g_sum[1] = g_sum[2] = PAL_FM1_LOG_NONE | PAL_FM1_LOG_NONE << 16;
        g_sum[3] = x;
        g_st.conn_txs |= (uint32_t)x << (4u * g_st.conns);
    }
    g_st.conns++;
    g_cev = g_cpk = 0;
    g_csent = g_cstale = g_chalt = 0;
}

static uint32_t sat(uint32_t v, uint32_t max) { return v > max ? max : v; }

/* the connection's summary word d (pal_fm1.h) */
static void sum_tx(void)
{
    if (g_sum)
        g_sum[3] = g_tx | sat(g_csent, 0xFFFu) << 4 | sat(g_cstale, 0xFFu) << 16 | sat(g_chalt, 0xFFu) << 24;
}

static void busy_end(uint32_t t)
{
    uint32_t us = t - g_bw_t0;
    g_bw_on = 0;
    g_st.busy_waits++;
    if (us > g_st.busy_us_max)
        g_st.busy_us_max = us;
}

/* A connection event ended (how: its flag, its deadline, or Cordio went on): logged and summed up */
static void conn_end(int how)
{
    uint32_t t = now();
    if (!g_conn_open)
        return;
    g_conn_open = 0;
    if (g_bw_on)
        busy_end(t);
    if (g_cfg.b->d.conn_flips != g_flips0) {
        g_st.tx_sent[g_tx]++;
        g_csent++;
    }
    if (how == END_DEADLINE)
        g_chalt++;
    if (g_log) {
        uint32_t x = fm1ble_op_txstate(g_cfg.b);
        g_log[3] |= (x & 0xFFFFu) | (x & FM1BLE_TXS_TXTOG ? 1u << 17 : 0u) | (x & FM1BLE_TXS_RXTOG ? 1u << 19 : 0u) |
                    (x & FM1BLE_TXS_BUF0 ? 1u << 20 : 0u) | (x & FM1BLE_TXS_BUF1 ? 1u << 21 : 0u);
        if (g_cfg.b->d.conn_flips != g_flips0)
            g_log[0] |= 1u << 10;
        if (how == END_FLAG)
            g_log[1] = (g_log[1] & 0xFFFFu) | s16((int32_t)(t - g_due)) << 16;
        else
            g_log[0] |= how == END_DEADLINE ? 1u << 11 : 1u << 12;
        g_log = NULL;
    }
    g_cev++;
    if (g_sum) {
        g_sum[0] = (g_sum[0] & ~0xFFF0u) | (g_cev > 0xFFFu ? 0xFFFu : g_cev) << 4;
        if (!g_conn_rx && how == END_FLAG)
            sum_minmax(&g_sum[2], (int32_t)(t - g_due));
    }
    sum_tx();
}

const pal_fm1_stats *pal_fm1_state(void) { return &g_st; }

/* ---- pal_bb.h ---- */

void PalBbInit(void) { }                    /* the radio is up before Cordio starts (pal_fm1_attach) */
void PalBbEnable(void) { }
void PalBbDisable(void) { }
void PalBbSetProtId(uint8_t protId) { (void)protId; }
void PalBbRegisterProtIrq(uint8_t protId, bbDrvIrqCback_t timerCback, bbDrvIrqCback_t radioCback)
{
    (void)protId; (void)timerCback; (void)radioCback;
}
uint32_t PalBbGetCurrentTime(void) { return now(); }

bool_t PalBbGetTimestamp(uint32_t *pTime)
{
    if (pTime)
        *pTime = now();
    return TRUE;
}

void PalBbLoadCfg(PalBbCfg_t *pCfg)
{
    pCfg->clkPpm = 50;                      /* the crystal's accuracy: Cordio widens windows by it [measure on FM-1]; 50 is the
                                               worst a peripheral may claim at the lowest sleep clock class but one */
    pCfg->rfSetupDelayUsec = BB_RF_SETUP_DELAY_US;
    pCfg->maxScanPeriodMsec = BB_MAX_SCAN_PERIOD_MS;
    pCfg->schSetupDelayUsec = g_cfg.sch_setup_us ? g_cfg.sch_setup_us : 2000u;
    pCfg->BbTimerBoundaryUsec = BB_TIMER_1MHZ_MAX_VALUE_US;
}

/* ---- pal_bb_ble.h ---- */

void PalBbBleInit(void) { }
void PalBbBleEnable(void) { }
void PalBbBleDisable(void) { }
void PalBbBleLowPower(void) { }
void PalBbBleEnableDataWhitening(bool_t enable) { (void)enable; }   /* the hardware whitens by itself [unchecked] */
void PalBbBleEnablePrbs15(bool_t enable) { (void)enable; }

void PalBbBleSetChannelParam(PalBbBleChan_t *pChan)
{
    g_chan = pChan->chanIdx;
    if (fm1ble_op_chan(g_cfg.b, pChan->accAddr, pChan->crcInit, pChan->chanIdx) != FM1BLE_OK)
        g_st.refused++;
}

void PalBbBleSetDataParams(const PalBbBleDataParam_t *pParam)
{
    g_txcb = pParam->txCback;
    g_rxcb = pParam->rxCback;
    g_due = pParam->dueUsec;
    g_rx_timeout = pParam->rxTimeoutUsec;
}

void PalBbBleSetOpParams(const PalBbBleOpParam_t *pOpParam) { (void)pOpParam; }   /* the hardware's own T_IFS turnaround */

/* Cordio's buffers in one PDU */
static uint16_t gather(const PalBbBleTxBufDesc_t descs[], uint8_t cnt, uint8_t *pdu)
{
    uint16_t n = 0;
    uint8_t i;
    for (i = 0; i < cnt; i++) {
        if (n + descs[i].len > PDU_MAX)
            return 0;
        memcpy(pdu + n, descs[i].pBuf, descs[i].len);
        n = (uint16_t)(n + descs[i].len);
    }
    return n;
}

/* A new operation: forget the last one's state. An event still running (its end not seen) will raise its end flag: that
 * one is skipped. */
static void op_begin(uint32_t window)
{
    int32_t d = (int32_t)(g_due - now());
    g_skip_done = (uint8_t)(g_armed && !g_done);
    g_held = g_done = g_fail = g_resp_late = 0;
    g_deadline = g_due + window + 2u * (air_us(PDU_MAX) + 150u) + SLACK_US;
    if (d < 0)
        g_st.late_ops++;
}

static uint32_t delay_to_due(void)
{
    int32_t d = (int32_t)(g_due - now());
    return d > 0 ? (uint32_t)d : 0u;
}

static void op_refused(void)            /* the completion says FAILED at once */
{
    g_st.refused++;
    g_fail = 1;
    g_done = 1;
    g_armed = 0;
}

/* A packet on a channel not yet asked for in the hardware advertising event armed for this one: it goes in that event.
 * Done at once when the event has ended; else when it ends or a request comes in. A packet received and not yet taken
 * stays for this one's receive. */
static int adv_covered(void)
{
    uint8_t bit = (uint8_t)(1u << (g_chan - 37u));
    if (!g_aev || (g_aev_mask & bit) || (uint32_t)(now() - g_aev_t) > ADV_SPAN_US)
        return 0;
    g_aev_mask |= bit;
    g_wait = W_TX;
    g_fail = g_resp_late = 0;
    if (!g_armed)
        g_done = 1;
    g_st.adv_ops++;
    g_st.adv_covered++;
    return 1;
}

/* The scan response of Cordio's advertising operation in progress, as it is now (*rsp NULL and 0 when it has none) */
static uint16_t adv_rsp(const uint8_t **rsp)
{
    BbOpDesc_t *bod = BbGetCurrentBod();
    const BbBleSlvAdvEvent_t *a;

    *rsp = NULL;
    if (!bod || !bod->prot.pBle || bod->prot.pBle->chan.opType != BB_BLE_OP_SLV_ADV_EVENT)
        return 0;
    a = &bod->prot.pBle->op.slvAdv;
    if (!a->pTxRspBuf || !a->txRspLen)
        return 0;
    *rsp = a->pTxRspBuf;
    return a->txRspLen;
}

void PalBbBleTxData(PalBbBleTxBufDesc_t descs[], uint8_t cnt)
{
    uint8_t pdu[PDU_MAX];
    uint16_t n = gather(descs, cnt, pdu), nrsp;
    const uint8_t *rsp;

    if (g_chan >= 37 && g_chan <= 39 && adv_covered())
        return;
    op_begin(ADV_EVENT_US);
    g_wait = W_TX;
    g_aev = 0;
    nrsp = adv_rsp(&rsp);
    if (!n || g_chan < 37 || fm1ble_op_adv(g_cfg.b, delay_to_due(), pdu, n, rsp, nrsp) != FM1BLE_OK) {
        op_refused();                   /* a master's first transmit on a data channel is not built (peripheral only) */
        return;
    }
    g_armed = 1;
    g_kind = K_ADV;
    g_aev = 1;
    g_aev_mask = (uint8_t)(1u << (g_chan - 37u));
    g_aev_t = now();
    g_st.adv_ops++;
    g_st.adv_events++;
}

void PalBbBleRxData(uint8_t *pBuf, uint16_t len)
{
    uint32_t delay, pad;
    uint8_t first = g_kind != K_CONN;   /* after advertising: a new connection's first event */
    conn_end(END_CORDIO);               /* one still open (Cordio went on without its end): ended here */
    op_begin(g_rx_timeout + 2u * FM1BLE_CONN_WIDE_US);
    g_wait = W_RX;
    g_rxbuf = pBuf;
    g_rxlen = len;
    g_aev = 0;
    if (first && g_chan <= 36)          /* a new connection: its setup (conn_start) */
        conn_start();
    if (g_chan <= 36)
        fm1ble_op_conn_setup(g_cfg.b, g_set);
    delay = delay_to_due();
    if (g_chan > 36 || fm1ble_op_conn(g_cfg.b, delay, g_rx_timeout) != FM1BLE_OK) {
        op_refused();
        return;
    }
    /* PAD (and PADJ, PADB, PADW) and WIDE arm the counter that much earlier, but fm1ble_op_conn clamps the delay at 0 */
    pad = g_set == FM1BLE_CONN_WIDE ? FM1BLE_CONN_WIDE_US :
          g_set == FM1BLE_CONN_PAD || g_set >= FM1BLE_CONN_PADJ ? FM1BLE_CONN_PAD_US : 0u;
    g_win_early = delay > pad ? pad : delay;
    g_flips0 = g_cfg.b->d.conn_flips;
    g_txs0 = fm1ble_op_txstate(g_cfg.b);
    g_st.tx_events[g_tx]++;
    g_log = NULL;
    if (g_st.log_n < PAL_FM1_LOG && g_cev < PAL_FM1_LOG_PER) {
        g_log = g_st.log[g_st.log_n++];
        g_log[0] = (uint32_t)(g_chan & 0x3Fu) | (uint32_t)(g_set & 3u) << 6 | (uint32_t)(g_set >> 2 & 1u) << 14 |
                   (first ? 1u << 13 : 0u) | sat16(g_rx_timeout) << 16;
        g_log[1] = PAL_FM1_LOG_NONE | PAL_FM1_LOG_NONE << 16;
        g_log[2] = sat16(delay);
        g_log[3] = (uint32_t)(g_tx & 3u) << 22 | 0xFFu << 24 | (g_txs0 & FM1BLE_TXS_TXTOG ? 1u << 16 : 0u) |
                   (g_txs0 & FM1BLE_TXS_RXTOG ? 1u << 18 : 0u);
    }
    g_armed = 1;
    g_kind = K_CONN;
    g_conn_open = 1;
    g_conn_rx = 0;
    g_st.conn_ops++;
    g_st.set_events[g_set]++;
    g_st.conn_timeout_us = g_rx_timeout;
}

void PalBbBleRxTifsData(uint8_t *pBuf, uint16_t len)
{
    g_wait = W_RX;
    g_rxbuf = pBuf;
    g_rxlen = len;
}

void PalBbBleTxTifsData(PalBbBleTxBufDesc_t descs[], uint8_t cnt)
{
    uint8_t pdu[PDU_MAX];
    uint16_t n = gather(descs, cnt, pdu);

    g_wait = W_TIFS_TX;
    if (g_kind == K_CONN && g_conn_rx) {
        uint32_t lat = now() - g_rx_t, k = lat < 50u ? 0u : lat < 100u ? 1u : lat < 150u ? 2u : 3u;
        if (lat > g_st.resp_us_max)
            g_st.resp_us_max = lat;
        if (lat < g_st.resp_us_min)
            g_st.resp_us_min = lat;
        if ((g_st.resp_hist >> (8u * k) & 0xFFu) < 0xFFu)
            g_st.resp_hist += 1u << (8u * k);
        if (g_log)
            g_log[3] = (g_log[3] & 0x00FFFFFFu) | sat(lat / 2u, 0xFFu) << 24;
        if (g_tx == FM1BLE_TX_INTIME && lat > PAL_FM1_TIFS_US && !g_done) {   /* too late: what is in the slots goes */
            g_st.resp_skipped++;
            g_resp_late = 1;
            return;
        }
    }
    if (g_done) {                       /* the event is over: too late for T_IFS */
        g_st.resp_late++;
        g_resp_late = 1;
        return;
    }
    if (!n || fm1ble_op_resp(g_cfg.b, pdu, n) != FM1BLE_OK) {
        g_st.refused++;
        g_resp_late = 1;
        return;
    }
    g_st.resp_set++;
}

void PalBbBleCancelData(void)
{
    conn_end(END_CORDIO);
    fm1ble_op_stop(g_cfg.b);
    g_wait = W_NONE;
    g_held = g_done = g_armed = g_skip_done = 0;
    g_aev = 0;
}

void PalBbBleCancelTifs(void) { }       /* nothing is pending apart from the event itself */

/* ---- completions ---- */

static int deliver(void)
{
    PalBbBleTxIsr_t tx = g_txcb;
    PalBbBleRxIsr_t rx = g_rxcb;

    if (g_wait == W_TX && (g_held || g_done)) {
        g_wait = W_NONE;
        g_st.tx_done++;
        if (g_chan >= 37 && g_chan <= 39)
            g_st.adv_done[g_chan - 37]++;
        tx(g_fail ? BB_STATUS_FAILED : BB_STATUS_SUCCESS);
        return 1;
    }
    if (g_wait == W_RX && g_held) {
        uint16_t n = g_held_n < g_rxlen ? g_held_n : g_rxlen;
        memcpy(g_rxbuf, g_held_pdu, n);
        g_held = 0;
        g_wait = W_NONE;
        if (g_held_ok)
            g_st.rx_ok++;
        else
            g_st.rx_bad++;
        rx(g_held_ok ? BB_STATUS_SUCCESS : BB_STATUS_CRC_FAILED, RSSI_NONE, 0, g_held_ts, 0);
        return 1;
    }
    if (g_wait == W_RX && g_done) {
        g_wait = W_NONE;
        g_st.rx_timeout++;
        rx(g_fail ? BB_STATUS_FAILED : BB_STATUS_RX_TIMEOUT, RSSI_NONE, 0, now(), 0);
        return 1;
    }
    if (g_wait == W_TIFS_TX && g_done) {
        g_wait = W_NONE;
        g_st.tx_done++;
        tx(g_resp_late || g_fail ? BB_STATUS_FAILED : BB_STATUS_SUCCESS);
        return 1;
    }
    return 0;
}

void pal_fm1_service(void)
{
    fm1ble_pkt p;
    uint32_t ev, t;
    int progress;

    if (g_busy || !g_cfg.b)
        return;
    g_busy = 1;
    do {
        progress = 0;
        ev = fm1ble_op_service(g_cfg.b, &p);
        t = now();
        if ((ev & FM1BLE_EV_RX) && g_kind == K_CONN && g_conn_open &&
            (int32_t)((t - air_us(2u + p.hdr[1])) - (g_due - g_win_early)) < -(int32_t)PAL_FM1_STALE_US) {
            /* a flag an earlier event raised late, before this window could open: not the central's packet */
            g_st.stale_rx++;
            g_cstale++;
            if (g_log)
                g_log[0] |= 1u << 15;
            sum_tx();
            ev &= ~FM1BLE_EV_RX;
            progress = 1;
        }
        if (ev & FM1BLE_EV_RX) {
            if (g_kind == K_ADV && (p.hdr[0] & 0xFu) == 5u)
                g_st.cind_gap_us = t - g_last_poll;
            g_held_n = (uint16_t)(2u + p.hdr[1]);
            g_held_pdu[0] = p.hdr[0];
            g_held_pdu[1] = p.hdr[1];
            memcpy(g_held_pdu + 2, p.payload, p.hdr[1]);
            g_held_ok = p.ok;
            g_held_ts = t - air_us(g_held_n);
            g_held = 1;
            progress = 1;
            if (g_kind == K_CONN && g_conn_open) {
                /* from the window's own start for this setup (g_win_early before g_due) */
                int32_t arr = (int32_t)(g_held_ts - g_due) + (int32_t)g_win_early;
                if (!g_st.first_rx_tx)
                    g_st.first_rx_tx = fm1ble_op_txstate(g_cfg.b) | 1u << 31;
                g_st.tx_rx[g_tx]++;
                if (p.stat & 0x100u)
                    g_st.tx_acks[g_tx]++;
                g_st.conn_rx++;
                if (!p.ok)
                    g_st.conn_rx_bad++;
                if (!g_conn_rx) {
                    if (!g_st.set_rx[g_set] || arr < g_st.set_arr_min[g_set])
                        g_st.set_arr_min[g_set] = arr;
                    if (!g_st.set_rx[g_set] || arr > g_st.set_arr_max[g_set])
                        g_st.set_arr_max[g_set] = arr;
                    g_st.set_rx[g_set]++;
                    g_cpk++;
                    if (g_sum) {
                        uint32_t at = s16((int32_t)(g_held_ts - g_due));
                        if ((g_sum[1] & 0xFFFFu) == PAL_FM1_LOG_NONE)
                            g_sum[1] = (g_sum[1] & 0xFFFF0000u) | at;
                        g_sum[1] = (g_sum[1] & 0xFFFFu) | at << 16;
                        g_sum[0] = (g_sum[0] & 0xFFFFu) | (g_cpk > 0xFFFFu ? 0xFFFFu : g_cpk) << 16;
                    }
                    if (g_log) {
                        g_log[0] |= 1u << 8 | (p.ok ? 1u << 9 : 0u);
                        g_log[1] = (g_log[1] & 0xFFFF0000u) | s16((int32_t)(g_held_ts - g_due));
                        g_log[2] = (g_log[2] & 0xFFFFu) | (uint32_t)g_cfg.b->d.conn_dhdr << 16;
                    }
                }
                g_conn_rx = 1;
                g_rx_t = t;
            }
            if (g_kind == K_ADV && g_armed && (p.hdr[0] & 0xFu) == 5u) {   /* CONNECT_IND: the advertising event ends here */
                fm1ble_op_stop(g_cfg.b);
                g_armed = 0;
                g_done = 1;
                g_st.adv_conn_ind++;
            }
        }
        if (ev & FM1BLE_EV_DONE) {
            if (g_skip_done) {
                g_skip_done = 0;
            } else {
                g_done = 1;
                g_armed = 0;
                fm1ble_op_stop(g_cfg.b);
                if (g_kind == K_ADV)
                    g_st.adv_ended++;
                else if (g_kind == K_CONN) {
                    g_st.conn_ended++;
                    conn_end(END_FLAG);
                }
            }
            progress = 1;
        }
        if (g_armed && !g_done && (int32_t)(t - g_deadline) > 0) {   /* the baseband never said: the operation ends */
            g_done = 1;
            g_armed = 0;
            g_skip_done = 0;
            g_st.stuck++;
            if (g_kind == K_CONN) {                     /* halted: it must not finish into the next event */
                g_st.halts++;
                if (fm1ble_op_halt(g_cfg.b) != FM1BLE_OK)
                    g_st.halt_busy++;
                conn_end(END_DEADLINE);
            } else {
                fm1ble_op_stop(g_cfg.b);
            }
            progress = 1;
        }
        if (g_tmr_on && (int32_t)(t - g_tmr_due) >= 0) {
            uint32_t late = t - g_tmr_due;
            g_tmr_on = 0;
            g_st.timer_fired++;
            if (late > g_st.timer_late_us)
                g_st.timer_late_us = late;
            if (g_tmr_cb)
                g_tmr_cb();
            progress = 1;
        }
        g_last_poll = t;
        if (deliver())
            progress = 1;
        /* the busy wait: a connection event armed and its packet not in yet */
        if (!progress && !g_cfg.no_busy_wait && g_kind == K_CONN && g_conn_open && g_armed && !g_done && g_wait == W_RX) {
            if (!g_bw_on) {
                g_bw_on = 1;
                g_bw_t0 = t;
            }
            fm1ble_port_delay_us(BUSY_STEP_US);
            progress = 1;
        } else if (!progress && g_bw_on) {
            busy_end(t);
        }
    } while (progress);
    g_busy = 0;
}

/* ---- pal_timer.h: the scheduler's one-shot timer, a deadline ---- */

void PalTimerInit(PalTimerCompCback_t expCback) { g_tmr_cb = expCback; g_tmr_on = 0; }

void PalTimerStart(uint32_t expUsec)
{
    uint32_t lead = g_cfg.no_timer_lead ? 0u : PAL_FM1_TIMER_LEAD_US;
    g_tmr_due = now() + (expUsec > lead ? expUsec - lead : 0u);
    g_tmr_on = 1;
}

void PalTimerStop(void) { g_tmr_on = 0; }

/* ---- pal_crypto.h ---- */

/* Not reached with a public address and privacy off (bb_ble_reslist.c only); counted, so a run that reaches it shows. */
void PalCryptoAesEcb(const uint8_t *pKey, uint8_t *pOut, const uint8_t *pIn)
{
    (void)pKey; (void)pIn;
    g_st.aes_calls++;
    memset(pOut, 0, 16);
}

void PalCryptoGenerateRandomNumber(uint8_t *pBuf, uint8_t len)
{
    uint8_t i;
    for (i = 0; i < len; i++) {
        uint32_t r;
        if (g_cfg.rand32) {
            r = g_cfg.rand32(g_cfg.ctx) ^ now();
        } else {
            g_rng ^= g_rng << 13;
            g_rng ^= g_rng >> 17;
            g_rng ^= g_rng << 5;
            r = g_rng;
        }
        pBuf[i] = (uint8_t)(r ^ r >> 8 ^ r >> 16 ^ r >> 24);
    }
}

/* ---- pal_radio.h: one level, its power not known (E.7: the chip's maximum level is 0 at reset) ---- */

int8_t PalRadioGetActualTxPower(int8_t txPwr, bool_t compFlag) { (void)txPwr; (void)compFlag; return 0; }

void PalRadioGetSupTxPower(int8_t *pMinTxPwr, int8_t *pMaxTxPwr)
{
    *pMinTxPwr = 0;
    *pMaxTxPwr = 0;
}

void PalRadioReadRfPathComp(int16_t *pTxPathComp, int16_t *pRxPathComp)
{
    *pTxPathComp = 0;
    *pRxPathComp = 0;
}

bool_t PalRadioWriteRfPathComp(int16_t txPathComp, int16_t rxPathComp)
{
    (void)txPathComp; (void)rxPathComp;
    return FALSE;
}

/* ---- pal_sys.h: one context (the main loop), so no masking yet; with IRQs 45 and 29 attached these must mask them ---- */

void PalEnterCs(void) { }
void PalExitCs(void) { }
void PalSysAssertTrap(void) { g_st.asserts++; }
uint32_t PalSysGetAssertCount(void) { return g_st.asserts; }
uint32_t PalSysGetStackUsage(void) { return 0; }
void PalSysSetTrap(bool_t enable) { (void)enable; }
