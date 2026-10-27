# The Bluetooth test page

Step 5 of docs/BLUETOOTH.md "Cordio path as built": a page on the FM-1, a USB SysEx report and `tools/fm1_ble_report.py`,
so the control session reads what the Bluetooth stack did on the device without a photo, one result line per step with its
raw evidence. It follows the WiFi test page (docs/WIFI_STAGE4.md, docs/WIFI_STAGE8.md) in everything but the radio.

**Nothing here has run on an FM-1.** The page, the report and the tool are tested on the host (`tests/run_host.sh`)
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

Twelve pages, PLAY turns them, the last one back to the first:

| Page | Shows |
| --- | --- |
| 1 | the eight steps, one line each: name, PASS / FAIL / SKIP (`--` before it ran), the first 20 characters of the note |
| 2-9 | one step each: the result and its time in ms, the note (two lines of 32 characters), how many bytes of evidence it kept and the first 40 of them, 10 to a line |
| 10-12 | the 24 counters, 8 to a page, as 32-bit hex |

A step that cannot run because an earlier one failed is SKIP, with the note `skipped: an earlier step failed`; with a
wait of 0 the connect and MIDI steps are SKIP with `skipped: no wait for a central`. SAVE during a run leaves at once and
the report holds what the steps had by then (the others NOT RUN).

## The steps and their evidence

In the order they run (`fm1ble_test.h`). A failed step says in its note what the next fix needs.

| Step | Passes when | Evidence (raw bytes in the report) |
| --- | --- | --- |
| `radio` | the Bluetooth side of the radio powered up and the registers read back (docs/BLUETOOTH_RADIO.md B-D) | register readbacks, each a 32-bit little-endian word |
| `block` | the baseband's RAM block is set up and the baseband started without an interrupt storm | the registers read back after the start |
| `hci` | the host reset the controller and read its version, public address and LE buffer size | the HCI events' bytes as received: Command Complete for Reset, Read Local Version, Read BD_ADDR, LE Read Buffer Size |
| `adv_set` | the controller accepted the four advertising commands (parameters, data, scan response, enable) | the events' bytes if one was refused |
| `adv_air` | the baseband's interrupt counter grew over a second of advertising | counters `bb_irq`, `bb_irq_tx`, `adv_events` |
| `connect` | a central connected within the wait | the LE Connection Complete event's bytes; the counters `interval`, `latency`, `timeout`, `att_mtu` |
| `midi_in` | a BLE-MIDI packet the central wrote reached the app's MIDI input | the packet's bytes |
| `midi_out` | a notification went out and the controller handed its buffer back | the notification's bytes; counter `nocp` |

`fm1ble_ctr_name` / `tools/fm1_ble_report.py` name the counters: `radio_regs_ok`, `radio_regs_bad`, `bb_irq`, `bb_irq_tx`,
`bb_irq_rx`, `hci_cmds`, `hci_events`, `hci_bad`, `hci_status` (the last non-zero HCI status), `hci_op` (its opcode),
`adv_events`, `connects`, `disconnects`, `acl_in`, `acl_out`, `att_mtu`, `interval`, `latency`, `timeout`, `midi_in`,
`midi_out`, `notify_failed`, `nocp`, `reserved`. `hci_status` and `hci_op` print in hex.

## The report and the tool

The layout is in docs/API.md "Bluetooth test page over SysEx" (commands `0x65` report, `0x66` run). The text is the pages'
text, then every step's whole note, then the controller's address; then the counters and the steps' evidence as bytes. The tool
prints the text, the counters and one line per step with its evidence in hex. `--out DIR` writes `report.txt`, `report.bin`
(the report as sent), `counters.txt` and `step-N-<name>.bin` for each step that kept evidence, so an HCI trace or a register
list can be diffed against `tests/ble/session_expect.txt` or docs/BLUETOOTH_RADIO.md without a photo.

## What is tested, and what is not

On the host (`tests/run_host.sh`): the SysEx commands against `ble_sysex.c` (statuses, the wait byte, chunking, EDIT),
the page's lines for a run with no central, with one, with a failed early step (the rest SKIP) and with no wait, the report's
text equal to the pages' text, the tool's `--run`, `--read`, `--wait`, `--out` and every refusal, and the decoder on broken
reports. **Not tested: any real run.** The run is a fake; the page has not been drawn on the FM-1's screen (the 32-character
lines were chosen from the WiFi page's longest lines); `HOME` on the WiFi page has not been pressed on hardware; the audio
muting around the radio start copies the WiFi page's and is as untested for Bluetooth as WiFi's was before its first run.
