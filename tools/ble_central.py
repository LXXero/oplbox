#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A BlueZ central for the FM-1's Bluetooth test run (docs/BLUETOOTH_PAGE.md "A central on the bench").

  ble_central.py --stack ADDR [--trials ADDR,ADDR,...] [--timeout S] [--json]

Runs on a Linux host with BlueZ (bluetoothctl) and, for the MIDI part, BlueZ's MIDI support and alsa-utils (aseqdump,
aseqsend). It scans (LE) and logs every advertisement it hears from the given addresses, with the time it first heard
each (seconds from the start) and its RSSI; when the stack's address shows up it stops scanning, connects, waits for
the ALSA sequencer port BlueZ makes for a BLE-MIDI device, captures it with aseqdump, sends a Note On (90 3C 64) with
aseqsend, waits for the FM-1's own Note On to come back as a notification, and disconnects. Everything is bounded by
--timeout (default 120 s).

With --json the last line on stdout is the result as JSON: seen {addr: {first, last, rssi, name}}, stack_seen,
connected, services (BlueZ resolved the FM-1's GATT services), dropped (seconds after the connect when BlueZ said
Connected: no, or null), port, sent, received (the aseqdump lines from the FM-1), errors. Without, a short summary.

bluetoothctl says "Connection successful" on LE once its controller has sent the CONNECT_IND, whether or not the
peripheral got it. So no MIDI port after a connect is first a question of the connection: with no GATT services resolved
(ServicesResolved: yes never came) or a drop, the connection never came up on the FM-1's side, and the result says so;
only with the services resolved and still no port is it BlueZ's MIDI side.

tools/fm1_ble_report.py --run --central HOST starts this on HOST over ssh (`ssh HOST python3 - ARGS` with this file on
stdin), so the run on the FM-1 and the central's side come back as one report. Exit code 0 when it finished (whatever
it found), 2 for bad arguments, 3 when bluetoothctl is missing.
"""
import argparse
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import threading
import time

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]|\x01|\x02|\r")
DEVICE = re.compile(r"\[(NEW|CHG|DEL)\]\s+Device\s+([0-9A-Fa-f:]{17})\s*(.*)")
PORT = re.compile(r"^\s*(\d+):(\d+)\s+(.*?)\s{2,}(.*)$")
STEP = {"connect": 25.0, "port": 10.0, "notify": 15.0, "settle": 0.5, "quit": 3.0}   # seconds; the tests shorten them


class Ctl:
    """bluetoothctl in interactive mode, its output read line by line in a thread"""

    def __init__(self, exe="bluetoothctl"):
        self.p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, bufsize=1)
        self.q = queue.Queue()
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        for line in self.p.stdout:
            line = ANSI.sub("", line).strip()
            # the prompt ("[bluetooth]# ", "[OPLBOX]> ") may run into the line before the message
            line = re.sub(r"^(\[[^\]]*\][#>]\s*)+", "", line)
            if line:
                self.q.put((time.monotonic(), line))
        self.q.put((time.monotonic(), None))

    def send(self, cmd):
        try:
            self.p.stdin.write(cmd + "\n")
            self.p.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    def lines(self, until):
        """yield (t, line) until the monotonic time `until`, or bluetoothctl ends (line None)"""
        while True:
            left = until - time.monotonic()
            if left <= 0:
                return
            try:
                t, line = self.q.get(timeout=left)
            except queue.Empty:
                return
            yield t, line
            if line is None:
                return

    def close(self):
        self.send("quit")
        try:
            self.p.wait(STEP["quit"])
        except subprocess.TimeoutExpired:
            self.p.kill()


def seq_ports():
    """{"client:port": "client name / port name"} from aseqdump -l, or None without aseqdump"""
    if not shutil.which("aseqdump"):
        return None
    out = subprocess.run(["aseqdump", "-l"], capture_output=True, text=True, timeout=10).stdout
    ports = {}
    for line in out.splitlines()[1:]:
        m = PORT.match(line)
        if m:
            ports[f"{m.group(1)}:{m.group(2)}"] = f"{m.group(3).strip()} / {m.group(4).strip()}"
    return ports


def run(a, log):
    t0 = time.monotonic()
    end = t0 + a.timeout
    stack = a.stack.upper()
    watch = {stack, *(x.upper() for x in a.trials)}
    res = {"seen": {}, "stack_seen": False, "connected": False, "services": False, "dropped": None, "port": None,
           "sent": False, "received": [], "errors": []}

    def at(t):
        return round(t - t0, 2)

    def device(t, line):
        m = DEVICE.search(line)
        if not m:
            return None
        addr, rest = m.group(2).upper(), m.group(3)
        if m.group(1) == "DEL":                                    # BlueZ dropping a cached device is not a sighting
            return None
        if addr not in watch:
            return addr
        d = res["seen"].setdefault(addr, {"first": at(t), "last": at(t), "rssi": None, "name": None})
        d["last"] = at(t)
        r = re.search(r"RSSI:\s*(?:0x[0-9a-fA-F]+\s*)?\(?(-?\d+)\)?", rest)
        if r:
            d["rssi"] = int(r.group(1))
        elif m.group(1) == "NEW" and rest:
            d["name"] = rest
        return addr

    ctl = Ctl(a.bluetoothctl)
    try:
        ports0 = seq_ports()
        ctl.send("scan le")
        log(f"{at(time.monotonic()):6.2f} scanning (LE) for {', '.join(sorted(watch))}")
        for t, line in ctl.lines(end):
            if line is None:
                res["errors"].append("bluetoothctl ended while scanning")
                return res
            if "Invalid argument" in line or "Usage:" in line:       # an old bluetoothctl without "scan le"
                ctl.send("scan on")
            addr = device(t, line)
            if addr in watch and res["seen"].get(addr, {}).get("first") == at(t):
                log(f"{at(t):6.2f} heard {addr}")
            if addr == stack:
                res["stack_seen"] = True
                break
        if not res["stack_seen"]:
            res["errors"].append(f"the stack's address {stack} was not heard in {a.timeout:g} s")
            return res
        ctl.send("scan off")
        ctl.send(f"connect {stack}")
        log(f"{at(time.monotonic()):6.2f} connecting to {stack}")
        for t, line in ctl.lines(min(end, time.monotonic() + STEP["connect"])):
            if line is None:
                break
            device(t, line)
            if re.search(r"ServicesResolved:\s*yes", line):
                res["services"] = True
            if "Connection successful" in line or re.search(r"Connected:\s*yes", line):
                res["connected"] = True
                break
            if "Failed to connect" in line or "not available" in line:
                res["errors"].append(f"connect: {line}")
                break
        if not res["connected"]:
            if not any(e.startswith("connect:") for e in res["errors"]):
                res["errors"].append("connect: no answer")
            return res
        t_conn = time.monotonic()
        log(f"{at(t_conn):6.2f} connected")
        port_end = min(end, t_conn + STEP["port"])
        while time.monotonic() < port_end and not res["port"] and res["dropped"] is None:
            for t, line in ctl.lines(min(port_end, time.monotonic() + 0.2)):     # BlueZ's word on the connection meanwhile
                if line is None:
                    break
                device(t, line)
                if re.search(r"ServicesResolved:\s*yes", line):
                    res["services"] = True
                if re.search(r"Connected:\s*no", line):
                    res["dropped"] = round(t - t_conn, 2)
                    log(f"{at(t):6.2f} BlueZ dropped the connection")
                    break
            if ports0 is not None and res["dropped"] is None:
                new = {k: v for k, v in (seq_ports() or {}).items() if k not in ports0}
                if new:
                    res["port"] = sorted(new)[0]
                    res["port_name"] = new[res["port"]]
        if res["dropped"] is not None:
            res["errors"].append(f"the connection never came up on the FM-1's side: BlueZ dropped it {res['dropped']:g} s "
                                 f"after sending CONNECT_IND, {'after' if res['services'] else 'before'} resolving GATT services "
                                 "(the FM-1 did not receive the CONNECT_IND or did not answer in the connection)")
            return res
        if not res["services"] and not res["port"]:
            res["errors"].append(f"the connection never came up on the FM-1's side: no GATT services resolved in "
                                 f"{STEP['port']:g} s (bluetoothctl says connected once it has sent CONNECT_IND)")
            return res
        if ports0 is None:
            res["errors"].append("no aseqdump (alsa-utils): the MIDI part is left out")
            return res
        if not res["port"]:
            res["errors"].append("GATT services resolved, but no ALSA sequencer port came for the FM-1 (BlueZ's MIDI plugin?)")
            return res
        log(f"{at(time.monotonic()):6.2f} MIDI port {res['port']} ({res['port_name']})")
        dump = subprocess.Popen(["aseqdump", "-p", res["port"]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, bufsize=1)
        got = queue.Queue()
        threading.Thread(target=lambda: [got.put(x.rstrip("\n")) for x in dump.stdout], daemon=True).start()
        try:
            time.sleep(STEP["settle"])
            if not shutil.which("aseqsend"):
                res["errors"].append("no aseqsend (alsa-utils 1.2.11 or later): the Note On was not sent")
            else:
                r = subprocess.run(["aseqsend", "-p", res["port"], "90", "3C", "64"], capture_output=True, text=True,
                                   timeout=10)
                res["sent"] = r.returncode == 0
                if r.returncode:
                    res["errors"].append(f"aseqsend: exit {r.returncode} {r.stderr.strip()}")
                log(f"{at(time.monotonic()):6.2f} Note On 90 3C 64 sent")
            note_end = min(end, time.monotonic() + STEP["notify"])
            while time.monotonic() < note_end:
                try:
                    line = got.get(timeout=max(0.05, note_end - time.monotonic()))
                except queue.Empty:
                    break
                if re.search(r"Note (on|off)", line, re.I):
                    res["received"].append(line.strip())
                    log(f"{at(time.monotonic()):6.2f} from the FM-1: {line.strip()}")
                    if re.search(r"Note on", line, re.I):
                        break
        finally:
            dump.kill()
            dump.wait()
        return res
    finally:
        if res["connected"]:
            ctl.send(f"disconnect {stack}")
            for _, line in ctl.lines(time.monotonic() + STEP["quit"]):
                if line is None or "Successful disconnected" in line or re.search(r"Connected:\s*no", line or ""):
                    break
        ctl.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stack", required=True, metavar="ADDR", help="the address the stack advertises from")
    ap.add_argument("--trials", default="", metavar="ADDR,...", help="more addresses to log (the air trials)")
    ap.add_argument("--timeout", type=float, default=120.0, metavar="S", help="the most it runs (default 120 s)")
    ap.add_argument("--json", action="store_true", help="the result as JSON on the last line of stdout")
    ap.add_argument("--bluetoothctl", default="bluetoothctl", help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    a.trials = [x for x in a.trials.split(",") if x]
    for x in [a.stack, *a.trials]:
        if not re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", x):
            ap.error(f"not an address: {x}")
    if not shutil.which(a.bluetoothctl):
        print("ble_central: no bluetoothctl here (BlueZ)", file=sys.stderr)
        if a.json:
            print(json.dumps({"error": "no bluetoothctl on this host"}))
        return 3
    res = run(a, lambda m: print(m, file=sys.stderr, flush=True))
    if a.json:
        print(json.dumps(res))
    else:
        for addr, d in sorted(res["seen"].items()):
            print(f"{addr}: first heard at {d['first']} s, RSSI {d['rssi']}")
        print(f"connected: {res['connected']}, port: {res['port']}, sent: {res['sent']}, received: {res['received']}")
        for e in res["errors"]:
            print(e)
    return 0


if __name__ == "__main__":
    sys.exit(main())
