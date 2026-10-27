# What the Cordio controller needs from the chip (the work order for the baseband driver)

This is the exact list of functions the peripheral-only controller leaves undefined when it is linked with
`--gc-sections` (`firmware/ble/cordio_port/SOURCES.txt`, `DEFINES.txt`): 36 functions in six of Cordio's PAL headers
(`firmware/ble/cordio/platform/include/pal_*.h`), and a 37th, `PalSysAssertTrap`, that the port's `WsfAssert` calls (the linker
drops it while `WSF_ASSERT_ENABLED=0`). `pal_stubs_host.c` defines every one as a counting stub for the host
tests; the chip's versions are the driver. Everything else the controller needs is C in this tree (`cordio_ctrl.c`,
`cordio_wsf.c`) or `memset`, `memcpy`, `memcmp` (the bsp's `chip/libc.c`; no `memmove` or `strlen` is needed).

The facts that fill each function are in `docs/BLUETOOTH_RADIO.md` (sections B-K, "What Cordio's hardware layer
needs, and what fills it"); this file says what the controller expects, so the two can be put side by side. "Context"
is where the controller calls it from: **loop** is `cordio_ctrl_poll` and the host's HCI calls, **isr** is the baseband
interrupt (a `PalBbBle*` completion callback) or the scheduler timer's callback, both of which run controller code and
call `WsfSetEvent` / `WsfCsEnter`.

The contract in one paragraph. The controller schedules baseband operations ("BODs") on a microsecond clock and hands
the baseband one protocol operation at a time. For an advertising event it calls, per advertising channel (37, 38, 39
in the order of `advChMap`): `PalBbBleSetChannelParam`, `PalBbBleSetDataParams` (absolute `dueUsec`, a Tx callback, an Rx
callback), `PalBbBleSetOpParams` (what follows the packet: nothing, or a response after T_IFS), then
`PalBbBleTxData` with one PDU. The Tx callback (isr) says the packet went (status 0); the controller then either moves to
the next channel, or (connectable or scannable) calls `PalBbBleRxTifsData` to receive a SCAN_REQ or CONNECT_IND 150 us
after the end of the packet, and from the Rx callback `PalBbBleTxTifsData` for the SCAN_RSP. A connection is the same
with `PalBbBleRxData` / `PalBbBleTxTifsData` pairs once per connection event. The baseband of JieLi's chip runs an
event on its own (it counts slots to an anchor and hops by itself, `BLUETOOTH_RADIO.md` "How the baseband works"), so
the driver is a translation: it turns "this packet at this time on this channel" into the parameter block and the
indexed registers, and turns the baseband's one interrupt per event (or per received packet) into the callbacks.

## pal_bb.h (7)

| Function | Context | What the controller expects | Radio facts |
| --- | --- | --- | --- |
| `void PalBbInit(void)` | loop, once, at `LlInit` | One-time hardware set-up, no radio activity. | B.1, B.2, C.1-C.3, D.1, E.1, E.3; order against `fm1wifi_radio_up` (not settled 1) |
| `void PalBbEnable(void)` | loop, before the first operation (`BbStart`) | Power and clocks on, "just before a first BB operation is executed". | B.1 steps 4-17, D.4 |
| `void PalBbDisable(void)` | loop, after the last operation | Power and clocks off. The controller calls Enable and Disable as the scheduler goes busy and idle. | B.5 |
| `void PalBbSetProtId(uint8_t protId)` | loop, `BbStart`/`BbStop` | Which protocol owns the baseband (BLE only here). A variable. | none |
| `uint32_t PalBbGetCurrentTime(void)` | loop and isr, very often | The baseband clock in **microseconds**, 32 bits, wrapping at `BbTimerBoundaryUsec` (below). Free-running while the baseband is enabled; every `dueUsec` and the scheduler's arithmetic are in this clock. | D.2 (slot counter and bit offset: anchor indexes 0/0xE and 7); a free-running microsecond count is [measure on FM-1] |
| `bool_t PalBbGetTimestamp(uint32_t *pTime)` | loop | TRUE when a timestamp is available (the baseband is active); writes it to `*pTime` **unless it is NULL** (`lhci_main.c` and `ll_init.c` only ask whether one exists). | as above |
| `void PalBbLoadCfg(PalBbCfg_t *pCfg)` | loop, once, from `cordio_ctrl_init` | Fills `clkPpm` (sleep clock accuracy, 20 on the reference board), `rfSetupDelayUsec` (150), `maxScanPeriodMsec` (1000), `schSetupDelayUsec` (500) and `BbTimerBoundaryUsec` (`0xFFFFFFFF` for a 1 MHz 32-bit timer). The values in brackets are what Cordio's reference platform uses (`pal_bb.h`); the chip's own are [measure on FM-1] (the RF setup delay is how long before a packet the radio must be started). | E.1, E.7; values [measure on FM-1] |

## pal_bb_ble.h (15)

| Function | Context | What the controller expects | Radio facts |
| --- | --- | --- | --- |
| `void PalBbBleInit(void)` | loop, once | BLE part of the baseband set up. | D.1, E.1, E.3 |
| `void PalBbBleEnable(void)` / `PalBbBleDisable(void)` | loop | BLE clocks and interrupts on / off, with `PalBbEnable`/`Disable`. | D.4, B.5 |
| `void PalBbBleLowPower(void)` | loop | Allow low power between operations. May do nothing. | D.6 |
| `void PalBbBleSetChannelParam(PalBbBleChan_t *pChan)` | loop and isr | Access address (`accAddr`), CRC init (`crcInit`), channel index (`chanIdx`, 0-39: advertising 37-39), transmit power (`txPower`, dBm), PHY (1M only here: `txPhy`, `rxPhy`). `enc` is unused (no encryption). | AA: BDADDR0/1; CRC: CRCWORD0/1; channel: index 6 and F.1 tables; power: E.7 |
| `void PalBbBleSetDataParams(const PalBbBleDataParam_t *pParam)` | loop and isr | `dueUsec`: **absolute** time of the first packet; `rxTimeoutUsec`: receive window; `txCback(status)` and `rxCback(status, rssi, crc, timestamp, rxPhyOptions)`: called on completion (isr). The structure is on the controller's stack: copy what is kept. | D.2, H; the due time must be turned into a slot count and bit offset |
| `void PalBbBleSetOpParams(const PalBbBleOpParam_t *pOpParam)` | loop and isr | What follows this packet: `ifsMode` = clear (last packet of the event), toggle (the other direction after T_IFS = 150 us) or same-direction at an absolute time `ifsTime`, and the channel for it (`pIfsChan`, NULL: unchanged). | D.3 state codes |
| `void PalBbBleTxData(PalBbBleTxBufDesc_t descs[], uint8_t cnt)` | loop and isr | Transmit these buffers (`len`, `pBuf`: the PDU header and payload, the controller builds them) at the due time; one for advertising and for a connection event's first packet. Completion: `txCback(0)`. | E.4, G steps 8-12, K |
| `void PalBbBleTxTifsData(PalBbBleTxBufDesc_t descs[], uint8_t cnt)` | isr (from an Rx callback) | Transmit 150 us after the end of the packet just received: the SCAN_RSP, or a connection event's response. Completion: `txCback(0)`. | as above; the hardware's own turnaround (H) |
| `void PalBbBleRxData(uint8_t *pBuf, uint16_t len)` | loop and isr | Receive into `pBuf` (`len` bytes of room) until `rxTimeoutUsec`; completion `rxCback`. | E.5, RXPTR/RXMAXBUF, H |
| `void PalBbBleRxTifsData(uint8_t *pBuf, uint16_t len)` | isr (from a Tx callback) | Receive 150 us after the end of the packet just sent (the SCAN_REQ or CONNECT_IND window); `rxCback` with status success, CRC failed or timeout. | as above |
| `void PalBbBleCancelData(void)` / `PalBbBleCancelTifs(void)` | loop and isr | Abort a pending operation / the pending T_IFS operation. | G stop sequence |
| `void PalBbBleEnableDataWhitening(bool_t enable)` / `PalBbBleEnablePrbs15(bool_t enable)` | loop, from `BbBleInit` and the test commands | Test modes; `enable` TRUE is the normal state for whitening. May be a no-op first. | `FORMAT` bits in K (bit 13) [measure on FM-1] |

The Tx and Rx callbacks must be called with the controller's critical section **not** held, from the baseband
interrupt; they call `WsfSetEvent` and read `PalBbGetCurrentTime`. `PalBbBleSetChannelParam` and `SetDataParams` are also
called from the loop while the baseband is idle (the next event is prepared ahead of time).

## pal_timer.h (3)

| Function | Context | What the controller expects | Radio facts |
| --- | --- | --- | --- |
| `void PalTimerInit(PalTimerCompCback_t expCback)` | loop, once, at `SchInit` | Remember the callback; the controller's scheduler timer. | D.2 (a compare against the slot counter) [measure on FM-1] |
| `void PalTimerStart(uint32_t expUsec)` | loop and isr | One-shot, `expUsec` microseconds from now (relative); then `expCback()` from interrupt context. The controller starts it about `schSetupDelayUsec` before the next operation is due, so the latency of the interrupt must be smaller than that (500 us on the reference board). The host test must fire it **at** the deadline (a late fire makes the controller drop the operation: "head element in the past"). | as above |
| `void PalTimerStop(void)` | loop and isr | Cancel the pending one. | as above |

## pal_crypto.h (2)

| Function | Context | What the controller expects | Radio facts |
| --- | --- | --- | --- |
| `void PalCryptoAesEcb(const uint8_t *pKey, uint8_t *pOut, const uint8_t *pIn)` | loop | AES-128 ECB encrypt, little-endian byte order as in `pal_crypto.h`. Called only for resolving private addresses (`bb_ble_reslist.c`): with a public address and no privacy it is not reached, but it must link. BearSSL's AES is in `firmware/wifi/bearssl/`. | none |
| `void PalCryptoGenerateRandomNumber(uint8_t *pBuf, uint8_t len)` | loop | `len` random bytes (LE Rand, address generation, the controller's own seeds). `firmware/hal/fm1_rand.h` has OPLBOX's source. | 0x13B00 is read by JieLi's `pseudo_random_genrate` [unchecked] |

## pal_radio.h (4)

| Function | Context | What the controller expects | Radio facts |
| --- | --- | --- | --- |
| `int8_t PalRadioGetActualTxPower(int8_t txPwr, bool_t compFlag)` | loop | The power in dBm the radio will really send for the requested level (with the RF path compensation when `compFlag`). | E.7 (levels 0..`rf.field0 >> 12`); the dBm per level [measure on FM-1] |
| `void PalRadioGetSupTxPower(int8_t *pMin, int8_t *pMax)` | loop | The range of levels the radio can send (LE Read Transmit Power). | as above |
| `void PalRadioReadRfPathComp(int16_t *pTx, int16_t *pRx)` | loop | The RF path compensation in 0.1 dB (LE Read RF Path Compensation); zeros are valid. | none in the IR |
| `bool_t PalRadioWriteRfPathComp(int16_t tx, int16_t rx)` | loop | Store it; TRUE when accepted. | none in the IR |

## pal_sys.h (6)

| Function | Context | What the controller expects | Radio facts |
| --- | --- | --- | --- |
| `void PalEnterCs(void)` / `void PalExitCs(void)` | loop and isr | A critical section against everything that calls controller code: mask the baseband interrupt and the scheduler timer's (the CPU's interrupt mask is enough; `cordio_wsf.c` nests them with a counter, so these see one Enter and one Exit). | D.4: IRQs 45 and 29, priority 2 |
| `void PalSysAssertTrap(void)` | loop | A failed `WSF_ASSERT` (compiled out here: `WSF_ASSERT_ENABLED=0`); stop or count. | none |
| `uint32_t PalSysGetAssertCount(void)`, `uint32_t PalSysGetStackUsage(void)`, `void PalSysSetTrap(bool_t)` | loop | Vendor-specific HCI commands (`lhci_cmd_vs.c`) that report these; return 0 and ignore. | none |

## Totals and what the host test showed

37 functions: 7 + 15 + 3 + 2 + 4 + 6 (36 reached by the link). Of them the advertiser needs real behaviour from: `PalBbInit/Enable/Disable`,
`PalBbGetCurrentTime`, `PalBbBleInit/Enable/Disable`, `PalBbBleSetChannelParam`, `SetDataParams`, `SetOpParams`,
`PalBbBleTxData`, `PalBbBleCancelData`, `PalTimerInit/Start/Stop`, `PalEnterCs/ExitCs`, `PalBbLoadCfg` and
`PalCryptoGenerateRandomNumber`. Connectable advertising and a connection also need `PalBbBleRxTifsData`,
`PalBbBleTxTifsData`, `PalBbBleRxData`, `PalBbBleCancelTifs`. The rest can return constants first.

With `pal_stubs_host.c` and the timer fired at its deadline, enabling advertising (HCI start-up, the four advertising
commands) produces: one `PalTimerStart`, then at the deadline `PalBbBleSetChannelParam` (channel 39, the access address
0x8E89BED6, CRC init 0x555555), `SetDataParams`, `SetOpParams` and one `PalBbBleTxData` of an 11-byte PDU: header
`00 09` (ADV_IND, 9 bytes), AdvA, `02 01 06`. After that the controller waits for the Tx callback, which the stub never
calls: the first thing the real driver adds is that interrupt.
