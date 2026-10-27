#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_ble_report.py against the device's own code: build/host/slot_sysex_test --serve-ble REPORT runs
firmware/src/ble_sysex.c (with slot_sysex.c, wifi_sysex.c and ota.c, as oplbox.c composes them) behind a pipe, serving
REPORT, the report the simulator's Bluetooth page built (HOST_BLE_REPORT, tests/ble/fm1ble_test_host.c's made-up run).
Also checks that the report's text is the pages' text the simulator drew (PAGES: its output, "ble page N: ..." lines),
the step records, a report with a failed step (FAILREPORT: SKIP after it), and the tool's decoder on broken reports.
Exit 1 when a check fails.

  tests/ble_report_tool.py build/host/slot_sysex_test REPORT PAGES FAILREPORT OUTDIR
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
import fm1_ble_report as br  # noqa: E402

fm1_slot.DELAY.update(reply=1.0, idle=3.0)
br.DELAY.update(reply=1.0, poll=0.01)
BIN, REPORT, PAGES, FAIL, OUT = sys.argv[1:6]
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
        rc = br.main(list(argv), backend=backend, out=out)
    return rc, out.getvalue(), err.getvalue()


raw = open(REPORT, "rb").read()
rep = br.decode(raw)

# the report's text is what the pages drew, line for line in page order (the footers and blank lines left out)
drawn = []
for line in open(PAGES):
    if " ble page " in line and ": " in line.split(" ble page ", 1)[1]:
        n, text = line.split(" ble page ", 1)[1].rstrip("\n").split(": ", 1)
        if not text.startswith("PLAY: PAGE") and text:
            drawn.append(text)
start = drawn.index("BLUETOOTH TEST (STAGE 5)   1/12")              # the run's page 1, then PLAY through to page 12
text = [t for t in rep["text"].split("\n") if t]
first = text[:text.index("STEPS IN FULL: NAME, RESULT, MS, NOTE")]
check(drawn[start:start + len(first)] == first, "the report's text is not the pages' text:\n" +
      "\n".join(f"  page   {a!r}\n  report {b!r}" for a, b in zip(drawn[start:], first) if a != b)[:2000])
check(len(first) > 80, f"the report holds {len(first)} page lines")
full = text[len(first) + 1:len(first) + 9]
check([f.split()[0] for f in full] == br.STEPS and all(f.split()[1] == "PASS" for f in full), f"steps in full: {full}")
check(text[-1] == "ADDRESS 02:46:4D:31:00:01", f"the address line: {text[-1]}")
check(rep["counters"]["att_mtu"] == 185 and rep["counters"]["adv_events"] == 37 and rep["counters"]["midi_in"] == 1,
      f"counters: {rep['counters']}")
check(len(rep["counters"]) == 24 and list(rep["counters"])[:2] == ["radio_regs_ok", "radio_regs_bad"], "counter names")
st = {s["step"]: s for s in rep["steps"]}
check([s["step"] for s in rep["steps"]] == br.STEPS and all(s["result"] == "pass" for s in rep["steps"]),
      f"steps: {[(s['step'], s['result']) for s in rep['steps']]}")
check(st["hci"]["had"] == 41 and st["hci"]["data"][:6].hex() == "0e0401030c00" and st["hci"]["data"][-9:].hex() == "0e07010220001b0002",
      "hci evidence: the four events' bytes")
check(st["radio"]["data"].hex() == "0700000098000000" and st["adv_set"]["data"] == b"", "radio evidence, adv_set has none")
check(not rep["cut_text"] and not rep["cut_steps"], "the simulator's report was cut short")

# the tool against the fake FM-1: nothing run yet, then --run, then --read
wait_out = os.path.join(OUT, "ble_wait.txt")
os.environ["BLE_WAIT_OUT"] = wait_out
if os.path.exists(wait_out):
    os.remove(wait_out)
dev = PipeBackend("--serve-ble", REPORT)
rc, out, err = tool(dev, "--read")
check(rc == 1 and "no Bluetooth test run" in err, f"--read before any run: exit {rc} {err}")
d = os.path.join(OUT, "ble_report")
rc, out, err = tool(dev, "--run", "--wait", "3", "--out", d)
check(rc == 0 and "Bluetooth test run 1" in out and "CONNECT  PASS" in out, f"--run: exit {rc} {err}")
check(open(wait_out).read() == "3\n", "--wait 3: the FM-1 was not told 3 s")
check(open(os.path.join(d, "report.txt")).read() == rep["text"], "--out: report.txt is not the report's text")
check(open(os.path.join(d, "report.bin"), "rb").read()[10:] == raw[10:], "--out: report.bin is not the report")
check("att_mtu 185" in open(os.path.join(d, "counters.txt")).read() and "hci_status 00000000" in open(os.path.join(d, "counters.txt")).read(),
      "--out: counters.txt")
files = sorted(f for f in os.listdir(d) if f.startswith("step-"))
check(files == ["step-1-radio.bin", "step-3-hci.bin", "step-6-connect.bin", "step-7-midi_in.bin", "step-8-midi_out.bin"],
      f"--out: step files {files}")
check(open(os.path.join(d, "step-3-hci.bin"), "rb").read() == st["hci"]["data"], "--out: the hci step's bytes")
rc, out, err = tool(dev, "--run", "--out", d)
check(rc == 0 and open(wait_out).read() == "20\n", f"--run without --wait: exit {rc} {err}, told {open(wait_out).read()!r}")
rc, out, err = tool(dev, "--read")
check(rc == 0 and "Bluetooth test run 2" in out, f"--read after the runs: exit {rc} {err}")
dev.stop()

dev = PipeBackend("--serve-ble", REPORT, "edit")
rc, out, err = tool(dev, "--run")
check(rc == 1 and "EDIT" in err, f"--run in EDIT: exit {rc} {err}")
dev.stop()
dev = PipeBackend("--serve-ble", REPORT, "running")
rc, out, err = tool(dev, "--run", "--timeout", "0")
check(rc == 5 and "not finished" in err, f"--run with no time to finish: exit {rc} {err}")
rc, out, err = tool(dev, "--run")
check(rc == 0 and "already going on" in err and "Bluetooth test run 1" in out, f"--run while one runs: exit {rc} {err}")
dev.stop()
old = PipeBackend("--serve", "--old")                         # a firmware that only answers the handshake
rc, out, err = tool(old, "--read")
check(rc == 4 and "no reply" in err, f"--read from a firmware without the report: exit {rc} {err}")
old.stop()
for bad_wait in ("121", "-1"):
    try:
        tool(None, "--run", "--wait", bad_wait)
        check(False, f"--wait {bad_wait} was taken")
    except SystemExit as e:
        check(e.code == 2, f"--wait {bad_wait}: exit {e.code}")
del os.environ["BLE_WAIT_OUT"]

# a run with a failed step: the steps after it are SKIP, and the report says so in its text and its records
fraw = open(FAIL, "rb").read()
frep = br.decode(fraw)
res = [s["result"] for s in frep["steps"]]
check(res == ["pass", "pass", "fail", "skip", "skip", "skip", "skip", "skip"], f"failed run's results: {res}")
ftext = frep["text"].split("\n")
check("HCI      FAIL MADE-UP FAILURE (HOS" in ftext and "ADV_SET  SKIP SKIPPED: AN EARLIER " in ftext, f"failed run's text: {ftext[:12]}")
check(any(t.startswith("hci FAIL") and "made-up failure" in t for t in ftext) and frep["counters"]["hci_status"] == 0x12,
      "failed run: the full note and the status counter")
out = io.StringIO()
br.show(frep, out)
check("hci       fail" in out.getvalue() and "adv_set   skip" in out.getvalue(), "show: the steps' results")

# the decoder refuses what is not a whole version-1 report
for broken, what in [(b"OPLX" + raw[4:], "not a Bluetooth report"), (raw[:4] + b"\x02" + raw[5:], "version 2"),
                     (raw[:-1], "ends inside"), (raw + b"\0", "after the last step"), (raw[:9], "ends inside")]:
    try:
        br.decode(broken)
        check(False, f"decode took a broken report ({what})")
    except br.ReportError as e:
        check(what in str(e), f"decode: {e} (want {what})")
wifi = b"OPLW" + raw[4:]
try:
    br.decode(wifi)
    check(False, "decode took a WiFi report")
except br.ReportError:
    pass
cut = bytearray(raw)
cut[5] = 2
out = io.StringIO()
br.show(br.decode(bytes(cut)), out)
check("cut the report short" in out.getvalue(), "a cut-short report is not said to be")

for b in bad:
    print("FAIL:", b, file=sys.stderr)
if bad:
    sys.exit(1)
print(f"ble_report_tool: checks passed (the report's text is the pages', {len(rep['steps'])} steps, a failed run's SKIPs; "
      "fm1_ble_report.py --run / --read / refusals against ble_sysex.c)")
