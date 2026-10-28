#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read the Bluetooth test page's results from an FM-1 over USB-MIDI, or start a run first.

  fm1_ble_report.py [--read] [--out DIR] [--port NAME]                 the last run's report
  fm1_ble_report.py --run [--wait S] [--timeout S] [--out DIR] [--port NAME] [--central HOST]
                                                        start a run, wait for it, then the report

SysEx in the updater's framing (firmware/src/ble_sysex.c, docs/API.md "Bluetooth test page over SysEx"). The report is
what the page's twelve pages show, as text, then the steps' full notes, then the counters behind pages 10-12 and each
step's raw evidence, as bytes (docs/BLUETOOTH_PAGE.md says what each step's evidence is). The tool prints the text and the
counters. With --out DIR it writes report.txt, report.bin (the report as sent), counters.txt and one file per step that
has evidence, step-N-<name>.bin (register readbacks as u32 little-endian, or HCI event bytes, as the step kept them), and
capture.bin, the PDU capture (firmware/ble/fm1ble_test.h FM1BLE_CAP_*: the bytes that went each way in the connections,
printed as PDU CAPTURE).

--run starts a run as SAVE, HOME, then PLAY would on the FM-1 (the sound stops; the page stays open afterwards, SAVE
leaves it). --wait S is how long a central has to connect once advertising is on (0-120 s, default 20; 0 skips the
connect and MIDI steps): connect to the FM-1 from a phone or a Mac (a BLE MIDI app) within that time. The FM-1 refuses
while EDIT is open. Finds the FM-1 the way fm1_install.py does. Needs mido with python-rtmidi.

--central HOST runs tools/ble_central.py on HOST (over ssh; "local" runs it here) while the run goes on: a BlueZ central
that scans for the run's advertisements (the three air trials' addresses, the four RX trials' and the stack's; its active
scan sends the SCAN_REQs the RX trials count), connects to the stack, sends
a Note On (90 3C 64) through the ALSA sequencer port BlueZ makes and captures the FM-1's notification. Its findings are
printed after the report and written to DIR/central.json. So one run on the bench checks all eight steps.

Exit codes: 0 done, 1 the FM-1 refused or has no report (no run since it started), 2 bad arguments, 3 FM-1 not found,
4 no reply (connection lost, or a firmware without the Bluetooth report), 5 the run did not finish within --timeout.
"""
import argparse
import io
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm1_install as fi  # noqa: E402
from fm1_slot import SlotError, message, wait_reply  # noqa: E402

REPORT, RUN = 0x66, 0x67
OK, E_REQ, E_NONE, E_RUNNING, E_EDIT = range(5)
STATUS = {E_REQ: "bad request", E_NONE: "no Bluetooth test run since the FM-1 started (--run starts one)",
          E_RUNNING: "a run is going on", E_EDIT: "EDIT is open on the FM-1: leave it (EDIT) and try again"}
STEPS = ["radio", "block", "hci", "adv_set", "adv_air", "connect", "midi_in", "midi_out"]
RESULTS = {0: "not run", 1: "pass", 2: "fail", 3: "skip"}
# the firmware's counters, in its order (firmware/ble/fm1ble_test.h FM1BLE_CTR_NAMES: tests/ble_report_tool.py holds the two
# lists equal)
COUNTERS = ["radio_regs_ok", "radio_regs_bad", "variant", "rf_common",
            "events", "tx_flips", "rx_packets", "rx_scan_req",
            "rx_conn_ind", "rx_other", "pending_last", "bb_err",
            "air_trials", "txtog", "rearm_used", "adv_err",
            "rx_hdr", "rx_stat", "rx_type", "rx_len",
            "pal_adv_ops", "pal_conn_ops", "pal_rx_ok", "pal_rx_timeout",
            "pal_resp_late", "pal_stuck", "pal_timer_late_us", "pal_done_37",
            "pal_done_38", "pal_done_39", "txpwr", "ram_end",
            "pal_adv_events", "pal_adv_covered", "pal_adv_ended", "pal_adv_conn_ind",
            "pal_conn_ended", "pal_conn_rx", "pal_refused", "evtcount",
            *[f"rxt{k}_{w}" for k in range(4) for w in ("counts", "stat", "pkt", "bytes")],
            "rx_stack_trial", "rx_agc_table", "rx_win", "agc_ctl",
            "agc_rssi", "conn_win", "pal_conn_timeout_us", "rx_stat1",
            "pal_busy_waits", "pal_busy_us_max", "pal_resp_us_max", "pal_conn_rx_bad",
            "pal_conn_setup", "pal_setup_moves", "pal_late_ops", "conn_flips",
            "pal_set0_counts", "pal_set1_counts", "pal_set2_counts", "conn_acks",
            "pal_set0_arr", "pal_set1_arr", "pal_set2_arr", "conn_rx_words",
            "pal_set3_counts", "pal_set3_arr", "pal_cind_gap_us", "pal_log_n",
            *[f"ev{k}_{w}" for k in range(12) for w in "abc"],
            *[f"pal_set{k}_{w}" for k in range(4, 7) for w in ("counts", "arr")], "pal_conns", "pal_good",
            *[f"conn{k}_{w}" for k in range(8) for w in "abc"],
            "nimble_conns", "pal_stale_rx", "pal_halts", "pal_resp_skipped",
            "pal_resp_hist", *[f"pal_tx{k}_counts" for k in range(3)], *[f"pal_tx{k}_sent" for k in range(3)],
            "pal_good_tx", "pal_conn_txs", "pal_first_rx_tx", "conn_txtog", "conn_txdhdr", "conn_txbuf",
            *[f"ev{k}_d" for k in range(12)], *[f"conn{k}_d" for k in range(8)], "pal_cfg", "pal_tx_order", "pal_resp_us_min",
            *[f"tr{c}_{n}" for c in range(2) for n in range(20)], *[f"conn{k}_ops" for k in range(4)],
            *[f"conn{k}_ll" for k in range(4)], "pal_tx3_counts", "pal_tx3_sent", "pal_arq_dup", "pal_arq_unwritten",
            "stack_acl", "stack_nocp", "nimble_mtu", "pal_rxtog"]
# up to a349877 the firmware sent the first 192 (no header trace, three TX modes); up to 122afe3 the firmware sent the first 32 of these (pal_done_37-39 then counted the driver's one-channel events
# that ended), up to 3b39ab4 the first 40, up to 58cadc5 the first 64, up to a7c1738 the first 80, up to 5474159 the
# first 120; a shorter report reads with the first names. f0e6d08 sent 192 with the window probe's 39 where the TX
# side's are now (COUNTERS_F0E6D08): its probe_delays word is always PROBE_DELAYS_F0E6D08, which tells the two apart
COUNTERS_F0E6D08 = COUNTERS[:153] + ["probe_n", *[f"clk_{w}_{i}" for w in ("idle", "arm") for i in ("0", "e", "7", "10")],
                                     *[f"pr{s}_{c}" for s in range(7) for c in "abcd"], "probe_delays", "probe_windows"]
PROBE_DELAYS_F0E6D08 = 1400 | 1800 << 16
# the 24 counters of the firmware at 48ea034 and before (the first FM-1 run); a pair is two 16-bit counts in one word, low
# then high, which the tool splits
COUNTERS_24 = ["radio_regs_ok", "radio_regs_bad", "variant", "rf_common", "events", "rearms", "rx_packets", "rx_scan_req",
               "rx_conn_ind", "rx_other", "pending_last", "bb_err", "events_v0", "events_v1", "rearm_used", "adv_err",
               "rx_hdr", "rx_stat", "rx_type", "rx_len", ("pal_adv_ops", "pal_conn_ops"), ("pal_rx_ok", "pal_rx_timeout"),
               ("pal_resp_late", "pal_stuck"), "pal_timer_late_us"]
HEX_COUNTERS = {"pending_last", "rx_hdr", "rx_stat", "air_trials", "txtog", "ram_end", "rx_win", "agc_ctl", "agc_rssi",
                "conn_win", "rx_stat1", *[f"rxt{k}_{w}" for k in range(4) for w in ("counts", "stat", "pkt", "bytes")],
                *[f"pal_set{k}_{w}" for k in range(7) for w in ("counts", "arr")], "conn_rx_words",
                *[f"ev{k}_{w}" for k in range(12) for w in "abc"], *[f"conn{k}_{w}" for k in range(8) for w in "abc"],
                "nimble_conns", *[f"clk_{w}_{i}" for w in ("idle", "arm") for i in ("0", "e", "7", "10")],
                *[f"pr{s}_{c}" for s in range(7) for c in "abcd"], "probe_delays", "probe_windows",
                "pal_halts", "pal_resp_hist", *[f"pal_tx{k}_{w}" for k in range(3) for w in ("counts", "sent")],
                "pal_conn_txs", "pal_first_rx_tx", "conn_txtog", "conn_txdhdr", "conn_txbuf",
                *[f"ev{k}_d" for k in range(12)], *[f"conn{k}_d" for k in range(8)], "pal_cfg", "pal_tx_order",
                *[f"tr{c}_{n}" for c in range(2) for n in range(20)], *[f"conn{k}_{w}" for k in range(4) for w in ("ops", "ll")],
                "pal_tx3_counts", "pal_tx3_sent", "stack_acl", "nimble_mtu", "pal_rxtog"}
# the air trials (fm1ble_test.h): the driver's own ADV_IND from these addresses, then the stack's
STACK_ADDR = "C2:46:4D:31:00:01"
TRIALS = [("C2:46:4D:31:00:10", "variant 0 (no shared radio init), level 6"),
          ("C2:46:4D:31:00:11", "variant 1 (shared radio init first), level 6"),
          ("C2:46:4D:31:00:12", "variant 1, level 11")]
# the RX trials (fm1ble_test.h): the same ADV_IND on variant 1 at level 6, each with its own receive setup
# the connection events' window setups (firmware/ble/fm1ble.h FM1BLE_CONN_*): up to a7c1738 the PAL moved to the next
# after each missed event, up to 5474159 after 3 in a row, in the order wide, pad, keep, widen; since then each
# connection runs one, the next connection the next, in the order padw, padb, padj, pad, wide, keep, widen (pal_fm1.h)
CONN_SETUPS = ["keep: as up to 58cadc5 (WIDEN at the open's 487 us)", "pad: no widening, 625 us earlier, 1250 us longer",
               "widen: JieLi's (at the anchor, WIDEN half the window)", "wide: no widening, 1250 us earlier, 2500 us longer",
               "padj: as pad, the counter as JieLi writes it (slots - 1)", "padb: as padj, the bit offset with bit 15",
               "padw: as padb, the window in WINCNTL0 too"]
SETUP_NAMES = ["keep", "pad", "widen", "wide", "padj", "padb", "padw"]
# the connections' TX modes (firmware/ble/fm1ble.h FM1BLE_TX_*), since f0e6d08 one per connection alongside the setup
TX_NAMES = ["air", "jieli", "intime", "arq"]
TX_MODES = ["air: as up to f0e6d08 (the response's bits 4-0 as on the air, an empty PDU at each arm)",
            "jieli: the IR's slot words (bit 2 the slot's, SN in bit 3), nothing written at the arm",
            "intime: as jieli, an empty PDU repeating the last SN at each arm, a response only in time",
            "arq: the chip's SN and NESN (MD in TXDHDR bit 3), the slot TXTOG names only, Cordio's SN and NESN the PAL's"]
# the link layer's control PDUs by opcode (Core Specification Vol 6 Part B 2.4.2), for the header trace
LL_OPS = ["CONN_UPDATE_IND", "CHANNEL_MAP_IND", "TERMINATE_IND", "ENC_REQ", "ENC_RSP", "START_ENC_REQ", "START_ENC_RSP",
          "UNKNOWN_RSP", "FEATURE_REQ", "FEATURE_RSP", "PAUSE_ENC_REQ", "PAUSE_ENC_RSP", "VERSION_IND", "REJECT_IND",
          "PERIPHERAL_FEATURE_REQ", "CONN_PARAM_REQ", "CONN_PARAM_RSP", "REJECT_EXT_IND", "PING_REQ", "PING_RSP",
          "LENGTH_REQ", "LENGTH_RSP", "PHY_REQ", "PHY_RSP", "PHY_UPDATE_IND", "MIN_USED_CHANNELS_IND"]
LOG_NONE = 0x8000                        # pal_fm1.h PAL_FM1_LOG_NONE
# the PDU capture (firmware/ble/fm1ble_test.h FM1BLE_CAP_*, since round 7): a frame after the steps', index len(STEPS)
CAP_HEAD = 16
ATT_OPS = {0x01: "ATT Error Rsp", 0x02: "ATT Exchange MTU Req", 0x03: "ATT Exchange MTU Rsp", 0x04: "ATT Find Information Req",
           0x05: "ATT Find Information Rsp", 0x06: "ATT Find By Type Value Req", 0x07: "ATT Find By Type Value Rsp",
           0x08: "ATT Read By Type Req", 0x09: "ATT Read By Type Rsp", 0x0A: "ATT Read Req", 0x0B: "ATT Read Rsp",
           0x0C: "ATT Read Blob Req", 0x0D: "ATT Read Blob Rsp", 0x10: "ATT Read By Group Type Req",
           0x11: "ATT Read By Group Type Rsp", 0x12: "ATT Write Req", 0x13: "ATT Write Rsp", 0x1B: "ATT Notification",
           0x1D: "ATT Indication", 0x1E: "ATT Confirmation", 0x52: "ATT Write Cmd"}
ATT_ERRORS = {0x01: "Invalid Handle", 0x02: "Read Not Permitted", 0x03: "Write Not Permitted", 0x04: "Invalid PDU",
              0x05: "Insufficient Authentication", 0x06: "Request Not Supported", 0x07: "Invalid Offset",
              0x0A: "Attribute Not Found", 0x0B: "Attribute Not Long", 0x0D: "Invalid Attribute Value Length",
              0x0E: "Unlikely Error", 0x10: "Unsupported Group Type", 0x11: "Insufficient Resources"}
L2CAP_SIG = {0x01: "L2CAP Command Reject", 0x12: "L2CAP Connection Parameter Update Req",
             0x13: "L2CAP Connection Parameter Update Rsp", 0x14: "L2CAP LE Credit Based Connection Req",
             0x15: "L2CAP LE Credit Based Connection Rsp", 0x16: "L2CAP Flow Control Credit"}
RX_TRIALS = [("C2:46:4D:31:00:13", "RX 0: no gain table, WINCNTL2 0x32"),
             ("C2:46:4D:31:00:14", "RX 1: the gain table, WINCNTL2 0"),
             ("C2:46:4D:31:00:15", "RX 2: the gain table, WINCNTL2 0x32 (JieLi's)"),
             ("C2:46:4D:31:00:16", "RX 3: the gain table, WINCNTL2 0x96 (the direct test's)")]
HW_WORDS = ["TXTOG", "RXTOG", "ANCHOR", "BITOFF", "EVTCOUNT", "LASTCHMAP", "RXSTAT0", "FORMAT"]
DELAY = {"reply": 3.0, "poll": 0.5, "central": 2.0}   # seconds; the tests shorten them
CENTRAL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ble_central.py")
WAIT_DEFAULT, WAIT_MAX = 20, 120


class ReportError(SlotError):
    pass


def decode(rep):
    """the report's bytes -> dict(run, cut_text, cut_steps, text, counters, steps); ReportError if malformed"""
    def need(n, what):
        if len(rep) < n:
            raise ReportError("noreply", f"the report ends inside {what} ({len(rep)} bytes)")

    need(10, "its header")
    if rep[:4] != b"OPLB":
        raise ReportError("noreply", f"not a Bluetooth report (starts {rep[:4].hex()})")
    if rep[4] != 1:
        raise ReportError("noreply", f"report version {rep[4]}: this tool reads version 1")
    flags, run, tl = rep[5], int.from_bytes(rep[6:8], "little"), int.from_bytes(rep[8:10], "little")
    at = 10 + tl
    need(at + 1, "its text")
    text = rep[10:at].decode("ascii", "replace")
    k = rep[at]
    at += 1
    need(at + 4 * k + 1, "its counters")
    vals = [int.from_bytes(rep[at + 4 * i:at + 4 * i + 4], "little") for i in range(k)]
    counters = {}
    names = COUNTERS_24 if k == len(COUNTERS_24) else COUNTERS
    if k == len(COUNTERS_F0E6D08) and vals[COUNTERS_F0E6D08.index("probe_delays")] == PROBE_DELAYS_F0E6D08:
        names = COUNTERS_F0E6D08
    for i, v in enumerate(vals):
        name = names[i] if i < len(names) else f"counter_{i}"
        if isinstance(name, tuple):
            counters[name[0]], counters[name[1]] = v & 0xFFFF, v >> 16
        else:
            counters[name] = v
    at += 4 * k
    n = rep[at]
    at += 1
    steps, capture = [], None
    for i in range(n):
        need(at + 6, f"step {i + 1}'s header")
        idx, res = rep[at], rep[at + 1]
        had, kept = int.from_bytes(rep[at + 2:at + 4], "little"), int.from_bytes(rep[at + 4:at + 6], "little")
        need(at + 6 + kept, f"step {i + 1}'s bytes")
        if idx == len(STEPS):                            # the PDU capture (fm1ble_test.h FM1BLE_CAP_*)
            capture = rep[at + 6:at + 6 + kept]
            at += 6 + kept
            continue
        steps.append({"step": STEPS[idx] if idx < len(STEPS) else f"step{idx}", "index": idx,
                      "result": RESULTS.get(res, f"result{res}"), "had": had, "data": rep[at + 6:at + 6 + kept]})
        at += 6 + kept
    if at != len(rep):
        raise ReportError("noreply", f"{len(rep) - at} bytes after the last step")
    return {"run": run, "cut_text": bool(flags & 1), "cut_steps": bool(flags & 2), "text": text,
            "counters": counters, "steps": steps, "capture": capture}


def ask(link, cmd, body):
    for _ in range(3):
        if not link.send(message(cmd, body)):
            raise ReportError("noreply", "the MIDI port is gone")
        try:
            return wait_reply(link, cmd, DELAY["reply"])
        except SlotError:
            continue
    raise ReportError("noreply", "no reply from the FM-1 (unplugged, or a firmware without the Bluetooth report?)")


def chunk(link, off):
    """-> (status, total, serial, data) for the report from off"""
    while True:
        b = ask(link, REPORT, off.to_bytes(3, "little"))
        if len(b) < 9:
            raise ReportError("noreply", f"a {len(b)}-byte reply to the report command")
        if int.from_bytes(b[4:7], "little") == off:
            return b[0], int.from_bytes(b[1:4], "little"), int.from_bytes(b[7:9], "little"), b[9:]


def read(link, tries=3):
    """the whole report (bytes); the FM-1 rebuilding it meanwhile (serial changed) starts the read again"""
    for _ in range(tries):
        data, off, serial = bytearray(), 0, None
        while True:
            st, total, ser, b = chunk(link, off)
            if st != OK:
                raise ReportError("refused", STATUS.get(st, f"status {st}"))
            if serial is None:
                serial = ser
            if ser != serial:
                break
            if not b and off < total:
                raise ReportError("noreply", f"an empty chunk at {off} of {total}")
            data += b
            off += len(b)
            if off >= total:
                return bytes(data)
    raise ReportError("noreply", "the report kept changing while it was read")


def start(link, wait, timeout):
    """start a run and wait for its report; -> the run's number"""
    b = ask(link, RUN, bytes([wait]))
    if len(b) < 3:
        raise ReportError("noreply", f"a {len(b)}-byte reply to the run command")
    st, run = b[0], int.from_bytes(b[1:3], "little")
    if st == E_RUNNING:
        print(f"run {run} is already going on: waiting for it", file=sys.stderr)
    elif st != OK:
        raise ReportError("refused", STATUS.get(st, f"status {st}"))
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        st, _, _, _ = chunk(link, 0)
        if st == OK:
            return run
        if st != E_RUNNING:
            raise ReportError("refused", f"the run did not start: {STATUS.get(st, f'status {st}')}")
        time.sleep(DELAY["poll"])
    raise ReportError("timeout", f"run {run} not finished after {timeout:g} s")


def counters_text(c):
    return "".join(f"{k} {v:08X}\n" if k in HEX_COUNTERS else f"{k} {v}\n" for k, v in c.items())


def step_name(i, st):
    return f"step-{i + 1}-{st['step']}.bin"


def write_out(d, rep, raw):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "report.bin"), "wb") as f:
        f.write(raw)
    with open(os.path.join(d, "report.txt"), "w") as f:
        f.write(rep["text"])
    with open(os.path.join(d, "counters.txt"), "w") as f:
        f.write(counters_text(rep["counters"]))
    for st in rep["steps"]:
        if st["data"]:
            with open(os.path.join(d, step_name(st["index"], st)), "wb") as f:
                f.write(st["data"])
    if rep.get("capture"):
        with open(os.path.join(d, "capture.bin"), "wb") as f:
            f.write(rep["capture"])


def air_trials(rep):
    """adv_air's evidence (firmware/ble/fm1ble_test.c air_step) -> (per trial: address, what, events, TXTOG changes,
    received), {hardware word: value}; None for a report without it (the firmware before the trials)"""
    st = next((s for s in rep["steps"] if s["step"] == "adv_air"), None)
    if not st or len(st["data"]) != 6 * len(TRIALS) + 2 * len(HW_WORDS):
        return None
    d = st["data"]
    u16 = [int.from_bytes(d[i:i + 2], "little") for i in range(0, len(d), 2)]
    trials = [(TRIALS[k][0], TRIALS[k][1], *u16[3 * k:3 * k + 3]) for k in range(len(TRIALS))]
    return trials, dict(zip(HW_WORDS, u16[3 * len(TRIALS):]))


def rx_trials(rep):
    """the RX trials' counters (fm1ble_test.h FM1BLE_RXT_*) -> [dict per trial], or None for a report without them"""
    c = rep["counters"]
    if "rxt3_bytes" not in c:
        return None
    out = []
    for k, (addr, what) in enumerate(RX_TRIALS):
        n, st, pk, by = (c[f"rxt{k}_{w}"] for w in ("counts", "stat", "pkt", "bytes"))
        out.append({"addr": addr, "what": what, "events": n & 0xFF, "flips": n >> 8 & 0xFF, "rx": n >> 16 & 0xFF,
                    "scan_req": n >> 24, "rxstat": st & 0xFFFF, "rxahdr": st >> 16, "rxdhdr": pk & 0xFFFF,
                    "buf": pk >> 16 & 0xFF, "chan": pk >> 24, "bytes": by.to_bytes(4, "little")})
    return out


def show_rx(rep, out):
    t = rx_trials(rep)
    if not t:
        return
    c = rep["counters"]
    print("\nRX TRIALS (the receive setups; a central's active scan sends each SCAN_REQs)", file=out)
    for r in t:
        line = (f"  {r['addr']}  {r['what']}: {r['events']} events, {r['flips']} TXTOG changes, {r['rx']} received, "
                f"{r['scan_req']} SCAN_REQ")
        if r["buf"] != 0xFF:
            line += (f"; first: RXSTAT {r['rxstat']:04X}, type {r['rxahdr'] & 0xF} (RXAHDR {r['rxahdr']:04X}), "
                     f"length {r['rxdhdr'] >> 8}, buffer {r['buf']}, bytes {r['bytes'].hex(' ')}")
        else:
            line += f"; nothing received: RXSTAT0 {r['rxstat']:04X}, RXAHDR0 {r['rxahdr']:04X} as left"
        print(line, file=out)
    if c.get("rx_stack_trial", 0xFF) != 0xFF:
        print(f"  the stack ran on RX trial {c['rx_stack_trial']}'s window: WINCNTL2 {c['rx_win']:X}, gain table "
              f"{'written' if c['rx_agc_table'] else 'not written'}", file=out)
    print(f"  AGC block after the RX trials: 0x2FD80 {c['agc_ctl']:08X}, 0x2FD8C {c['agc_rssi']:08X}; RXSTAT1 "
          f"{c['rx_stat1']:04X}; last windows WINCNTL0 {c['conn_win'] & 0xFFFF:X}, WINCNTL2 {c['conn_win'] >> 16:X}; "
          f"Cordio's last connection timeout {c['pal_conn_timeout_us']} us", file=out)


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def show_conn(rep, out):
    """the connection events' counters (pal_fm1.h: the busy wait, the window setups), for a report that has them"""
    c = rep["counters"]
    if "conn_rx_words" not in c:
        return
    log = "pal_log_n" in c
    per_conn = "conn0_a" in c
    if "pal_tx_order" in c:
        # since the TX modes the connections turn through those; the window setups are the build's (fm1ble_stack.c: PADW only)
        used = sorted({c[f"conn{k}_a"] & 0xF for k in range(min(c["pal_conns"], 8))})
        names = ", ".join(SETUP_NAMES[s] if s < len(SETUP_NAMES) else str(s) for s in used) or "none started"
        how = f"each connection runs one TX mode, the next connection the next one; window setups used: {names})"
    elif per_conn:
        how = "each connection runs one window setup, the next connection the next one)"
    else:
        how = "each runs one window setup; " + ("3 missed in a row move to the next)" if log else "a missed one moves to the next)")
    print("\nCONNECTION EVENTS (" + how, file=out)
    for k, what in enumerate(CONN_SETUPS):
        if f"pal_set{k}_counts" not in c:
            continue
        n, a = c[f"pal_set{k}_counts"], c[f"pal_set{k}_arr"]
        line = f"  setup {k} {what}: {n & 0xFFFF} events, {n >> 16} received"
        if n >> 16:
            line += f"; packets {s16(a & 0xFFFF)}..{s16(a >> 16)} us after the window's start"
        print(line, file=out)
    w = c["conn_rx_words"]
    if per_conn:
        show_conns(c, out)
    print(f"  in use at the end: setup {c['pal_conn_setup']}, {c['pal_setup_moves']} moves; busy waits {c['pal_busy_waits']} "
          f"(longest {c['pal_busy_us_max']} us); Cordio's response {c['pal_resp_us_max']} us after a packet at most "
          f"(T_IFS 150); late operations {c['pal_late_ops']}; bad CRC {c['pal_conn_rx_bad']}", file=out)
    print(f"  the hardware: TXTOG changed after {c['conn_flips']} connection events; {c['conn_acks']} packets with RXSTAT "
          f"bit 8; the last packet RXSTAT {w & 0xFFFF:04X}, RXDHDR {w >> 16:04X} (length {w >> 24}, NESN {w >> 18 & 1}, "
          f"SN {w >> 19 & 1}, MD {w >> 20 & 1})", file=out)
    if "pal_tx_order" in c:
        show_tx(c, out)
    if log:
        show_log(c, out)


def show_tx(c, out):
    """the TX side (pal_fm1.h, fm1ble.h FM1BLE_TX_*), for a report that has it"""
    nm = 4 if "pal_tx3_counts" in c else 3
    order = [c["pal_tx_order"] >> (4 * k) & 0xF for k in range(nm)]
    print(f"  TX modes, one per connection in the order {', '.join(TX_NAMES[m] if m < len(TX_NAMES) else str(m) for m in order)}:", file=out)
    for k, what in enumerate(TX_MODES[:nm]):
        n, s_ = c[f"pal_tx{k}_counts"], c[f"pal_tx{k}_sent"]
        print(f"    {what}: {n & 0xFFFF} events, {n >> 16} received, TXTOG changed in {s_ & 0xFFFF}, {s_ >> 16} packets with "
              f"RXSTAT bit 8", file=out)
    h, hist, mn = c["pal_halts"], c["pal_resp_hist"], c["pal_resp_us_min"]
    tifs, stale = c["pal_cfg"] & 0xFFFF, c["pal_cfg"] >> 16
    print(f"  Cordio's response after the packet was seen: under 50 us {hist & 0xFF}, 50-99 {hist >> 8 & 0xFF}, 100-149 "
          f"{hist >> 16 & 0xFF}, 150 and more {hist >> 24} (least {'-' if mn == 0xFFFF else mn} us); intime left out "
          f"{c['pal_resp_skipped']} (over {tifs} us)", file=out)
    print(f"  stale packet flags dropped {c['pal_stale_rx']} (starting {stale} us or more before the window); events halted at "
          f"their deadline {h & 0xFFFF}, the hardware busy through {h >> 16}", file=out)
    f, t, dh, tb = c["pal_first_rx_tx"], c["conn_txtog"], c["conn_txdhdr"], c["conn_txbuf"]
    if f >> 31:
        print(f"  at the first connection packet (before Cordio's response): TXDHDR0 {f & 0xFF:02X}, TXDHDR1 {f >> 8 & 0xFF:02X}, "
              f"TXTOG {f >> 16 & 1}, RXTOG {f >> 17 & 1}, TXBUF0CNTL {f >> 18 & 1}, TXBUF1CNTL {f >> 19 & 1}", file=out)
    print(f"  at the end: TXTOG {t & 0xFFFF:04X}, RXTOG {t >> 16:04X}, TXDHDR0 {dh & 0xFFFF:04X}, TXDHDR1 {dh >> 16:04X}, "
          f"TXBUF0CNTL {tb & 0xFF:02X}, TXBUF1CNTL {tb >> 8 & 0xFF:02X}, IFSCNT {tb >> 16:04X}", file=out)


def ll_op(op, llid):
    """a data PDU's kind from its LLID and first payload byte"""
    if llid == 3:
        return f"LL_{LL_OPS[op]}" if op < len(LL_OPS) else f"LL op {op:02X}"
    return {1: "empty" if not op else "continuation", 2: "L2CAP"}.get(llid, f"LLID {llid}")


def show_ll(c, out):
    """the link layer (pal_fm1.h PAL_FM1_TRACE_*, ll_ops, ll_cnt) and the host's side, for a report that has them"""
    if "tr0_0" not in c:
        return
    acl, mtu = c["stack_acl"], c["nimble_mtu"]
    print("\nLINK LAYER (the PAL's header trace; RXDHDR as the chip wrote it, Cordio's view as the PAL gave it)", file=out)
    print(f"  HCI ACL: host to controller {acl & 0xFFFF}, controller to host {acl >> 16}; Number Of Completed Packets "
          f"{c['stack_nocp']}; NimBLE's ATT MTU {mtu & 0xFFFF}, the central's writes {mtu >> 16}", file=out)
    dup, unw = c["pal_arq_dup"], c["pal_arq_unwritten"]
    print(f"  ARQ: packets Cordio was told were repeats {dup & 0xFFFF}, responses that did not go in "
          f"{unw & 0xFFFF}; at the end RXTOG {c['pal_rxtog'] & 0xFFFF:04X}, TXTOG {c['pal_rxtog'] >> 16:04X}", file=out)
    print(f"  packets after the first in a connection event {dup >> 16}, given to Cordio {unw >> 16} (both 0 in a report "
          f"from before round 6: not counted)", file=out)
    for k in range(min(c["pal_conns"], 4)):
        ops, n = c[f"conn{k}_ops"], c[f"conn{k}_ll"]
        x = c.get(f"conn{k}_d", 0) & 0xF
        names = [LL_OPS[i] if i < len(LL_OPS) else f"{i:02X}" for i in range(32) if ops >> i & 1]
        print(f"  connection {k + 1} ({TX_NAMES[x] if x < len(TX_NAMES) else x}): the central's control PDUs "
              f"{', '.join(names) or 'none'}; its L2CAP PDUs Cordio took {n & 0xFF}; Cordio's non-empty responses put in "
              f"{n >> 8 & 0xFF}; told not acknowledged {n >> 16 & 0xFF}; RXTOG changed {n >> 24}", file=out)
    for k in range(2):
        words = [c[f"tr{k}_{i}"] for i in range(20)]
        if not any(words):
            continue
        print(f"  connection {k + 1}, its first events with a packet: RXDHDR bits 4-0, the packet, RXTOG/TXTOG changed, "
              f"what Cordio was told (ack: its last PDU acknowledged, new), Cordio's answer (header bits 4-0: MD SN NESN LLID)",
              file=out)
        for i, w in enumerate(words):
            if not w:
                break
            rx = f"{w & 0x1F:02X} {ll_op(w >> 8 & 0xFF, w & 3):<22s}{'' if w >> 5 & 1 else ' crc'}"
            tog = f"{'R' if w >> 6 & 1 else '-'}{'T' if w >> 7 & 1 else '-'}"
            told = f"{'ack' if w >> 23 & 1 else 'nak'} {'new' if w >> 24 & 1 else 'rep'}" if w >> 5 & 1 else "       "
            ans = "-"
            if w >> 21 & 1:
                h = w >> 16 & 0x1F
                ans = (f"{h:02X} {ll_op(w >> 25, h & 3) if h & 3 != 1 or w >> 25 else 'empty'}"
                       f"{'' if w >> 22 & 1 else ' (not put in)'}")
            print(f"    {i:2d}  {rx} {tog}  {told}  {ans}", file=out)


def pdu_what(llid, data, n):
    """a data PDU's payload (its first bytes) -> what it is: an LL control PDU, an ATT or L2CAP signalling PDU"""
    if llid == 3:
        return f"LL {LL_OPS[data[0]] if data and data[0] < len(LL_OPS) else (f'{data[0]:02X}' if data else '?')}"
    if llid == 1:
        return "L2CAP continuation"
    if len(data) < 5:
        return "L2CAP"
    ln, cid, op = int.from_bytes(data[0:2], "little"), int.from_bytes(data[2:4], "little"), data[4]
    tail = "" if ln + 4 == n else f" (L2CAP length {ln}, the PDU's {n})"
    if cid == 4:
        w = ATT_OPS.get(op, f"ATT {op:02X}")
        if op == 0x01 and len(data) >= 9:
            req, h, e = data[5], int.from_bytes(data[6:8], "little"), data[8]
            w += f": {ATT_OPS.get(req, f'{req:02X}')}, handle {h:04X}, {ATT_ERRORS.get(e, f'error {e:02X}')}"
        elif op in (0x02, 0x03) and len(data) >= 7:
            w += f": MTU {int.from_bytes(data[5:7], 'little')}"
        elif op in (0x04, 0x06, 0x08, 0x10) and len(data) >= 9:
            w += f": {int.from_bytes(data[5:7], 'little'):04X}-{int.from_bytes(data[7:9], 'little'):04X}"
            if op in (0x08, 0x10) and len(data) >= 11 and ln == 7:
                w += f", type {int.from_bytes(data[9:11], 'little'):04X}"
        return w + tail
    if cid == 5:
        return L2CAP_SIG.get(op, f"L2CAP signalling {op:02X}") + tail
    return f"L2CAP channel {cid:04X}" + tail


def show_capture(rep, out):
    """the PDU capture and ARQ's ways (fm1ble_test.h FM1BLE_CAP_*, pal_fm1.h PAL_FM1_CAP), for a report that has them"""
    c = rep.get("capture")
    if not c or len(c) < CAP_HEAD or c[0] != 1:
        return
    n, size = c[1], c[2]
    u16 = lambda i: int.from_bytes(c[i:i + 2], "little")  # noqa: E731
    ahead = [str(k + 1) for k in range(16) if u16(4) >> k & 1]
    kept = {0: "in T_IFS", 1: "ahead"}.get(c[6], "none")
    print(f"\nPDU CAPTURE (what went each way, as the chip received it and as Cordio handed it over; ARQ's ways)", file=out)
    print(f"  ARQ connections {c[3]}: ahead {', '.join(ahead) or 'none'}, the others in T_IFS; kept: {kept} "
          f"(after {c[14]} L2CAP PDUs from the central); responses written ahead {u16(12)}", file=out)
    print(f"  the chip's SN: bit 2 changed at a connection's first packet {u16(8)}, after {c[7]} packets the central took "
          f"none of {u16(10)}", file=out)
    print("     # conn    ev  from     hdr len  side  what", file=out)
    for k in range(n):
        e = c[CAP_HEAD + size * k:CAP_HEAD + size * (k + 1)]
        if len(e) < 6:
            break
        mine, ln, ev = e[0] & 1, e[2], e[3] | e[4] << 8
        data = e[6:6 + min(ln, size - 6)]
        if mine:
            frm, side = "Cordio" + ("+" if e[0] >> 1 & 1 else " "), "-" if e[5] == 0xFF else f"{2 * e[5]}us"
        else:
            frm = "central" + ("" if e[0] >> 1 & 1 else "!")
            side = ("R" if e[5] & 1 else "-") + ("T" if e[5] & 2 else "-") + ("+" if e[0] >> 3 & 1 else "")
        print(f"  {k:4d} {(e[0] >> 4) + 1:4d} {ev:5d}  {frm:8s} {e[1]:02X} {ln:4d}  {side:5s} {pdu_what(e[1] & 3, data, ln)}",
              file=out)
        print(f"{'':30s}{data.hex(' ')}", file=out)
    print("  (from: Cordio+ written ahead; side: Cordio's response time after the packet was seen, or for the central's, "
          "RXTOG/TXTOG changed at it, + a later packet of its event; hdr: RXDHDR bits 4-0 as the chip wrote them, or "
          "Cordio's header bits 4-0; the first 14 payload bytes)", file=out)


def t16(v):
    """a signed 16-bit time of the PAL's (LOG_NONE: none) -> int or None"""
    return None if v == LOG_NONE else s16(v)


def show_conns(c, out):
    """the PAL's connection summaries (pal_fm1.h conn_sum) and NimBLE's connects"""
    n = c["pal_conns"]
    nc = c.get("nimble_conns", 0)
    good = c.get("pal_good", 0xFF)
    gt = c.get("pal_good_tx", 0xFF)
    print(f"  connections: {n} started on the PAL, NimBLE {nc & 0xFFFF} connects and {nc >> 16} disconnects; kept setup: "
          f"{SETUP_NAMES[good] if good < len(SETUP_NAMES) else 'none'}" +
          (f", kept TX mode: {TX_NAMES[gt] if gt < len(TX_NAMES) else 'none'}" if "pal_good_tx" in c else ""), file=out)
    for k in range(min(n, 8)):
        a, b, d = c[f"conn{k}_a"], c[f"conn{k}_b"], c[f"conn{k}_c"]
        st = a & 0xF
        line = f"  connection {k + 1}: {SETUP_NAMES[st] if st < len(SETUP_NAMES) else st}"
        if f"conn{k}_d" in c:
            x = c[f"conn{k}_d"]
            line += f" {TX_NAMES[x & 0xF] if (x & 0xF) < len(TX_NAMES) else x & 0xF}"
        line += f", {a >> 4 & 0xFFF} events, {a >> 16} received"
        f0, f1 = t16(b & 0xFFFF), t16(b >> 16)
        if f0 is not None:
            line += f"; packets {f0} (first) and {f1} (last) us after dueUsec"
        e0, e1 = t16(d & 0xFFFF), t16(d >> 16)
        if e0 is not None:
            line += f"; missed events ended {e0}..{e1} us after dueUsec"
        if f"conn{k}_d" in c:
            x = c[f"conn{k}_d"]
            line += f"; TXTOG changed in {x >> 4 & 0xFFF}, stale {x >> 16 & 0xFF}, halted {x >> 24}"
        print(line, file=out)


def show_probe(rep, out):
    """the window probe (fm1ble_test.h FM1BLE_PROBE_*), for a report that has it"""
    c = rep["counters"]
    if "probe_n" not in c or not c["probe_n"]:
        return
    d0, d1 = c["probe_delays"] & 0xFFFF, c["probe_delays"] >> 16
    w0, w1 = c["probe_windows"] & 0xFFFF, c["probe_windows"] >> 16
    print(f"\nWINDOW PROBE (no central: {c['probe_n']} connection events on a data channel, each setup at delays {d0} and "
          f"{d1} us and windows {w0} and {w1} us; when the baseband's end flag came, us after the arm plus the delay, "
          "earliest..latest of the repeats)", file=out)
    print(f"  setup  d{d0} w{w0:<5d} d{d0} w{w1:<5d} d{d1} w{w0:<5d} d{d1} w{w1:<5d}", file=out)

    def cell(v):
        lo, hi = v & 0xFFFF, v >> 16
        if lo == LOG_NONE:
            return "-"
        if lo == 0x7FFF:
            return "never"
        return f"{s16(lo)}..{s16(hi)}"
    for st in range(7):
        print(f"  {SETUP_NAMES[st]:5s} " + " ".join(f"{cell(c[f'pr{st}_{x}']):>12s}" for x in "abcd"), file=out)
    for w in ("idle", "arm"):
        print(f"  anchor file read back {'with no event' if w == 'idle' else 'an event armed'}, two reads "
              f"600 us apart: " + ", ".join(f"index {i.upper() if i != '10' else '0x10'} {c[f'clk_{w}_{i}'] & 0xFFFF:04X} "
                                          f"{c[f'clk_{w}_{i}'] >> 16:04X}" for i in ("0", "e", "7", "10")), file=out)


def log_events(c):
    """the PAL's event log (pal_fm1.h): one dict per logged connection event"""
    evs = []
    for k in range(min(c["pal_log_n"], 12)):
        a, b, d = c[f"ev{k}_a"], c[f"ev{k}_b"], c[f"ev{k}_c"]
        rx, end = b & 0xFFFF, b >> 16
        evs.append({"chan": a & 0x3F, "setup": (a >> 6 & 3) | (a >> 14 & 1) << 2, "got": bool(a >> 8 & 1), "crc_ok": bool(a >> 9 & 1),
                    "sent": bool(a >> 10 & 1), "deadline": bool(a >> 11 & 1), "cordio_went_on": bool(a >> 12 & 1),
                    "first": bool(a >> 13 & 1), "timeout_us": a >> 16, "delay_us": d & 0xFFFF, "rxdhdr": d >> 16,
                    "rx_us": None if rx == LOG_NONE else s16(rx), "end_us": None if end == LOG_NONE else s16(end),
                    "stale": bool(a >> 15 & 1)})
        if f"ev{k}_d" in c:
            x = c[f"ev{k}_d"]
            evs[-1].update({"tx": x >> 22 & 3, "txdhdr0": x & 0xFF, "txdhdr1": x >> 8 & 0xFF, "txtog": (x >> 16 & 1, x >> 17 & 1),
                            "rxtog": (x >> 18 & 1, x >> 19 & 1), "txbuf": (x >> 20 & 1, x >> 21 & 1),
                            "resp_us": None if x >> 24 == 0xFF else 2 * (x >> 24)})
    return evs


def show_log(c, out):
    print(f"  the CONNECT_IND was seen {c['pal_cind_gap_us']} us after the PAL last looked (its timestamp, which places the "
          f"first window, is up to that late)", file=out)
    evs = log_events(c)
    if not evs:
        print("  no connection event logged", file=out)
        return
    print(f"  the first {len(evs)} connection events" + (" (the first 4 of each connection)" if "conn0_a" in c else "") +
          " (times in us from Cordio's dueUsec; end: when the hardware's flag came):", file=out)
    txw = "tx" in evs[0]
    print("     # ch setup  armed timeout  packet     end  sent RXDHDR" +
          ("  mode   resp  TXDHDR0/1 TXTOG RXTOG BUF" if txw else ""), file=out)
    for k, e in enumerate(evs):
        pkt = "-" if e["rx_us"] is None else f"{e['rx_us']}{'' if e['crc_ok'] else ' crc'}"
        if e["stale"]:
            pkt += " st"
        end = "deadline" if e["deadline"] else "cordio" if e["cordio_went_on"] else "-" if e["end_us"] is None else e["end_us"]
        hdr = f"{e['rxdhdr']:04X}" if e["got"] else "-"
        line = (f"  {k:2d}{'*' if e['first'] else ' '} {e['chan']:2d} {SETUP_NAMES[e['setup']]:5s} {e['delay_us']:6d} {e['timeout_us']:7d}"
                f" {pkt:>7s} {end!s:>7s}  {'yes' if e['sent'] else 'no ':3s}  {hdr:6s}")
        if txw:
            resp = "-" if e["resp_us"] is None else str(e["resp_us"])
            line += (f"  {TX_NAMES[e['tx']] if e['tx'] < len(TX_NAMES) else e['tx']:6s} {resp:>5s}  {e['txdhdr0']:02X}/{e['txdhdr1']:02X}"
                     f"     {e['txtog'][0]}>{e['txtog'][1]}   {e['rxtog'][0]}>{e['rxtog'][1]}   {e['txbuf'][0]}{e['txbuf'][1]}")
        print(line.rstrip(), file=out)
    print("  (* a connection's first event; the window a setup opens: keep at dueUsec, pad (padj, padb, padw) 625 us and "
          "wide 1250 us before it, each that much longer on both sides; widen at dueUsec + half the timeout" +
          ("; st: a stale packet flag dropped; resp: Cordio's response, us after the packet was seen; TXDHDR0/1 at the end, "
           "TXTOG and RXTOG bit 0 at the arm > at the end, TXBUF0CNTL and TXBUF1CNTL bit 0 at the end)" if txw else ")"), file=out)


def show(rep, out):
    print(f"Bluetooth test run {rep['run']}", file=out)
    print(rep["text"], end="" if rep["text"].endswith("\n") else "\n", file=out)
    print("\nCOUNTERS", file=out)
    print(counters_text(rep["counters"]), end="", file=out)
    print("\nSTEPS", file=out)
    for st in rep["steps"]:
        print(f"{st['index'] + 1:2d} {st['step']:9s} {st['result']:7s} {st['had']:2d} bytes  {st['data'].hex()}", file=out)
    t = air_trials(rep)
    if t:
        print("\nAIR TRIALS (the driver's own ADV_IND; a central should list the addresses that reached the air)", file=out)
        for addr, what, ev, flips, rx in t[0]:
            print(f"  {addr}  {what}: {ev} events, {flips} TXTOG changes, {rx} received", file=out)
        print("  after the last trial: " + ", ".join(f"{k} {v:04X}" for k, v in t[1].items()), file=out)
    show_rx(rep, out)
    show_probe(rep, out)
    show_conn(rep, out)
    show_ll(rep["counters"], out)
    show_capture(rep, out)
    if rep["cut_text"] or rep["cut_steps"]:
        print("(the FM-1 cut the report short: it did not fit its buffer)", file=out)


class Central:
    """tools/ble_central.py on another host over ssh (the script on its stdin), or here ("local"), in the background"""

    def __init__(self, host, timeout):
        args = ["--stack", STACK_ADDR, "--trials", ",".join(a for a, _ in TRIALS + RX_TRIALS), "--timeout", f"{timeout:g}", "--json"]
        if host == "local":
            argv, stdin = [sys.executable, "-I", CENTRAL, *args], subprocess.DEVNULL
        else:
            argv, stdin = ["ssh", "-o", "BatchMode=yes", host, "python3", "-", *args], open(CENTRAL, "rb")
        self.host = host
        self.p = subprocess.Popen(argv, stdin=stdin, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if stdin is not subprocess.DEVNULL:
            stdin.close()

    def result(self, timeout):
        try:
            out, err = self.p.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.p.kill()
            out, err = self.p.communicate()
            return {"error": f"the central on {self.host} did not finish", "stderr": err.decode("utf-8", "replace")[-2000:]}
        lines = out.decode("utf-8", "replace").strip().splitlines()
        try:
            return json.loads(lines[-1])
        except (IndexError, ValueError):
            return {"error": f"no result from the central on {self.host} (exit {self.p.returncode})",
                    "stderr": err.decode("utf-8", "replace")[-2000:]}


def show_central(c, out):
    print("\nCENTRAL (tools/ble_central.py)", file=out)
    if c.get("error"):
        print(f"  error: {c['error']}", file=out)
        if c.get("stderr"):
            print("  " + c["stderr"].strip().replace("\n", "\n  "), file=out)
    seen = c.get("seen", {})
    for addr, what in [*TRIALS, *RX_TRIALS, (STACK_ADDR, "the stack (Cordio, NimBLE)")]:
        d = seen.get(addr)
        print(f"  {addr}  {what}: " + (f"seen at {d['first']:.1f} s, RSSI {d.get('rssi')}" if d else "not seen"), file=out)
    if "connected" in c:
        if c["connected"] and "services" in c:
            print(f"  GATT services resolved: {'yes' if c['services'] else 'no'}; dropped: "
                  f"{'after %g s' % c['dropped'] if c.get('dropped') is not None else 'no'}", file=out)
        print(f"  connect: {'yes' if c['connected'] else 'no'}; MIDI port: {c.get('port') or 'none'}; Note On sent: "
              f"{'yes' if c.get('sent') else 'no'}; heard from the FM-1: {c.get('received') or 'nothing'}", file=out)
    for k, t in enumerate(c.get("attempts", [])):
        what = "connected" if t.get("connected") else "no connection"
        if t.get("connected"):
            what += f", GATT services {'resolved' if t.get('services') else 'not resolved'}"
            what += f", dropped after {t['dropped']:g} s" if t.get("dropped") is not None else ", not dropped"
        if t.get("error"):
            what += f" ({t['error']})"
        print(f"  attempt {k + 1} at {t.get('at', 0):.1f} s: {what}", file=out)
    for e in c.get("errors", []):
        print(f"  {e}", file=out)


EXIT = {"refused": 1, "usage": 2, "notfound": 3, "noreply": 4, "timeout": 5}


def run(a, backend, out):
    up = fi.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        raise ReportError("notfound", str(fi.not_found(up)))
    central = None
    try:
        if a.run and a.central:
            central = Central(a.central, a.timeout)
            time.sleep(DELAY["central"])             # scanning before the first trial's advertisements
        want = start(dev.link, a.wait, a.timeout) if a.run else None
        raw = read(dev.link)
    finally:
        dev.link.close()
        if central and central.p.poll() is None and not a.run:
            central.p.kill()
    rep = decode(raw)
    if want is not None and rep["run"] != want:
        raise ReportError("refused", f"run {want} did not start (EDIT pressed on the FM-1?): the FM-1 holds run "
                                     f"{rep['run']}'s report")
    shown = io.StringIO()
    show(rep, shown)
    c = central.result(a.timeout) if central else None
    if c is not None:
        show_central(c, shown)
    out.write(shown.getvalue())
    if a.out:
        write_out(a.out, rep, raw)
        with open(os.path.join(a.out, "summary.txt"), "w") as f:      # what this printed: the event log's table with it
            f.write(shown.getvalue())
        n = sum(1 for st in rep["steps"] if st["data"])
        if c is not None:
            with open(os.path.join(a.out, "central.json"), "w") as f:
                json.dump(c, f, indent=1)
        print(f"-> {a.out} (report.txt, report.bin, counters.txt, summary.txt, {n} step files{', central.json' if c is not None else ''})",
              file=out)
    return 0


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--read", action="store_true", help="read the last run's report (the default)")
    g.add_argument("--run", action="store_true", help="start a run, wait for it, then read its report")
    ap.add_argument("--wait", type=int, default=WAIT_DEFAULT, metavar="S",
                    help=f"--run: seconds a central has to connect (0-{WAIT_MAX}, default {WAIT_DEFAULT}; 0 skips the connect steps)")
    ap.add_argument("--timeout", type=float, metavar="S",
                    help="--run: seconds to wait for the run (default: the wait plus 60)")
    ap.add_argument("--out", metavar="DIR", help="write the text, the counters and each step's bytes into DIR")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    ap.add_argument("--central", metavar="HOST", help="--run: run tools/ble_central.py on HOST over ssh ('local': here) "
                    "as the central meanwhile")
    a = ap.parse_args(argv)
    if not 0 <= a.wait <= WAIT_MAX:
        ap.error(f"--wait takes 0-{WAIT_MAX} seconds")
    if a.timeout is None:
        a.timeout = a.wait + 60.0
    try:
        if backend is None:
            try:
                backend = fi.MidoBackend()
            except ImportError:
                raise ReportError("usage", "needs mido and python-rtmidi (pip install mido python-rtmidi)")
        return run(a, backend, out)
    except SlotError as e:
        print(f"error: {e}", file=sys.stderr)
        return EXIT.get(e.code, 1)


if __name__ == "__main__":
    sys.exit(main())
