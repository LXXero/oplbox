/* SPDX-License-Identifier: GPL-3.0-only
 * The chip's side of Cordio's controller as host-test stubs (docs/BLUETOOTH_CORDIO.md, PAL_NEEDED.md): every PAL function
 * the peripheral-only controller needs, each one counting its calls and keeping its last arguments, nothing touching
 * hardware. They let the whole controller link and run on the host: LHCI commands are answered, advertising can be
 * enabled, the scheduler runs; no radio exists, so no operation completes (pal_stub.tx_ops counts what the controller
 * asked the baseband to send). Own code, written from the contracts in platform/include/pal_*.h (names, arguments and
 * the comments there), not from any chip's driver. Host only: never built into the firmware.
 *
 * Test hooks: pal_stub.now_us is the baseband clock (the test advances it); pal_stub_timer_fire() calls the scheduler
 * timer's callback when the deadline given to PalTimerStart has come. */
#include <string.h>
#include "wsf_types.h"
#include "pal_bb.h"
#include "pal_bb_ble.h"
#include "pal_crypto.h"
#include "pal_radio.h"
#include "pal_sys.h"
#include "pal_timer.h"
#include "pal_stubs_host.h"

pal_stub_state pal_stub;
static PalTimerCompCback_t g_timer_cb;

/* ---- pal_bb.h ---- */

void PalBbInit(void) { pal_stub.calls[PS_BbInit]++; }
void PalBbEnable(void) { pal_stub.calls[PS_BbEnable]++; }
void PalBbDisable(void) { pal_stub.calls[PS_BbDisable]++; }
void PalBbSetProtId(uint8_t protId) { pal_stub.calls[PS_BbSetProtId]++; pal_stub.prot_id = protId; }
uint32_t PalBbGetCurrentTime(void) { pal_stub.calls[PS_BbGetCurrentTime]++; return pal_stub.now_us; }

bool_t PalBbGetTimestamp(uint32_t *pTime)
{
    pal_stub.calls[PS_BbGetTimestamp]++;
    if (pTime)                          /* NULL asks only whether a timestamp exists (lhci_main.c, ll_init.c) */
        *pTime = pal_stub.now_us;
    return TRUE;
}

void PalBbLoadCfg(PalBbCfg_t *pCfg)
{
    pal_stub.calls[PS_BbLoadCfg]++;
    pCfg->clkPpm = 20;
    pCfg->rfSetupDelayUsec = BB_RF_SETUP_DELAY_US;
    pCfg->maxScanPeriodMsec = BB_MAX_SCAN_PERIOD_MS;
    pCfg->schSetupDelayUsec = BB_SCH_SETUP_DELAY_US;
    pCfg->BbTimerBoundaryUsec = BB_TIMER_1MHZ_MAX_VALUE_US;
}

/* ---- pal_bb_ble.h ---- */

void PalBbBleInit(void) { pal_stub.calls[PS_BbBleInit]++; }
void PalBbBleEnable(void) { pal_stub.calls[PS_BbBleEnable]++; }
void PalBbBleDisable(void) { pal_stub.calls[PS_BbBleDisable]++; }
void PalBbBleLowPower(void) { pal_stub.calls[PS_BbBleLowPower]++; }

void PalBbBleSetChannelParam(PalBbBleChan_t *pChan)
{
    pal_stub.calls[PS_BbBleSetChannelParam]++;
    pal_stub.chan_idx = pChan->chanIdx;
    pal_stub.acc_addr = pChan->accAddr;
    pal_stub.crc_init = pChan->crcInit;
}

void PalBbBleSetDataParams(const PalBbBleDataParam_t *pParam)
{
    pal_stub.calls[PS_BbBleSetDataParams]++;
    pal_stub.due_us = pParam->dueUsec;
    pal_stub.rx_timeout_us = pParam->rxTimeoutUsec;
    pal_stub.tx_cback = pParam->txCback;
    pal_stub.rx_cback = pParam->rxCback;
}

void PalBbBleSetOpParams(const PalBbBleOpParam_t *pOpParam)
{
    pal_stub.calls[PS_BbBleSetOpParams]++;
    pal_stub.ifs_mode = (uint8_t)pOpParam->ifsMode;
}

static void note_tx(PalBbBleTxBufDesc_t descs[], uint8_t cnt)
{
    pal_stub.tx_ops++;
    pal_stub.tx_bufs = cnt;
    pal_stub.tx_len = cnt ? descs[0].len : 0;
    if (cnt && descs[0].pBuf)
        memcpy(pal_stub.tx_first, descs[0].pBuf, descs[0].len < sizeof pal_stub.tx_first ? descs[0].len : sizeof pal_stub.tx_first);
}

void PalBbBleTxData(PalBbBleTxBufDesc_t descs[], uint8_t cnt) { pal_stub.calls[PS_BbBleTxData]++; note_tx(descs, cnt); }
void PalBbBleTxTifsData(PalBbBleTxBufDesc_t descs[], uint8_t cnt) { pal_stub.calls[PS_BbBleTxTifsData]++; note_tx(descs, cnt); }
void PalBbBleRxData(uint8_t *pBuf, uint16_t len) { pal_stub.calls[PS_BbBleRxData]++; pal_stub.rx_buf = pBuf; pal_stub.rx_len = len; }
void PalBbBleRxTifsData(uint8_t *pBuf, uint16_t len) { pal_stub.calls[PS_BbBleRxTifsData]++; pal_stub.rx_buf = pBuf; pal_stub.rx_len = len; }
void PalBbBleCancelData(void) { pal_stub.calls[PS_BbBleCancelData]++; }
void PalBbBleCancelTifs(void) { pal_stub.calls[PS_BbBleCancelTifs]++; }
void PalBbBleEnableDataWhitening(bool_t enable) { pal_stub.calls[PS_BbBleEnableDataWhitening]++; (void)enable; }
void PalBbBleEnablePrbs15(bool_t enable) { pal_stub.calls[PS_BbBleEnablePrbs15]++; (void)enable; }

/* ---- pal_timer.h: the scheduler's one-shot timer ---- */

void PalTimerInit(PalTimerCompCback_t expCback)
{
    pal_stub.calls[PS_TimerInit]++;
    g_timer_cb = expCback;
}

void PalTimerStart(uint32_t expUsec)
{
    pal_stub.calls[PS_TimerStart]++;
    pal_stub.timer_due_us = pal_stub.now_us + expUsec;
    pal_stub.timer_on = 1;
}

void PalTimerStop(void)
{
    pal_stub.calls[PS_TimerStop]++;
    pal_stub.timer_on = 0;
}

/* 1 when the callback ran: the deadline has come */
int pal_stub_timer_fire(void)
{
    if (!pal_stub.timer_on || (int32_t)(pal_stub.now_us - pal_stub.timer_due_us) < 0 || !g_timer_cb)
        return 0;
    pal_stub.timer_on = 0;
    g_timer_cb();
    return 1;
}

/* ---- pal_crypto.h ---- */

/* AES-128 is not here: no encryption in this configuration. The controller still calls it for resolvable-address
 * and key-derivation helpers that are compiled in; the stub returns zeros. */
void PalCryptoAesEcb(const uint8_t *pKey, uint8_t *pOut, const uint8_t *pIn)
{
    pal_stub.calls[PS_CryptoAesEcb]++;
    (void)pKey;
    memcpy(pOut, pIn, 16);
}

void PalCryptoGenerateRandomNumber(uint8_t *pBuf, uint8_t len)
{
    uint8_t i;
    pal_stub.calls[PS_CryptoRandom]++;
    for (i = 0; i < len; i++) {
        pal_stub.rng = pal_stub.rng * 1664525u + 1013904223u;
        pBuf[i] = (uint8_t)(pal_stub.rng >> 24);
    }
}

/* ---- pal_radio.h ---- */

int8_t PalRadioGetActualTxPower(int8_t txPwr, bool_t compFlag) { pal_stub.calls[PS_RadioGetActualTxPower]++; (void)compFlag; return txPwr; }

void PalRadioGetSupTxPower(int8_t *pMinTxPwr, int8_t *pMaxTxPwr)
{
    pal_stub.calls[PS_RadioGetSupTxPower]++;
    *pMinTxPwr = -20;
    *pMaxTxPwr = 0;
}

void PalRadioReadRfPathComp(int16_t *pTxPathComp, int16_t *pRxPathComp)
{
    pal_stub.calls[PS_RadioReadRfPathComp]++;
    *pTxPathComp = 0;
    *pRxPathComp = 0;
}

bool_t PalRadioWriteRfPathComp(int16_t txPathComp, int16_t rxPathComp)
{
    pal_stub.calls[PS_RadioWriteRfPathComp]++;
    (void)txPathComp; (void)rxPathComp;
    return TRUE;
}

/* ---- pal_sys.h ---- */

void PalEnterCs(void) { pal_stub.calls[PS_EnterCs]++; }
void PalExitCs(void) { pal_stub.calls[PS_ExitCs]++; }
void PalSysAssertTrap(void) { pal_stub.calls[PS_SysAssertTrap]++; }
uint32_t PalSysGetAssertCount(void) { pal_stub.calls[PS_SysGetAssertCount]++; return 0; }
uint32_t PalSysGetStackUsage(void) { pal_stub.calls[PS_SysGetStackUsage]++; return 0; }
void PalSysSetTrap(bool_t enable) { pal_stub.calls[PS_SysSetTrap]++; (void)enable; }
