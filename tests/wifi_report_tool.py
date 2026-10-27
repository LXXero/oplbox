#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_wifi_report.py against the device's own code: build/host/slot_sysex_test --serve-wifi REPORT runs
firmware/src/wifi_sysex.c (with slot_sysex.c and ota.c, as oplbox.c composes them) behind a pipe, serving REPORT, the
report the simulator's WiFi page built (HOST_WIFI_REPORT). Also checks that the report's text is the pages' text the
simulator drew (PAGES: its output, "wifi page N: ..." lines) and the tool's decoder on broken reports. Exit 1 when a
check fails.

  tests/wifi_report_tool.py build/host/slot_sysex_test REPORT PAGES OUTDIR
"""
import io
import os
import queue
import subprocess
import sys
import threading
from contextlib import redirect_stderr

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import fm1_slot  # noqa: E402
import fm1_wifi_report as wr  # noqa: E402

fm1_slot.DELAY.update(reply=1.0, idle=3.0)
wr.DELAY.update(reply=1.0, poll=0.01)
BIN, REPORT, PAGES, OUT = sys.argv[1:5]
bad = []


def check(ok, what):
    if not ok:
        bad.append(what)


class PipeLink:
    """fm1_install's link interface on the fake FM-1's stdin / stdout"""

    def __init__(self, proc):
        self.p, self.q = proc, queue.Queue()
        threading.Thread(target=self._rx, daemon=True).start()

    def _rx(self):
        for line in self.p.stdout:
            self.q.put(bytes.fromhex(line.strip()))

    lost = False

    def send(self, pkt):
        self.p.stdin.write(pkt.hex() + "\n")
        self.p.stdin.flush()
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        pass


class PipeBackend:
    def __init__(self, *args):
        self.p = subprocess.Popen([BIN, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.link = PipeLink(self.p)

    def input_names(self):
        return ["FM-1 (pipe)"]

    output_names = input_names

    def open(self, i, o):
        return self.link

    def stop(self):
        self.p.stdin.close()
        self.p.wait(5)


def tool(backend, *argv):
    err, out = io.StringIO(), io.StringIO()
    with redirect_stderr(err):
        rc = wr.main(list(argv), backend=backend, out=out)
    return rc, out.getvalue(), err.getvalue()


raw = open(REPORT, "rb").read()
rep = wr.decode(raw)

# the report's text is what the pages drew, line for line in page order (the footers and blank lines left out)
drawn = []
for line in open(PAGES):
    if " wifi page " in line and ": " in line.split(" wifi page ", 1)[1]:
        n, text = line.split(" wifi page ", 1)[1].rstrip("\n").split(": ", 1)
        if not text.startswith("PLAY: PAGE") and text:
            drawn.append(text)
start = drawn.index("WIFI TEST (STAGE 9)   1/14")                      # the run's page 1, then PLAY to page 14
pages = drawn[start:start + 200]
text = [t for t in rep["text"].split("\n") if t]
first = text[:text.index("ALL NETWORKS: SSID, BSSID, CH, RSSI, SECURITY, P: PROBED")]
check(pages[:len(first)] == first, "the report's text is not the pages' text:\n" +
      "\n".join(f"  page   {a!r}\n  report {b!r}" for a, b in zip(pages, first) if a != b)[:2000])
check(len(first) > 80, f"the report holds {len(first)} page lines")
nets = text[len(first) + 1:]
check([n.split()[0] for n in nets] == ["HOME", "OPLBOX", "OLDROUTER", "(HIDDEN)"], f"all networks: {nets}")
check(rep["counters"]["frames"] == 33 and rep["counters"]["to_me"] == 8 and rep["counters"]["sent"] == 32,
      f"counters: {rep['counters']}")
check(len(rep["frames"]) == 12 and rep["frames"][0]["had"] == 104 and len(rep["frames"][0]["data"]) == 104,
      f"frames: {[(f['step'], f['had'], len(f['data'])) for f in rep['frames']]}")
check(rep["frames"][0]["data"][:36].hex().upper() == "60000000FF004D0000000000C30000000000000080000000FFFFFFFFFFFF020000000001",
      "frame 1's first 36 bytes are not page 7's")
check(not rep["cut_text"] and not rep["cut_frames"], "the simulator's report was cut short")

# the tool against the fake FM-1: nothing run yet, then --run, then --read
dev = PipeBackend("--serve-wifi", REPORT)
rc, out, err = tool(dev, "--read")
check(rc == 1 and "no WiFi test run" in err, f"--read before any run: exit {rc} {err}")
d = os.path.join(OUT, "wifi_report")
rc, out, err = tool(dev, "--run", "--out", d)
check(rc == 0 and "WiFi test run 1" in out and "PASSIVE OK 4 NETWORKS" in out, f"--run: exit {rc} {err}")
check(open(os.path.join(d, "report.txt")).read() == rep["text"], "--out: report.txt is not the report's text")
check(open(os.path.join(d, "report.bin"), "rb").read()[10:] == raw[10:], "--out: report.bin is not the report")
check("wl30_04_before 00000E37" in open(os.path.join(d, "counters.txt")).read(), "--out: counters.txt")
files = sorted(f for f in os.listdir(d) if f.startswith("frame-"))
check(len(files) == 12 and files[0] == "frame-01-passive-ch01.bin" and files[-1].startswith("frame-12-join-"),
      f"--out: frame files {files}")
check(open(os.path.join(d, files[0]), "rb").read() == rep["frames"][0]["data"], "--out: frame 1's bytes")
rc, out, err = tool(dev, "--read")
check(rc == 0 and "WiFi test run 1" in out, f"--read after the run: exit {rc} {err}")
dev.stop()

# --ping (stage 9): the live command during the run, then ping (a stand-in on PATH) at the address the FM-1 gives
pb = os.path.join(OUT, "ping-bin")
os.makedirs(pb, exist_ok=True)
with open(os.path.join(pb, "ping"), "w") as f:
    f.write(f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{pb}/args'\n"
            "echo '5 packets transmitted, 5 received, 0% packet loss'\n")
os.chmod(os.path.join(pb, "ping"), 0o755)
path0 = os.environ["PATH"]
os.environ["PATH"] = pb + os.pathsep + path0
dev = PipeBackend("--serve-wifi", REPORT)
d2 = os.path.join(OUT, "wifi_report_ping")
rc, out, err = tool(dev, "--run", "--ping", "--out", d2)
check(rc == 0 and "pinged the FM-1 at 10.0.0.23" in out and "5 received" in out, f"--run --ping: exit {rc} {err} {out[-300:]}")
check(os.path.exists(os.path.join(pb, "args")) and open(os.path.join(pb, "args")).read().split() ==
      ["-c", "5", "-i", "0.2", "-W", "1", "10.0.0.23"], "--ping: ping's arguments")
check(os.path.exists(os.path.join(d2, "ping.txt")) and "5 received" in open(os.path.join(d2, "ping.txt")).read(), "--ping --out: ping.txt")
dev.stop()
os.environ["PATH"] = path0
try:
    with redirect_stderr(io.StringIO()):
        rc = wr.main(["--ping"], backend=None, out=io.StringIO())
except SystemExit as e:
    rc = e.code
check(rc == 2, f"--ping without --run: exit {rc}")

dev = PipeBackend("--serve-wifi", REPORT, "edit")
rc, out, err = tool(dev, "--run")
check(rc == 1 and "EDIT" in err, f"--run in EDIT: exit {rc} {err}")
dev.stop()
dev = PipeBackend("--serve-wifi", REPORT, "running")
rc, out, err = tool(dev, "--run", "--timeout", "0")
check(rc == 5 and "not finished" in err, f"--run with no time to finish: exit {rc} {err}")
rc, out, err = tool(dev, "--run")
check(rc == 0 and "already going on" in err and "WiFi test run 1" in out, f"--run while one runs: exit {rc} {err}")
dev.stop()
old = PipeBackend("--serve", "--old")                         # a firmware that only answers the handshake
rc, out, err = tool(old, "--read")
check(rc == 4 and "no reply" in err, f"--read from a firmware without the report: exit {rc} {err}")
old.stop()

# --ssid: the network command, the passphrase from a file (only when no one else can read it) or from NetworkManager,
# never printed; refusals before anything is sent; --forget. The fake FM-1 writes what it got to WIFI_NET_OUT.
netout = os.path.join(OUT, "wifi_net.txt")
os.environ["WIFI_NET_OUT"] = netout
if os.path.exists(netout):
    os.remove(netout)
pw = "host test passphrase"
pf = os.path.join(OUT, "wifi.psk")
with open(pf, "w") as f:
    f.write(pw + "\n")
os.chmod(pf, 0o644)
dev = PipeBackend("--serve-wifi", REPORT)
rc, out, err = tool(dev, "--ssid", "TESTWPA2", "--psk-file", pf, "--read")
check(rc == 2 and "chmod 600" in err and not os.path.exists(netout), f"--psk-file readable by others: exit {rc} {err}")
os.chmod(pf, 0o600)
rc, out, err = tool(dev, "--ssid", "TESTWPA2", "--psk-file", pf, "--run")
check(rc == 0 and "network TESTWPA2 set" in out and "WiFi test run 1" in out, f"--ssid --psk-file --run: exit {rc} {err}")
check(os.path.exists(netout) and open(netout).read().split() == [b"TESTWPA2".hex(), pw.encode().hex()],
      "--psk-file: the FM-1 did not get the SSID and the passphrase")
check(pw not in out + err, "--psk-file: the passphrase was printed")
nm = os.path.join(OUT, "nmcli-bin")
os.makedirs(nm, exist_ok=True)
with open(os.path.join(nm, "nmcli"), "w") as f:
    f.write(f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{nm}/args'\n"
            "[ \"$6\" = 'Home WiFi' ] || { echo 'Error: no such connection profile.' >&2; exit 10; }\n"
            "printf 'nm passphrase 1\\n'\n")
os.chmod(os.path.join(nm, "nmcli"), 0o755)
path0 = os.environ["PATH"]
os.environ["PATH"] = nm + os.pathsep + path0
rc, out, err = tool(dev, "--ssid", "TESTWPA2", "--psk-from-nm", "Home WiFi")
check(rc == 0 and open(os.path.join(nm, "args")).read() == "-s\n-g\n802-11-wireless-security.psk\nconnection\nshow\nHome WiFi\n",
      f"--psk-from-nm: exit {rc} {err}, nmcli's arguments")
check(open(netout).read().split()[1] == b"nm passphrase 1".hex() and "nm passphrase" not in out + err,
      "--psk-from-nm: the FM-1 did not get NetworkManager's passphrase, or it was printed")
rc, out, err = tool(dev, "--ssid", "TESTWPA2", "--psk-from-nm", "Office")
check(rc == 2 and "no such connection" in err, f"--psk-from-nm with no such connection: exit {rc} {err}")
os.environ["PATH"] = path0
os.remove(netout)
with open(pf, "w") as f:
    f.write("short\n")
rc, out, err = tool(dev, "--ssid", "TESTWPA2", "--psk-file", pf)
check(rc == 2 and "8-63" in err and not os.path.exists(netout), f"a 5-character passphrase: exit {rc} {err}")
rc, out, err = tool(dev, "--ssid", "X" * 33, "--psk-file", pf)
check(rc == 2 and "1-32" in err and not os.path.exists(netout), f"a 33-byte SSID: exit {rc} {err}")
rc, out, err = tool(dev, "--forget")
check(rc == 0 and "forgotten" in out and open(netout).read().split() == [], f"--forget: exit {rc} {err}")
dev.stop()
del os.environ["WIFI_NET_OUT"]

# the decoder refuses what is not a whole version-1 report
for broken, what in [(b"OPLX" + raw[4:], "not a WiFi report"), (raw[:4] + b"\x02" + raw[5:], "version 2"),
                     (raw[:-1], "ends inside"), (raw + b"\0", "after the last frame"), (raw[:9], "ends inside")]:
    try:
        wr.decode(broken)
        check(False, f"decode took a broken report ({what})")
    except wr.ReportError as e:
        check(what in str(e), f"decode: {e} (want {what})")
cut = bytearray(raw)
cut[5] = 2
out = io.StringIO()
wr.show(wr.decode(bytes(cut)), out)
check("cut the report short" in out.getvalue(), "a cut-short report is not said to be")

for b in bad:
    print("FAIL:", b, file=sys.stderr)
if bad:
    sys.exit(1)
print(f"wifi_report_tool: checks passed (the report's text is the pages', {len(rep['frames'])} frames; "
      "fm1_wifi_report.py --run / --read / refusals against wifi_sysex.c)")
