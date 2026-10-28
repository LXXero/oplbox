#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/ble_central.py against stand-ins for bluetoothctl, aseqdump and aseqsend (tests/ble/fake_bluez/, made up from
those tools' output formats, not recorded): the trial addresses heard and the others left out, the RSSI in both of
bluetoothctl's forms, the prompt and colours stripped, connect, the new sequencer port found, the Note On sent and the
FM-1's heard, disconnect; and the stack never heard, a connection refused, no MIDI port, no bluetoothctl. Then
tools/fm1_ble_report.py's summary of a central's result. Exit 1 when a check fails.

  tests/ble_central_tool.py OUTDIR
"""
import io
import json
import os
import shutil
import sys
from contextlib import redirect_stderr, redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import ble_central as bc  # noqa: E402
import fm1_ble_report as br  # noqa: E402

OUT = sys.argv[1]
bad = []
bc.STEP.update(connect=3.0, port=2.0, notify=3.0, settle=0.05, quit=2.0, again=0.05)


def check(ok, what):
    if not ok:
        bad.append(what)


def central(mode, *extra):
    st = os.path.join(OUT, "fake_bluez_" + mode)
    shutil.rmtree(st, ignore_errors=True)
    os.makedirs(st)
    os.environ.update(FAKE_BLUEZ_STATE=st, FAKE_BLUEZ_MODE=mode, PATH=os.path.join(HERE, "ble", "fake_bluez") + os.pathsep + PATH)
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        rc = bc.main(["--stack", br.STACK_ADDR, "--trials", ",".join(a for a, _ in br.TRIALS), "--timeout", "8", "--json", *extra])
    cmds = open(os.path.join(st, "cmds.txt")).read().split("\n") if os.path.exists(os.path.join(st, "cmds.txt")) else []
    sent = open(os.path.join(st, "sent")).read() if os.path.exists(os.path.join(st, "sent")) else ""
    return rc, json.loads(out.getvalue().strip().splitlines()[-1]), cmds, sent, err.getvalue()


PATH = os.environ["PATH"]
rc, r, cmds, sent, err = central("ok")
check(rc == 0, f"ok: exit {rc}")
check(sorted(r["seen"]) == ["C2:46:4D:31:00:01", "C2:46:4D:31:00:10", "C2:46:4D:31:00:11"],
      f"ok: the trials heard and the stack, nothing else: {sorted(r['seen'])}")
check(r["seen"]["C2:46:4D:31:00:10"]["rssi"] == -67 and r["seen"]["C2:46:4D:31:00:11"]["rssi"] == -71,
      f"ok: the RSSI in both forms: {r['seen']}")
check(r["seen"]["C2:46:4D:31:00:11"]["name"] == "OPLBOX", "ok: a device's name, after bluetoothctl's prompt")
check(r["stack_seen"] and r["connected"] and r["port"] == "128:0" and r["port_name"] == "OPLBOX / OPLBOX Bluetooth",
      f"ok: connected, BlueZ's port for the FM-1: {r}")
check(r["sent"] and sent == "-p 128:0 90 3C 64\n", f"ok: aseqsend got the Note On for that port: {sent!r}")
check(len(r["received"]) == 1 and "Note on" in r["received"][0] and "note 60, velocity 100" in r["received"][0],
      f"ok: the FM-1's Note On heard: {r['received']}")
check(r["errors"] == [] and r["services"] and r["dropped"] is None, f"ok: no errors, services resolved: {r}")
check(cmds[:4] == ["scan le", "scan off", "connect C2:46:4D:31:00:01", "disconnect C2:46:4D:31:00:01"] and "quit" in cmds,
      f"ok: the commands in order: {cmds}")
check("heard C2:46:4D:31:00:10" in err and "connected" in err, "ok: progress on stderr")

rc, r, cmds, sent, err = central("nostack")
check(rc == 0 and not r["stack_seen"] and not r["connected"] and len(r["seen"]) == 2 and "was not heard" in r["errors"][0],
      f"nostack: {r}")
check("connect C2:46:4D:31:00:01" not in cmds and cmds[-2:] == ["quit", ""], f"nostack: no connect, quit: {cmds}")

rc, r, cmds, sent, err = central("noconnect")
check(r["stack_seen"] and not r["connected"] and r["errors"][0].startswith("connect: Failed to connect"), f"noconnect: {r}")
check("disconnect C2:46:4D:31:00:01" not in cmds, "noconnect: no disconnect")

rc, r, cmds, sent, err = central("nomidi")
check(r["connected"] and r["services"] and r["port"] is None and "services resolved, but no ALSA sequencer port" in r["errors"][0]
      and not sent, f"nomidi: {r}")
check("disconnect C2:46:4D:31:00:01" in cmds, "nomidi: disconnected anyway")

# connected as bluetoothctl says it (CONNECT_IND sent), but the FM-1 never came up in the connection
rc, r, cmds, sent, err = central("noservices")
check(r["connected"] and not r["services"] and r["port"] is None and len(r["errors"]) == 1 and
      r["errors"][0].startswith("the connection never came up on the FM-1's side: no GATT services resolved") and
      "MIDI" not in r["errors"][0], f"noservices: the connection, not BlueZ's MIDI, named: {r}")
rc, r, cmds, sent, err = central("dropped")
check(r["connected"] and r["dropped"] is not None and r["port"] is None and len(r["errors"]) == 1 and
      "never came up on the FM-1's side: BlueZ dropped it" in r["errors"][0] and "before resolving" in r["errors"][0],
      f"dropped: {r}")
check("dropped the connection" in err, "dropped: said on stderr")
check(len(r["attempts"]) == 7 and all(t["connected"] and t["dropped"] is not None for t in r["attempts"]) and
      cmds.count("connect C2:46:4D:31:00:01") == 7, f"dropped: connected again after each drop, 7 connects in all: {r['attempts']}")
out = io.StringIO()
br.show_central(r, out)
check("GATT services resolved: no; dropped: after" in out.getvalue() and "attempt 7 at" in out.getvalue() and
      "connected, GATT services not resolved, dropped after" in out.getvalue(), f"show_central, a drop:\n{out.getvalue()}")
rc, r, cmds, sent, err = central("dropped", "--tries", "1")
check(len(r["attempts"]) == 1 and cmds.count("connect C2:46:4D:31:00:01") == 1, f"dropped, --tries 1: one connect: {cmds}")
# the first two connections drop (the FM-1's first two window setups miss), the third comes up: the MIDI part runs on it
rc, r, cmds, sent, err = central("dropfirst")
check(r["connected"] and r["services"] and r["dropped"] is None and r["port"] == "128:0" and r["sent"] and len(r["received"]) == 1
      and r["errors"] == [] and len(r["attempts"]) == 3 and [t["dropped"] is not None for t in r["attempts"]] == [True, True, False],
      f"dropfirst: the third connect comes up and the Note On goes both ways: {r}")
check(cmds.count("connect C2:46:4D:31:00:01") == 3, f"dropfirst: three connects: {cmds}")
# BlueZ makes the MIDI port before its discovery ends: ServicesResolved: yes comes after the port (and here after the
# Note On), and the central goes on reading for it before it disconnects
rc, r, cmds, sent, err = central("lateservices")
check(r["services"] and r["services_at"] is not None and r["services_at"] >= 0.4 and r["port"] == "128:0" and
      r["sent"] and len(r["received"]) == 1 and r["errors"] == [] and r["attempts"][-1]["services"] and
      r["dropped"] is None, f"lateservices: services resolved after the port, still read: {r}")
check("GATT services resolved" in err and cmds.count("disconnect C2:46:4D:31:00:01") == 1,
      f"lateservices: said on stderr, then one disconnect: {cmds}")
out = io.StringIO()
br.show_central(r, out)
check("GATT services resolved: yes (" in out.getvalue() and "s after the connect); dropped: no" in out.getvalue(),
      f"show_central, services after the port:\n{out.getvalue()}")
noservices_cmds = central("noservices")[2]
check(noservices_cmds.count("disconnect C2:46:4D:31:00:01") >= 2, f"noservices: closed before connecting again: {noservices_cmds}")

out = io.StringIO()
with redirect_stdout(out), redirect_stderr(io.StringIO()):
    rc = bc.main(["--stack", br.STACK_ADDR, "--json", "--bluetoothctl", "no-such-bluetoothctl"])
check(rc == 3 and "no bluetoothctl" in out.getvalue(), f"no bluetoothctl: exit {rc} {out.getvalue()}")
try:
    with redirect_stderr(io.StringIO()):
        bc.main(["--stack", "C2:46:4D:31:00"])
    check(False, "a short address was taken")
except SystemExit as e:
    check(e.code == 2, f"a short address: exit {e.code}")
os.environ["PATH"] = PATH

# the report tool's summary of a central's result
rc, r, cmds, sent, err = central("ok")
os.environ["PATH"] = PATH
out = io.StringIO()
br.show_central(r, out)
o = out.getvalue()
check("C2:46:4D:31:00:10  variant 0 (no shared radio init), level 6: seen at" in o and
      "C2:46:4D:31:00:12  variant 1, level 11: not seen" in o and "C2:46:4D:31:00:01  the stack (Cordio, NimBLE): seen" in o and
      "connect: yes; MIDI port: 128:0" in o and "Note On sent: yes" in o, f"show_central:\n{o}")
out = io.StringIO()
br.show_central({"error": "the central on xlap did not finish", "stderr": "ssh: connect to host xlap"}, out)
check("error: the central on xlap did not finish" in out.getvalue() and "ssh: connect" in out.getvalue() and
      "not seen" in out.getvalue(), f"show_central, an error:\n{out.getvalue()}")

for b in bad:
    print("FAIL:", b, file=sys.stderr)
if bad:
    sys.exit(1)
print("ble_central_tool: checks passed (scan, the trials' addresses, connect, the MIDI port, Note On both ways, the failures)")
