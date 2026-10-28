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
  what each connection did. f0e6d08's window probe (connection events with no central) showed that only padw's window
  follows the register (its end moved with WINCNTL0: 449..811 us at 60 us, 3019..3709 at 2500), so the stack runs padw
  alone since then (fm1ble_stack.c), and the probe is gone.
- **The TX side** (fm1ble.h FM1BLE_TX_*, since f0e6d08). At f0e6d08 (oplbox-ble-run-f0e6d08-1) padw's first connection
  received its first three events: the second's packet came, Cordio's response went in (up to 251 us after a packet:
  T_IFS is 150), and the hardware neither sent (TXTOG unchanged) nor ended the event; the PAL ended it by its deadline and
  armed the next, and 364 us after that arm a packet flag came, 1192 us before that event's dueUsec and 567 us before
  its window could open. Cordio took it as the anchor and every later window of the connection was 1.2 ms early. The IR
  (docs/BLUETOOTH_RADIO.md E.4, "the TX slots in a connection") says the hardware keeps per-slot state in TXDHDR bit 2 and
  that JieLi fills the slots ahead of the event, where the driver wrote Cordio's NESN over bit 2 in both slots after the
  packet. Three guards since then, on every connection: a packet flag that starts `PAL_FM1_STALE_US` (400 us) or more
  before the window could open is dropped (`pal_stale_rx`), not given to Cordio; an event ended by its deadline is halted
  (fm1ble_op_halt: the counter off and the wait for idle that ble_hw_disable does, at most 2 ms; `pal_halts`); and the
  event log and the summaries carry the TX words (TXDHDR0/1, TXTOG, RXTOG, TXBUF0/1CNTL) and Cordio's response time.
  Three TX modes, one per connection in the order intime, jieli, air (`PAL_FM1_TX_ORDER`, kept like the setup once a
  connection receives 8 packets): air as up to f0e6d08; jieli as the IR (the CONNECT_IND's slot words, a response in both
  slots with bit 2 kept, the SN in bit 3, the MD in bit 4); intime as jieli, an empty PDU repeating the last SN in both
  slots at each arm, and Cordio's response put in only within `PAL_FM1_TIFS_US` (100 us) of the packet, so nothing is
  written while the hardware may be turning around (Cordio is told the transmit failed; it sends the PDU again in the next
  event, since the central does not acknowledge a repeated SN). On the register model (which puts in NESN itself in these
  tests, `hw_nesn`) every mode carries data both ways, a stale flag leaves the anchor where it was, a late response is left
  out and data still flows, and a hung event is halted with the connection going on.
  **Open:** on the model, a connection that follows one whose event ended by its deadline, or one with a left-out late
  response, receives and acknowledges the central's packets but does not pass its first ATT write to the host; it happens
  with fm1ble_op_stop as up to f0e6d08 too, and with the left-out response reported as sent [unexplained]. A run whose
  later connections stay up and resolve GATT but never get MIDI in may be this. In round 4's test order (more connections
  before it) the hung event's connection and the one after it both pass the ATT write, with the model's central as it was
  and as round 4 changed it: not reproduced, cause still unknown. (Round 4's change to the central: it sent a newly queued
  PDU under the SN of one not yet acknowledged, which a receiver drops as a repeat; it now sends the same PDU until it is
  acknowledged, Vol 6 Part B 4.5.9.)
- **a349877, the done-check after round 3 (2026-10-28):** two connections stayed up 6.6 and 10 s on intime and received
  462 packets, and BlueZ never resolved GATT. The IR answers why: `__hw_tx_buf_proess` sets its header bit 9 when the TX
  queue is not empty, which is MD, and `ble_hw_tx` puts that bit in TXDHDR bit 3; so TXDHDR (and RXDHDR, which the receive
  path reads the same way) is the chip's layout, not the air's, and the chip keeps SN and NESN itself (bit 2 the slot's
  SN, the slot TXTOG names sent until the central acknowledges it) [unchecked on the chip]. The PAL handed Cordio RXDHDR
  bits 4-2 as MD, SN and NESN: Cordio read the central's MD (0) as its SN, took the first packet (LL_FEATURE_REQ) and
  dropped every later one as a repeat. The fourth TX mode, arq (fm1ble.h FM1BLE_TX_ARQ, first in `PAL_FM1_TX_ORDER`),
  lets the chip keep them: a response goes only into the slot TXTOG names, from that slot's own buffer, with bit 2 kept
  and MD 0; the other slot gets an empty PDU at every arm; and the PAL gives Cordio an SN and NESN of its own making, from
  Cordio's last response and the chip's toggles (pal_fm1.c "ARQ"). A mode is now kept only once Cordio took an L2CAP PDU
  on it. The header trace (`trC_N`, LINK LAYER in the report) records, per packet, the chip's header bits and toggles and
  what Cordio was told and answered, so the run says which reading of the bits holds. On the register model with the
  chip's own ARQ (`hw_arq`) arq answers LL_FEATURE_REQ once and passes the ATT write with a lost response, a lost feature
  response and a late one, and intime, jieli and air receive every packet and pass nothing, as a349877 did.
- **1b251ea, the done-check after round 5 (2026-10-28, `~/oplbox-ble-runs/1b251ea-1` on xryzen):** on padw the
  connection timing works (869 of 895 events received), but no HCI ACL went from Cordio to NimBLE, so GATT never
  resolved. The header trace shows why: BlueZ's LL_LENGTH_REQ came with RXDHDR bits 4-0 `1F` and its first L2CAP PDU
  (`02`) next, in the same connection event (that event ended about 500 us later than the others, one more exchange).
  The chip's ARQ acknowledged the L2CAP PDU on the air, but the PAL told Cordio MD 0, so Cordio had closed the event and
  never got the PDU; and since the PAL makes Cordio's SN and NESN from the toggles, the next packet looked new to Cordio
  and nothing showed the loss. The same trace reads RXDHDR as the air's layout, not the chip's: bits 3 and 2 toggle on the
  central's alternating empty PDUs (`0D`, `01`), so they are SN and NESN, and the LL_LENGTH_REQ that had a second packet
  after it has bit 4 (MD) set [one run; the IR reading above says MD is in bit 3]. Round 6: in arq the PAL tells Cordio
  MD 1 on every packet, so Cordio keeps a receive buffer for a later packet in the event whichever bit is MD (an event
  with none ends with Cordio's receive timing out at the event's end), and in every mode Cordio's response is complete
  when the central's next packet comes, not only at the event's end, so each packet of the event reaches Cordio in turn.
  `pal_arq_dup` and `pal_arq_unwritten` carry, in bits 31-16, the packets after the first in an event and the ones given
  to Cordio (LINK LAYER in the report). On the model the central can send its queue in one event (`air_central.h` burst,
  `bb_air` conn_next): with the PAL as at 1b251ea the ATT write after LL_FEATURE_REQ (MD set) is lost, as on the FM-1.

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
