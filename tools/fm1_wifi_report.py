#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read the WiFi test page's results from an FM-1 over USB-MIDI, or start a run first.

  fm1_wifi_report.py [--read] [--out DIR] [--port NAME]         the last run's report
  fm1_wifi_report.py --run [--timeout S] [--out DIR] [--port NAME]  start a run, wait for it, then the report

SysEx in the updater's framing (firmware/src/wifi_sysex.c, docs/API.md "WiFi test page
over SysEx"). The report is what the page's ten pages show, as text, then every network
the scan kept, the counters behind page 9 and every frame the FIFO gave (up to 512 bytes
each), as bytes. It prints the text and the counters. With --out DIR it writes
report.txt, report.bin (the report as sent), counters.txt and one file per frame,
frame-NN-<step>-chNN.bin (the bytes as the FIFO gave them: length word, RXWI, frame,
trailer), for parser fixtures.

--run starts a run as SAVE then PLAY on the FM-1 would (the sound stops; the page stays
open afterwards, SAVE leaves it). The FM-1 refuses while EDIT is open. Finds the FM-1 the
way fm1_install.py does. Needs mido with python-rtmidi.

Exit codes: 0 done, 1 the FM-1 refused or has no report (no run since it started),
2 bad arguments, 3 FM-1 not found, 4 no reply (connection lost, or a firmware without
the WiFi report), 5 the run did not finish within --timeout.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm1_install as fi  # noqa: E402
from fm1_slot import SlotError, message, wait_reply  # noqa: E402

REPORT, RUN = 0x62, 0x63
OK, E_REQ, E_NONE, E_RUNNING, E_EDIT = range(5)
STATUS = {E_REQ: "bad request", E_NONE: "no WiFi test run since the FM-1 started (--run starts one)",
          E_RUNNING: "a run is going on", E_EDIT: "EDIT is open on the FM-1: leave it (EDIT) and try again"}
STEPS = {1: "passive", 2: "active", 3: "join"}
COUNTERS = ["wl30_04_before", "wl30_04_after", "txrxq_before", "txrxq_after", "frames", "bad", "to_me", "sent",
            "send_errors", "crc_err", "phy_err", "rx_dup", "rx_overflow", "tx_ok", "tx_fail", "tx_retry",
            "tx_underflow", "beacons", "probe_responses"]
HEX_COUNTERS = {"wl30_04_before", "wl30_04_after", "txrxq_before", "txrxq_after"}
DELAY = {"reply": 3.0, "poll": 0.5}   # seconds; the tests shorten them


class ReportError(SlotError):
    pass


def decode(rep):
    """the report's bytes -> dict(run, cut_text, cut_frames, text, counters, frames); ReportError if malformed"""
    def need(n, what):
        if len(rep) < n:
            raise ReportError("noreply", f"the report ends inside {what} ({len(rep)} bytes)")

    need(10, "its header")
    if rep[:4] != b"OPLW":
        raise ReportError("noreply", f"not a WiFi report (starts {rep[:4].hex()})")
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
    nf = rep[at]
    at += 1
    frames = []
    for i in range(nf):
        need(at + 6, f"frame {i + 1}'s header")
        step, ch = rep[at], rep[at + 1]
        had, kept = int.from_bytes(rep[at + 2:at + 4], "little"), int.from_bytes(rep[at + 4:at + 6], "little")
        need(at + 6 + kept, f"frame {i + 1}'s bytes")
        frames.append({"step": STEPS.get(step, f"step{step}"), "ch": ch, "had": had, "data": rep[at + 6:at + 6 + kept]})
        at += 6 + kept
    if at != len(rep):
        raise ReportError("noreply", f"{len(rep) - at} bytes after the last frame")
    return {"run": run, "cut_text": bool(flags & 1), "cut_frames": bool(flags & 2), "text": text,
            "counters": counters, "frames": frames}


def ask(link, cmd, body):
    for _ in range(3):
        if not link.send(message(cmd, body)):
            raise ReportError("noreply", "the MIDI port is gone")
        try:
            return wait_reply(link, cmd, DELAY["reply"])
        except SlotError:
            continue
    raise ReportError("noreply", "no reply from the FM-1 (unplugged, or a firmware without the WiFi report?)")


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


def start(link, timeout):
    """start a run and wait for its report; -> the run's number"""
    b = ask(link, RUN, b"")
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


def write_out(d, rep, raw):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "report.bin"), "wb") as f:
        f.write(raw)
    with open(os.path.join(d, "report.txt"), "w") as f:
        f.write(rep["text"])
    with open(os.path.join(d, "counters.txt"), "w") as f:
        f.write(counters_text(rep["counters"]))
    for i, fr in enumerate(rep["frames"], 1):
        with open(os.path.join(d, f"frame-{i:02d}-{fr['step']}-ch{fr['ch']:02d}.bin"), "wb") as f:
            f.write(fr["data"])


def show(rep, out):
    print(f"WiFi test run {rep['run']}", file=out)
    print(rep["text"], end="" if rep["text"].endswith("\n") else "\n", file=out)
    print("\nCOUNTERS", file=out)
    print(counters_text(rep["counters"]), end="", file=out)
    print(f"\nFRAMES {len(rep['frames'])}", file=out)
    for i, fr in enumerate(rep["frames"], 1):
        cut = "" if len(fr["data"]) >= fr["had"] else f", first {len(fr['data'])} kept"
        print(f"{i:2d} {fr['step']:7s} ch {fr['ch']:2d}  {fr['had']} bytes{cut}  {fr['data'][:36].hex()}", file=out)
    if rep["cut_text"] or rep["cut_frames"]:
        print("(the FM-1 cut the report short: it did not fit its buffer)", file=out)


EXIT = {"refused": 1, "usage": 2, "notfound": 3, "noreply": 4, "timeout": 5}


def run(a, backend, out):
    up = fi.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        raise ReportError("notfound", str(fi.not_found(up)))
    try:
        want = start(dev.link, a.timeout) if a.run else None
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
        print(f"-> {a.out} (report.txt, report.bin, counters.txt, {len(rep['frames'])} frames)", file=out)
    return 0


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--read", action="store_true", help="read the last run's report (the default)")
    g.add_argument("--run", action="store_true", help="start a run, wait for it, then read its report")
    ap.add_argument("--timeout", type=float, default=60.0, metavar="S", help="--run: seconds to wait (default 60)")
    ap.add_argument("--out", metavar="DIR", help="write the text, the counters and each frame's bytes into DIR")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    a = ap.parse_args(argv)
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
