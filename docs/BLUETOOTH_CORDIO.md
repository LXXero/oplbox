# Cordio's LE controller on the FM-1: what is built, what is tested, what is missing

OPLBOX is GPL-3.0-only and ships no JieLi binary for Bluetooth. The controller is Arm/Packetcraft Cordio's LE link layer
(Apache-2.0, <https://github.com/packetcraft-inc/stacks>, commit 3656312, 2020-11-11), vendored unchanged in
`firmware/ble/cordio/` (`README.md` there, `SHA256SUMS`, checked by `tests/run_host.sh`). The host above it is Apache NimBLE
(`firmware/ble/nimble/`, same rules). Between Cordio and the chip is the PAL, Cordio's hardware interface; the chip's PAL is
the one piece still to be written (below). Nothing in this document ran on an FM-1.

## What was taken and how it is configured

- 68 sources (`firmware/ble/cordio_port/SOURCES.txt`): the baseband manager's advertising-slave and connection-slave
  operations, the LL's link control for legacy advertising and one slave connection, the scheduler, the LHCI command
  handler, the LL's support code, and WSF's queue, message and buffer utilities. Not compiled: scan, initiator and master,
  extended and periodic advertising, ISO, encryption, PAST, DTM-only paths and the host.
- `firmware/ble/cordio_port/DEFINES.txt`: `INIT_BROADCASTER`, `INIT_PERIPHERAL`, Bluetooth 4.2, `CHCI_TR_CUSTOM=1` (the HCI
  transport is ours), WSF's asserts, statistics and trace off. One connection, three TX and three RX data buffers, the
  controller's heap 2 KB (`CORDIO_HEAP_BYTES`).
- Our code, GPL-3.0-only: `cordio_wsf.c` (WSF's timers from the main loop's millisecond clock, events and messages dispatched
  from the loop, a static heap, critical sections as no-ops: one context), `cordio_ctrl.c`/`.h` (start-up, the HCI function
  pair to the host in the shape of `firmware/ble/hci.h`'s transport, the poll), `pal_stubs_host.c` (host stubs).
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

**Open, found by the test and not explained:** on the stubs the first transmit is asked for about five seconds after
the enable (the scheduler's first event is that far off on this stub clock; the reason was not found, it may be the stub
clock or Cordio's start-up spacing: to be measured with the chip's PAL), and after the stub completes a transmit by hand
(`tx_cback(0)`) the controller did not ask for the next channel within 8 s, and it did not answer Advertising Enable (off)
while an operation was in flight. Those three are not asserted; they are the first thing to look at with a real PAL, which
completes operations and has a real clock.

## What is missing: the chip's PAL

`firmware/ble/cordio_port/PAL_NEEDED.md` is the exact list: 36 functions in six of Cordio's PAL headers (`pal_bb.h`,
`pal_bb_ble.h`, `pal_timer.h`, `pal_crypto.h`, `pal_radio.h`, `pal_sys.h`) and the one `PalSysAssertTrap` the port's
`WsfAssert` calls, with what the controller expects of each and which radio facts (docs/BLUETOOTH_RADIO.md) fill it.
The baseband is a per-link engine that runs whole advertising and connection events itself, while Cordio hands it one
packet at a time with an absolute time: the PAL is a translation (the parameter block and indexed registers from the
packet, the baseband's one interrupt per event or received packet into Cordio's completion callbacks). The facts in
docs/BLUETOOTH_RADIO.md settle advertising on paper; the receive timing (the 150 us turnaround), the free-running
microsecond counter Cordio wants, and the connection setup are what the first FM-1 run has to measure.

Because of that, this build's test run (docs/BLUETOOTH_PAGE.md) advertises **straight from the driver**
(`firmware/ble/fm1ble.c`), not through Cordio: radio, block, advertising, the baseband's event flags, and the SCAN_REQ and
CONNECT_IND PDUs the radio takes in. The HCI, connect and MIDI steps report SKIP or FAIL with the reason. NimBLE on Cordio
over HCI is tested on the host (`tests/ble_nimble.c` against the fake controller, `tests/ble_cordio.c` for Cordio alone),
not yet NimBLE on Cordio together: there is no test that joins them.

## Size (tools/build.py, JieLi's clang at `-Os -ffunction-sections`, numbers from the build)

| Piece | Code + read-only | Static RAM | How |
| --- | --- | --- | --- |
| NimBLE host, port, glue, the driver and the test run | 52,368 B | 14,047 B | object sizes, unlinked: an upper bound |
| Cordio controller (70 objects with the glue) | 54,834 B | 2,523 B | object sizes, unlinked; its 2 KB heap is in the RAM figure |
| NimBLE + Cordio + C library + the stub PAL, `--gc-sections` | 69,606 B | 19,588 B | linked, driver and test run left out (they are in the app) |
| The app today (WiFi, the page, the driver, the test run) | 416,156 B of the 581,564 B slot | 62,864 B of 98,304 B | `./build.sh` |

Carrying the host and the controller adds about 70 KB of flash and 19.6 KB of RAM: 486 KB of 581 KB, 82.5 KB of 96 KB. WiFi and
Bluetooth fit together; no separate Bluetooth image is needed. (The NimBLE host is not linked into the app yet: no
controller to run it on, so the app image above does not include the 70 KB.) The RAM figure holds NimBLE's mbuf pool
(8 blocks of 224 B), its HCI transport buffers and `ble_libc.c`'s 3 KB heap.
