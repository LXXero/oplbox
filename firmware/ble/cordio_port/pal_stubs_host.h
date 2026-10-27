/* SPDX-License-Identifier: GPL-3.0-only
 * State kept by pal_stubs_host.c (host tests only). */
#ifndef OPLBOX_BLE_PAL_STUBS_HOST_H
#define OPLBOX_BLE_PAL_STUBS_HOST_H

#include <stdint.h>

/* one counter per PAL function the peripheral-only controller needs (PAL_NEEDED.md) */
enum {
    PS_BbInit, PS_BbEnable, PS_BbDisable, PS_BbSetProtId, PS_BbGetCurrentTime, PS_BbGetTimestamp, PS_BbLoadCfg,
    PS_BbBleInit, PS_BbBleEnable, PS_BbBleDisable, PS_BbBleLowPower, PS_BbBleSetChannelParam, PS_BbBleSetDataParams,
    PS_BbBleSetOpParams, PS_BbBleTxData, PS_BbBleTxTifsData, PS_BbBleRxData, PS_BbBleRxTifsData, PS_BbBleCancelData,
    PS_BbBleCancelTifs, PS_BbBleEnableDataWhitening, PS_BbBleEnablePrbs15,
    PS_TimerInit, PS_TimerStart, PS_TimerStop,
    PS_CryptoAesEcb, PS_CryptoRandom,
    PS_RadioGetActualTxPower, PS_RadioGetSupTxPower, PS_RadioReadRfPathComp, PS_RadioWriteRfPathComp,
    PS_EnterCs, PS_ExitCs, PS_SysAssertTrap, PS_SysGetAssertCount, PS_SysGetStackUsage, PS_SysSetTrap,
    PS_N
};

typedef struct {
    uint32_t calls[PS_N];
    uint32_t now_us;                    /* the baseband clock: the test moves it */
    uint8_t prot_id;
    uint8_t chan_idx, ifs_mode;
    uint32_t acc_addr, crc_init, due_us, rx_timeout_us;
    void (*tx_cback)(uint8_t status);
    void *rx_cback;
    uint32_t tx_ops, tx_bufs, tx_len;
    uint8_t tx_first[64];               /* the first bytes of the last PDU the controller asked to transmit */
    uint8_t *rx_buf;
    uint16_t rx_len;
    uint32_t timer_due_us;
    uint8_t timer_on;
    uint32_t rng;
} pal_stub_state;

extern pal_stub_state pal_stub;

int pal_stub_timer_fire(void);

#endif
