# Cordio's LE controller on the FM-1: what is built, what is tested, what is missing

OPLBOX is GPL-3.0-only and ships no JieLi binary for Bluetooth. The controller is Arm/Packetcraft Cordio's LE link layer
(Apache-2.0, <https://github.com/packetcraft-inc/stacks>, commit 3656312, 2020-11-11), vendored unchanged in
`firmware/ble/cordio/` (`README.md` there, `SHA256SUMS`, checked by `tests/run_host.sh`). The host above it is Apache NimBLE
(`firmware/ble/nimble/`, same rules). Between Cordio and the chip is the PAL, Cordio's hardware interface: the chip's PAL is
`firmware/ble/cordio_port/pal_fm1.c` (below). Nothing in this document ran on an FM-1.

## What was taken and how it is configured

- 68 sources (`firmware/ble/cordio_port/SOURCES.txt`): the baseband manager's advertising-slave and connection-slave
  operations, the LL's link control for legacy advertising and one slave connection, the scheduler, the LHCI command
  handler, the LL's support code, and WSF's queue, message and buffer utilities. Not compiled: scan, initiator and master,
  extended and periodic advertising, ISO, encryption, PAST, DTM-only paths and the host.
- `firmware/ble/cordio_port/DEFINES.txt`: `INIT_BROADCASTER`, `INIT_PERIPHERAL`, Bluetooth 4.2, `CHCI_TR_CUSTOM=1` (the HCI
  transport is ours), WSF's asserts, statistics and trace off. One connection, three TX and three RX data buffers, the
  controller's heap 3 KB (`CORDIO_HEAP_BYTES`, in the pool) and its message pools of 16, 32, 64, 128 bytes and the ACL size.
- Our code, GPL-3.0-only: `cordio_wsf.c` (WSF's timers from the main loop's millisecond clock, events and messages dispatched
  from the loop, a static heap, critical sections as no-ops: one context), `cordio_ctrl.c`/`.h` (start-up, the HCI function
  pair to the host in the shape of `firmware/ble/hci.h`'s transport, the poll), `pal_stubs_host.c` (host stubs), `pal_fm1.c`
  (the chip's PAL).
- The compile for the FM-1 CPU needs `-fshort-enums` (Cordio's enum-typed arguments must match its `uint8_t` definitions); the
  build passes it for this group only.

## The host test (`tests/ble_cordio.c`, 40 checks, on the host stubs)

The HCI Reset, Set Event Mask, LE Set Event Mask, LE Read Buffer Size, Read BD_ADDR (the address the port set, least
significant byte first), Read Local Version and Supported Commands, LE Read Local Supported Features are answered with
Command Complete and status 0; an unknown vendor command is refused (0x01) and LE Set Advertising Parameters with too few bytes
(0x12); LE Set Advertising Parameters, Data, Scan Response Data and Enable are accepted; a malformed command and a
packet type the controller does not take are dropped and counted; an event the host's transport refuses is offered again
and arrives. Then the controller asks the baseband, through the PAL, to transmit: an advertising PDU on an advertising
channel with the advertising access address 0x8E89BED6 and CRC init 0x555555, header ADV_IND, length data + 6, our address,
and exactly the host's data.

**Found by the PAL's test, fixed:** with one 128-byte message buffer (the number of advertising reports) LE Set
Advertising Parameters, a 40-byte message on the host, found no buffer and was dropped while the command still answered
success, so advertising ran at the default 1.28 s; a connection's 96-byte set-up message would have been lost the same way.
The pools now hold four 64-byte and four 128-byte buffers. On the stubs the first transmit came seconds after the enable
and nothing followed a hand-made completion; with the PAL Cordio advertises at the interval set and goes on after every
completion. Why the stub run stalled was not looked into further.

## The chip's PAL (`pal_fm1.c`)

`firmware/ble/cordio_port/PAL_NEEDED.md` lists the 37 functions and what Cordio expects of each; `pal_fm1.c` has all of them.
Cordio hands the baseband one packet at a time with an absolute time; the baseband runs whole events from a parameter
block. The PAL maps Cordio's operations onto hardware events through the driver's one-packet calls (`fm1ble.h` "for
Cordio's PAL"):

- an advertising event (`PalBbBleTxData` on 37, 38 and 39, one call per channel: `bb_ble_adv_slave.c`): the first of
  Cordio's packets arms one hardware advertising event over all three channels, armed exactly as the driver's own
  advertising (`fm1ble.c` `adv_arm`: hop increment 1, map 37-39, first event in the next slot with no bit offset, the
  local address from AdvA, FORMAT bit 2 last), the PDU in TX buffer 0 at the slot TXTOG names. Cordio's packets on the
  other two channels go in that event (`adv_covered`, counted as `pal_adv_covered`): they complete when it ends, at once
  if it already has. A request (SCAN_REQ, CONNECT_IND) received in the event completes the transmit and goes to Cordio's
  receive callback. Each event is armed with Cordio's scan response already in TX buffer 1, in the other TX slot, so the
  hardware can answer a SCAN_REQ by itself: the one in Cordio's advertising operation in progress (`BbGetCurrentBod`,
  `pTxRspBuf`, the buffer `bb_ble_adv_slave.c` hands to `PalBbBleTxTifsData`), read when the event is armed. Cordio puts a
  scan response the host changed into that operation between events, so the next event answers with it; nothing is kept
  from an earlier event. The SCAN_RSP Cordio gives after the request (`PalBbBleTxTifsData`) goes into TX buffer 1 for the
  rest of the event; a CONNECT_IND stops the event. No request: the
  event's end completes the transmit and closes the receive window as a timeout. Until 122afe3 the PAL armed one
  one-channel event per packet (hop 0, a map of that channel alone): on the FM-1 none of those reached a central, and the
  ones on 38 and 39 never ended (docs/BLUETOOTH.md "The second FM-1 run").
- a connection event as the peripheral (`PalBbBleRxData` on 0-36): state 7 on a fixed channel (the one Cordio computed with
  CSA #1) and the window Cordio asks for. The master's packet goes to Cordio's receive callback, the response Cordio gives
  goes into TX buffer 0, in both TX slots (the hardware sends the one TXTOG names), for T_IFS; the event's end completes it. One exchange per event; a second receive in the event
  times out.
- time: `PalBbGetCurrentTime` is the port's microsecond clock (TIMER4 on the FM-1). A connection event is armed (due -
  now) from now, in 625 us slots and a bit offset; an advertising event in whole slots (Cordio loads it 300 us ahead: the
  next slot). A received packet's timestamp, which Cordio uses for a connection's anchor, is the
  time the PAL saw it minus its time on the air. The scheduler's timer is a deadline checked by `pal_fm1_service`.
- an operation the baseband never ends ends by its deadline (its start, the window, two packets, 5 ms; an advertising
  event's window is 25 ms for its three channels and the hardware's random delay), so Cordio is never left waiting.
- a connection event is waited through (since 58cadc5's run): once it is armed, `pal_fm1_service` polls the baseband every
  2 us until its packet comes or it ends, so the packet's timestamp is that close and Cordio's response goes in at once.
  At 58cadc5 the PAL ran only when the main loop came back: the FM-1 received the first packet of a connection (inside
  the wide transmit window), Cordio put the next anchor as late as the loop had been, and the next 17 windows (286 us)
  missed the central (`pal_conn_ops` 18, `pal_conn_rx` 1; xlap's BlueZ dropped it after 0.56 s). The wait costs the main
  loop up to Cordio's 300 us load delay plus the window, every connection event (`pal_busy_us_max`). The scheduler's timer
  is still polled: much later than 300 us (`pal_timer_late_us`; 215 at most at 58cadc5) and Cordio skips the event.
- where the window lands against the packet is not settled on the chip: the driver has three setups
  (`fm1ble.h` `FM1BLE_CONN_*`): keep (as up to 58cadc5: WIDEN0/WIDEN1 at the open's values, 487 us of widening the chip may
  or may not apply), pad (no widening, 625 us earlier and 1250 us longer: covers a slot boundary of the baseband's own) and
  widen (JieLi's `le_hw_ioctrl` shape: the counter at Cordio's anchor, WIDEN half the window), wide (as pad, 1250 us each
  side), and since 5474159 three on pad's shape that each add one fact of the IR: padj (the counter as `__set_anchor_cnt`
  writes it, the slots minus one), padb (and the bit offset with bit 15, as `__hw_set_widen_drift` writes it), padw (and
  the window in WINCNTL0/1, where `__hw_set_widen_drift` puts a connection event's). Each connection runs one for all its
  events, the next connection the next one (`PAL_FM1_SETUP_ORDER`: padw, padb, padj, pad, wide, keep, widen); a setup a
  connection received 8 packets on is kept for every later connection. Up to a7c1738 a missed event moved to the next
  setup, up to 5474159 three in a row did; at 5474159 that left pad, which had received, before the anchor settled.
  `pal_setN_counts` and `pal_setN_arr` say what each received and where in its window the packets started, `connN_*`
  what each connection did, and the test run's window probe where the chip ends each setup's window with no central.

**What the first run has to show.** The response must be in its buffer within T_IFS (150 us) of the packet's end, and the
timestamp is right only when the PAL runs at the packet's end: both need `pal_fm1_service` in the receive interrupt (IRQ
29) or, for connection events, the busy wait above (`pal_resp_us_max` says how long Cordio took). This build attaches no
interrupt: everything runs from the main loop, so on the FM-1 a SCAN_RSP is expected to come too late (the PAL counts
them, `resp_late`, and tells Cordio the transmit failed; the scan response the event was armed with goes out from TX
buffer 1 if the hardware sends it by itself). The host tests call the PAL
at once, as an interrupt within T_IFS would. Also unchecked: where the hardware's slot boundaries lie against the arming
write (the model takes one at the write), state 7 with a fixed channel, how TXTOG changes in a connection, the data PDU header
words, the receive window's unit, the RSSI (not read: Cordio is told 127, not available).

**Tests.** `tests/ble_pal.c` (78 checks; `tests/ble_pal_mutants.py` catches 53 deliberate bugs in the PAL and the driver's
operations; the model can count a slot more than op_arm writes, take the bit offset only with bit 15, and take the
window from WINCNTL0 (`bb_model.h` cnt_jieli, bit15, win0), so each new setup is shown to receive on the chip it is for; the model sends a connection response from the TX slot as it is T_IFS after the packet, and a connection polled
every 200 us, as on the FM-1, misses most events without the busy wait and none with it; with the padded window alone,
each event armed under 625 us before its due time opens the window at once, and the PAL's arrival positions must match
where the model opened it) and `tests/ble_stack.c` (22 checks): NimBLE over Cordio over the PAL with a central doing L2CAP and ATT; with
`run`, the firmware's test run on the same air, every step PASS (docs/BLUETOOTH.md "Cordio path as built" 4 and 5).

## Size (tools/build.py, JieLi's clang at `-Os -ffunction-sections`, numbers from the build)

| Piece | Code + read-only | Static RAM | How |
| --- | --- | --- | --- |
| NimBLE host, port, glue, the driver, the stack's glue and the test run | 55,626 B | 14,403 B | object sizes, unlinked: an upper bound |
| Cordio controller (70 objects with the glue) | 54,842 B | 475 B | object sizes, unlinked; its 3 KB heap is in the pool |
| The chip's PAL (`pal_fm1.c`) | 1,334 B | 412 B | object size |
| The app with all of it (WiFi stages 4-9, the pages, the stack) | 525,904 B of the 581,564 B slot | 93,764 B of 98,304 B | `./build.sh` |

The two heaps (Cordio's and `ble_libc.c`'s, 3 KB each) live in the pool, app.ld's zeroed region outside the 96 KB of
`.data` and `.bss`; the baseband's RAM block stays in `.bss`, NimBLE's buffers (8 mbufs of 224 B) too.
