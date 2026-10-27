# The Bluetooth test page

Step 5 of docs/BLUETOOTH.md "Cordio path as built": a page on the FM-1, a USB SysEx report and `tools/fm1_ble_report.py`,
so the control session reads what the Bluetooth stack did on the device without a photo, one result line per step with its
raw evidence. It follows the WiFi test page (docs/WIFI_STAGE4.md, docs/WIFI_STAGE8.md) in everything but the radio.

**One run on an FM-1 so far** (48ea034, docs/BLUETOOTH.md "The first FM-1 run"): the page, the report and the tool
worked over USB; the tool printed the counters under wrong names, fixed since. The page, the report and the tool are
tested on the host (`tests/run_host.sh`)
against a scripted run, `tests/ble/fm1ble_test_host.c`, whose numbers and bytes are made up. The run the firmware links
is `firmware/ble/fm1ble_test.c` behind `firmware/ble/fm1ble_test.h`; this file describes what each step is meant to
report, as the header fixes it. Where the real run differs, the header and this file change together.

## Running it

From the computer the FM-1 is plugged into, in the oplbox checkout:

```
python3 tools/fm1_ble_report.py --run --wait 30 --out ble-run
```

The tool starts a run (the sound stops; EDIT must be closed), waits for it, prints the report and writes it to `ble-run/`.
`--wait S` is how long a phone or a Mac has to connect once advertising is on (default 20 s, 0 to 120; 0 skips the
connect and MIDI steps): open a BLE MIDI app on it and connect to "OPLBOX" in that time. `--read` reads the last run's
report without starting one. Exit codes: 0 done, 1 the FM-1 refused or has no report, 2 bad arguments, 3 FM-1 not found,
4 no reply (a firmware without the Bluetooth report), 5 the run did not finish within `--timeout` (default: the wait plus 60 s).

By the buttons: SAVE opens the WiFi page, **HOME there opens the Bluetooth page** (the WiFi page's other keys are
unchanged; SAVE on the Bluetooth page returns to the WiFi page's first screen, SAVE again leaves). PLAY starts the run
(20 s wait), then turns the pages. A run asked for over USB opens the page and starts as PLAY would.

## The page

Thirteen pages, PLAY turns them, the last one back to the first:

| Page | Shows |
| --- | --- |
| 1 | the eight steps, one line each: name, PASS / FAIL / SKIP (`--` before it ran), the first 20 characters of the note |
| 2-9 | one step each: the result and its time in ms, the note (two lines of 32 characters), how many bytes of evidence it kept and the first 40 of them, 10 to a line |
| 10-13 | the 32 counters, 8 to a page, as 32-bit hex |

A step that cannot run because an earlier one failed is SKIP, with the note `skipped: an earlier step failed`; with a
wait of 0 the connect and MIDI steps are SKIP with `skipped: no wait for a central`. SAVE during a run leaves at once and
the report holds what the steps had by then (the others NOT RUN).

## The steps and their evidence

In the order the firmware's run (`firmware/ble/fm1ble_test.c`) runs them, which is not the table's: radio, block, then the
driver's own advertising in three air trials (adv_set, adv_air), then the stack on the radio of the trial that sent (hci,
connect, midi_in, midi_out). A failed step says in its note what the next fix needs.

The air trials, 3 s each, ADV_IND every 100 ms on 37-39 with the name in the scan response, each from its own static
random address so a central can say which reached the air: trial 0 C2:46:4D:31:00:10 (variant 0: without the shared
radio's init, TX level 6), trial 1 C2:46:4D:31:00:11 (variant 1: `fm1wifi_radio_up` first, level 6), trial 2
C2:46:4D:31:00:12 (variant 1, level 11, the highest). The stack advertises from C2:46:4D:31:00:01.

| Step | Passes when | Evidence (raw bytes in the report) |
| --- | --- | --- |
| `radio` | the Bluetooth side of the radio powered up and the registers read back (docs/BLUETOOTH_RADIO.md B-D) | eight register readbacks, each a 32-bit little-endian word |
| `block` | the baseband's RAM block's start address (0x2FC84) reads back and the baseband is idle; the end (0x2FCBC) is not required to read back (it read 0 on the first FM-1 run, and JieLi never reads it): the note says what it read | eight registers read back after it (0x28000, 0x28008, 0x28010, 0x28034, 0x2FC84, 0x2FCBC, 0x2FC80, busy) |
| `adv_set` | the driver armed its own ADV_IND (service UUID, the name in the scan response) | the advertising data |
| `adv_air` | in at least one trial the baseband raised 3 or more event flags **and** TXTOG changed (the hardware's record of a buffer sent) or a packet came in; the note lists the trials that did ("sent, trials 0 1 2"; "(re-armed)" when the driver had to arm each event). Event flags alone fail it: "events, but TXTOG never changed: nothing sent" | per trial its events, TXTOG changes and packets received (u16 each, 18 bytes), then the entity's TXTOG, RXTOG, ANCHOR, BITOFF, EVTCOUNT, LASTCHMAP, RXSTAT0, FORMAT after the last trial (u16 each): the words the hardware writes. `fm1_ble_report.py` prints them as AIR TRIALS |
| `hci` | NimBLE's start-up went through Cordio and the PAL and ended with advertising on (via Cordio) | Cordio's commands, events, heap used, NimBLE's fail code, refused sends (u32 each), the address |
| `connect` | a central connected within the wait | interval, latency, timeout, ATT MTU (u32 each); on a failure, the last received PDU's first bytes |
| `midi_in` | a BLE-MIDI write from the central reached the MIDI input (`ble_midi_get`) | the message (3 bytes), the writes counted |
| `midi_out` | OPLBOX's notification of a Note On (90 3C 64) was acknowledged: Cordio sent Number Of Completed Packets | notified, NOCP events (u32 each) |

The counters (`fm1ble_ctr_name`, `FM1BLE_CTR_NAMES` in fm1ble_test.h, the tool's list held equal to it): the driver's
`radio_regs_ok`, `radio_regs_bad`, `variant` (the radio the stack runs on), `rf_common`, `events` (event flags since the
last power-up), `tx_flips` (events after which TXTOG had changed), `rx_packets`, `rx_scan_req`, `rx_conn_ind`, `rx_other`,
`pending_last`, `bb_err`, `air_trials` (bit n: trial n showed it sent), `txtog`, `rearm_used`, `adv_err`, `rx_hdr`,
`rx_stat`, `rx_type`, `rx_len`; the PAL's (`firmware/ble/cordio_port/pal_fm1.c`): `pal_adv_ops` and `pal_conn_ops`
(operations armed), `pal_rx_ok` and `pal_rx_timeout` (receive completions given to Cordio), `pal_resp_late` (responses asked
for after their event had ended, too late for T_IFS), `pal_stuck` (operations the baseband never ended), `pal_timer_late_us`
(the most the scheduler's timer fired late), `pal_done_37`, `_38`, `_39` (advertising operations that ended, per channel);
then `txpwr` (the stack's TX level) and `ram_end` (0x2FCBC as read back). The PAL runs from the main loop in this build: a
connection that fails with `pal_resp_late` above 0 needs the receive interrupt (docs/BLUETOOTH_CORDIO.md). The firmware at
48ea034 and before sent 24 counters, the PAL's two to a word; the tool reads such a report by its count, with those names.

The host build's scripted run (`tests/ble/fm1ble_test_host.c`) has the firmware's counter names and made-up values; only
the page and the tool are tested with it. The firmware's run is tested on the register model (`tests/ble_radio.c`) and on
its air with a central (`tests/ble_stack.c run`).

## A central on the bench

`tools/ble_central.py` is the other end of the run: a BlueZ central on a Linux computer near the FM-1 (`bluetoothctl`; for
the MIDI part BlueZ built with MIDI support, which makes an ALSA sequencer port for a BLE-MIDI device, and alsa-utils 1.2.11
or later for `aseqdump` and `aseqsend`). It scans (LE) and logs every advertisement from the trials' addresses and the
stack's with the time and RSSI, then connects to the stack, finds the new sequencer port, captures it with `aseqdump`,
sends a Note On (90 3C 64) with `aseqsend`, waits for the FM-1's Note On (the run's midi_out) and disconnects. From the
computer the FM-1 is plugged into:

```
python3 tools/fm1_ble_report.py --run --wait 30 --central xlap --out ble-run
```

starts it on xlap over ssh (the script goes on ssh's stdin; nothing to install there but BlueZ and alsa-utils), two
seconds before the run, and prints its findings after the report (CENTRAL: each address seen or not, connect, the port, the
Note On both ways), with `ble-run/central.json`. `--central local` runs it on the same computer. It is tested on stand-ins
for bluetoothctl, aseqdump and aseqsend (`tests/ble/fake_bluez/`, `tests/ble_central_tool.py`) made from those tools'
output formats: **not run against a real BlueZ yet**; bluetoothctl's interactive output is the part most likely to differ.

## The report and the tool

The layout is in docs/API.md "Bluetooth test page over SysEx" (commands `0x66` report, `0x67` run). The text is the pages'
text, then every step's whole note, then the controller's address; then the counters and the steps' evidence as bytes. The tool
prints the text, the counters, one line per step with its evidence in hex, and the air trials decoded. `--out DIR` writes `report.txt`, `report.bin`
(the report as sent), `counters.txt` and `step-N-<name>.bin` for each step that kept evidence, so an HCI trace or a register
list can be diffed against `tests/ble/session_expect.txt` or docs/BLUETOOTH_RADIO.md without a photo.

## What is tested, and what is not

On the host (`tests/run_host.sh`): the SysEx commands against `ble_sysex.c` (statuses, the wait byte, chunking, EDIT),
the page's lines for a run with no central, with one, with a failed early step (the rest SKIP) and with no wait, the report's
text equal to the pages' text, the tool's `--run`, `--read`, `--wait`, `--out` and every refusal, and the decoder on broken
reports. **Not tested: any real run.** The run is a fake; the page has not been drawn on the FM-1's screen (the 32-character
lines were chosen from the WiFi page's longest lines); `HOME` on the WiFi page has not been pressed on hardware; the audio
muting around the radio start copies the WiFi page's and is as untested for Bluetooth as WiFi's was before its first run.
