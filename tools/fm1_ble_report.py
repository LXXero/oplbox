#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read the Bluetooth test page's results from an FM-1 over USB-MIDI, or start a run first.

  fm1_ble_report.py [--read] [--out DIR] [--port NAME]                 the last run's report
  fm1_ble_report.py --run [--wait S] [--timeout S] [--out DIR] [--port NAME]   start a run, wait for it, then the report

SysEx in the updater's framing (firmware/src/ble_sysex.c, docs/API.md "Bluetooth test page over SysEx"). The report is
what the page's twelve pages show, as text, then the steps' full notes, then the counters behind pages 10-12 and each
step's raw evidence, as bytes (docs/BLUETOOTH_PAGE.md says what each step's evidence is). The tool prints the text and the
counters. With --out DIR it writes report.txt, report.bin (the report as sent), counters.txt and one file per step that
has evidence, step-N-<name>.bin (register readbacks as u32 little-endian, or HCI event bytes, as the step kept them).

--run starts a run as SAVE, HOME, then PLAY would on the FM-1 (the sound stops; the page stays open afterwards, SAVE
leaves it). --wait S is how long a central has to connect once advertising is on (0-120 s, default 20; 0 skips the
connect and MIDI steps): connect to the FM-1 from a phone or a Mac (a BLE MIDI app) within that time. The FM-1 refuses
while EDIT is open. Finds the FM-1 the way fm1_install.py does. Needs mido with python-rtmidi.

Exit codes: 0 done, 1 the FM-1 refused or has no report (no run since it started), 2 bad arguments, 3 FM-1 not found,
4 no reply (connection lost, or a firmware without the Bluetooth report), 5 the run did not finish within --timeout.
"""
import argparse
import os
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
COUNTERS = ["radio_regs_ok", "radio_regs_bad", "bb_irq", "bb_irq_tx", "bb_irq_rx", "hci_cmds", "hci_events", "hci_bad",
            "hci_status", "hci_op", "adv_events", "connects", "disconnects", "acl_in", "acl_out", "att_mtu",
            "interval", "latency", "timeout", "midi_in", "midi_out", "notify_failed", "nocp", "reserved"]
HEX_COUNTERS = {"hci_status", "hci_op"}
DELAY = {"reply": 3.0, "poll": 0.5}   # seconds; the tests shorten them
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
    counters = {COUNTERS[i] if i < len(COUNTERS) else f"counter_{i}": v for i, v in enumerate(vals)}
    at += 4 * k
    n = rep[at]
    at += 1
    steps = []
    for i in range(n):
        need(at + 6, f"step {i + 1}'s header")
        idx, res = rep[at], rep[at + 1]
        had, kept = int.from_bytes(rep[at + 2:at + 4], "little"), int.from_bytes(rep[at + 4:at + 6], "little")
        need(at + 6 + kept, f"step {i + 1}'s bytes")
        steps.append({"step": STEPS[idx] if idx < len(STEPS) else f"step{idx}", "index": idx,
                      "result": RESULTS.get(res, f"result{res}"), "had": had, "data": rep[at + 6:at + 6 + kept]})
        at += 6 + kept
    if at != len(rep):
        raise ReportError("noreply", f"{len(rep) - at} bytes after the last step")
    return {"run": run, "cut_text": bool(flags & 1), "cut_steps": bool(flags & 2), "text": text,
            "counters": counters, "steps": steps}


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


def show(rep, out):
    print(f"Bluetooth test run {rep['run']}", file=out)
    print(rep["text"], end="" if rep["text"].endswith("\n") else "\n", file=out)
    print("\nCOUNTERS", file=out)
    print(counters_text(rep["counters"]), end="", file=out)
    print("\nSTEPS", file=out)
    for st in rep["steps"]:
        print(f"{st['index'] + 1:2d} {st['step']:9s} {st['result']:7s} {st['had']:2d} bytes  {st['data'].hex()}", file=out)
    if rep["cut_text"] or rep["cut_steps"]:
        print("(the FM-1 cut the report short: it did not fit its buffer)", file=out)


EXIT = {"refused": 1, "usage": 2, "notfound": 3, "noreply": 4, "timeout": 5}


def run(a, backend, out):
    up = fi.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        raise ReportError("notfound", str(fi.not_found(up)))
    try:
        want = start(dev.link, a.wait, a.timeout) if a.run else None
        raw = read(dev.link)
    finally:
        dev.link.close()
    rep = decode(raw)
    if want is not None and rep["run"] != want:
        raise ReportError("refused", f"run {want} did not start (EDIT pressed on the FM-1?): the FM-1 holds run "
                                     f"{rep['run']}'s report")
    show(rep, out)
    if a.out:
        write_out(a.out, rep, raw)
        n = sum(1 for st in rep["steps"] if st["data"])
        print(f"-> {a.out} (report.txt, report.bin, counters.txt, {n} step files)", file=out)
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
