/* SPDX-License-Identifier: GPL-3.0-only
 * Cordio's PAL on the FM-1's BLE baseband (PAL_NEEDED.md, docs/BLUETOOTH_CORDIO.md): the 37 functions of Cordio's six PAL
 * headers, on the radio driver's one-packet operations (fm1ble.h "for Cordio's PAL"). Own code, written from the contracts
 * in platform/include/pal_*.h and from Cordio's callers (bb_ble_adv_slave.c, bb_ble_conn_slave.c), not from any chip's PAL.
 * Never run on an FM-1.
 *
 * The translation. Cordio asks for one packet at a time with an absolute time; the driver makes each of Cordio's operations
 * a one-channel hardware event: an advertising packet (PalBbBleTxData on channel 37-39) or a peripheral's connection event
 * (PalBbBleRxData on 0-36). The baseband reports two things per event, a packet received and the event's end, and they
 * become Cordio's completions:
 *   advertising:  the ADV PDU went (txCback) when a request came in or the event ended; the request (rxCback) with the
 *                 packet, or a timeout at the end; the SCAN_RSP Cordio then hands over (PalBbBleTxTifsData) goes into the
 *                 response buffer, and its txCback comes at the end;
 *   connection:   the master's packet (rxCback), or a timeout at the end; the response goes into the response buffer
 *                 and its txCback comes at the end; a second receive in the same event times out (one exchange per event:
 *                 the hardware's own behaviour after the response is [measure on FM-1]).
 * The response must be in its buffer before T_IFS (150 us) after the packet's end: pal_fm1_service has to run from the
 * receive interrupt (IRQ 29) for that, and from the main loop it is too late (counted: resp_late, and Cordio is told the
 * response failed). The baseband's interrupts are not attached in this version: everything runs from the main loop.
 *
 * Time: PalBbGetCurrentTime is the port's microsecond clock (TIMER4 on the FM-1), not the baseband's slot counter; an
 * operation is armed (due - now) from now. A packet's timestamp (its start, which Cordio uses to place a connection's
 * anchor) is the time pal_fm1_service saw it minus its time on the air: right when the service runs in the interrupt,
 * late by the main loop's latency otherwise. The scheduler's timer is a deadline checked in pal_fm1_service.
 *
 * What a baseband that never answers does: each operation has a deadline (its start, the window and two packets, plus
 * 5 ms); past it the operation counts as ended, so Cordio is never left waiting (the stub PAL's hang, docs/BLUETOOTH_CORDIO.md).
 */
#include <string.h>
#include "wsf_types.h"
#include "pal_bb.h"
#include "pal_bb_ble.h"
#include "pal_crypto.h"
#include "pal_radio.h"
#include "pal_sys.h"
#include "pal_timer.h"
#include "fm1ble.h"
#include "fm1ble_port.h"
#include "pal_fm1.h"

#define RSSI_NONE       127     /* Cordio's "not available": the RSSI register (C.3) is not read [measure on FM-1] */
#define SLACK_US        5000u
#define PDU_MAX         (2u + 255u)

enum { W_NONE, W_TX, W_RX, W_TIFS_TX };

static pal_fm1_cfg g_cfg;
static pal_fm1_stats g_st;
static uint8_t g_busy;

/* the operation in flight */
static PalBbBleTxIsr_t g_txcb;
static PalBbBleRxIsr_t g_rxcb;
static uint32_t g_due, g_rx_timeout;
static uint8_t g_chan, g_wait, g_fail, g_done, g_skip_done, g_resp_late, g_armed;
static uint32_t g_deadline;
static uint8_t *g_rxbuf;
static uint16_t g_rxlen;

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

static uint32_t now(void) { return fm1ble_port_now_us(); }
static uint32_t air_us(uint32_t pdu_len) { return (1u + 4u + pdu_len + 3u) * 8u; }   /* 1M PHY: preamble, AA, PDU, CRC */

void pal_fm1_attach(const pal_fm1_cfg *cfg)
{
    g_cfg = *cfg;
    if (!g_cfg.sch_setup_us)
        g_cfg.sch_setup_us = 2000u;
    memset(&g_st, 0, sizeof g_st);
    g_wait = W_NONE;
    g_held = g_done = g_armed = g_skip_done = 0;
    g_tmr_on = 0;
    g_rng = now() ^ 0x9E3779B9u;
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

void PalBbBleTxData(PalBbBleTxBufDesc_t descs[], uint8_t cnt)
{
    uint8_t pdu[PDU_MAX];
    uint16_t n = gather(descs, cnt, pdu);

    op_begin(0);
    g_wait = W_TX;
    if (!n || g_chan < 37 || fm1ble_op_adv(g_cfg.b, delay_to_due(), pdu, n) != FM1BLE_OK) {
        op_refused();                   /* a master's first transmit on a data channel is not built (peripheral only) */
        return;
    }
    g_armed = 1;
    g_st.adv_ops++;
}

void PalBbBleRxData(uint8_t *pBuf, uint16_t len)
{
    op_begin(g_rx_timeout);
    g_wait = W_RX;
    g_rxbuf = pBuf;
    g_rxlen = len;
    if (g_chan > 36 || fm1ble_op_conn(g_cfg.b, delay_to_due(), g_rx_timeout) != FM1BLE_OK) {
        op_refused();
        return;
    }
    g_armed = 1;
    g_st.conn_ops++;
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
    fm1ble_op_stop(g_cfg.b);
    g_wait = W_NONE;
    g_held = g_done = g_armed = g_skip_done = 0;
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
        if (ev & FM1BLE_EV_RX) {
            g_held_n = (uint16_t)(2u + p.hdr[1]);
            g_held_pdu[0] = p.hdr[0];
            g_held_pdu[1] = p.hdr[1];
            memcpy(g_held_pdu + 2, p.payload, p.hdr[1]);
            g_held_ok = p.ok;
            g_held_ts = t - air_us(g_held_n);
            g_held = 1;
            progress = 1;
        }
        if (ev & FM1BLE_EV_DONE) {
            if (g_skip_done) {
                g_skip_done = 0;
            } else {
                g_done = 1;
                g_armed = 0;
                fm1ble_op_stop(g_cfg.b);
            }
            progress = 1;
        }
        if (g_armed && !g_done && (int32_t)(t - g_deadline) > 0) {   /* the baseband never said: the operation ends */
            g_done = 1;
            g_armed = 0;
            g_skip_done = 0;
            fm1ble_op_stop(g_cfg.b);
            g_st.stuck++;
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
        if (deliver())
            progress = 1;
    } while (progress);
    g_busy = 0;
}

/* ---- pal_timer.h: the scheduler's one-shot timer, a deadline ---- */

void PalTimerInit(PalTimerCompCback_t expCback) { g_tmr_cb = expCback; g_tmr_on = 0; }

void PalTimerStart(uint32_t expUsec)
{
    g_tmr_due = now() + expUsec;
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
