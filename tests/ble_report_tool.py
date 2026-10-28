#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_ble_report.py against the device's own code: build/host/slot_sysex_test --serve-ble REPORT runs
firmware/src/ble_sysex.c (with slot_sysex.c, wifi_sysex.c and ota.c, as oplbox.c composes them) behind a pipe, serving
REPORT, the report the simulator's Bluetooth page built (HOST_BLE_REPORT, tests/ble/fm1ble_test_host.c's made-up run).
Also checks that the report's text is the pages' text the simulator drew (PAGES: its output, "ble page N: ..." lines),
the step records, a report with a failed step (FAILREPORT: SKIP after it), and the tool's decoder on broken reports.
Also: the tool's counter names against the firmware's (firmware/ble/fm1ble_test.h FM1BLE_CTR_NAMES) and its trial
addresses against the run's (fm1ble_test.h, fm1ble_test.c), the first FM-1 run's report (tests/ble/report_48ea034.bin,
recorded from the device at oplbox 48ea034: 24 counters, two of the PAL's in a word) read with that firmware's names, and
--run --central local with the stand-in BlueZ tools of tests/ble/fake_bluez/. Exit 1 when a check fails.

  tests/ble_report_tool.py build/host/slot_sysex_test REPORT PAGES FAILREPORT OUTDIR
"""
import io
import os
import queue
import re
import subprocess
import sys
import threading
from contextlib import redirect_stderr

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import fm1_slot  # noqa: E402
import fm1_ble_report as br  # noqa: E402

fm1_slot.DELAY.update(reply=1.0, idle=3.0)
br.DELAY.update(reply=1.0, poll=0.01, central=0.1)
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
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


# the tool's names are the firmware's, and its addresses the run's
hdr = open(os.path.join(ROOT, "firmware/ble/fm1ble_test.h")).read()
names = re.findall(r'"([a-z0-9_]+)"', hdr[hdr.index("#define FM1BLE_CTR_NAMES"):].split("\n\n")[0])
check(names == br.COUNTERS, f"the tool's counters are not the firmware's FM1BLE_CTR_NAMES:\n  tool     {br.COUNTERS}\n  firmware {names}")
first = int(re.search(r"#define FM1BLE_TRIAL_ADDR0\s+0x([0-9A-Fa-f]+)u", hdr).group(1), 16)
ntr = int(re.search(r"#define FM1BLE_TRIALS\s+(\d+)", hdr).group(1))
src = open(os.path.join(ROOT, "firmware/ble/fm1ble_test.c")).read()
addr = [int(x, 16) for x in re.search(r"g_addr\[6\] = \{([^}]*)\}", src).group(1).replace("0x", "").split(",")]
mac = lambda b: ":".join(f"{x:02X}" for x in reversed(b))  # noqa: E731
check(br.STACK_ADDR == mac(addr) and [a for a, _ in br.TRIALS] == [mac([first + k] + addr[1:]) for k in range(ntr)],
      f"the tool's addresses are not the run's: {br.STACK_ADDR} {br.TRIALS}")
nrx = int(re.search(r"#define FM1BLE_RX_TRIALS\s+(\d+)", hdr).group(1))
check([a for a, _ in br.RX_TRIALS] == [mac([first + ntr + k] + addr[1:]) for k in range(nrx)],
      f"the tool's RX trial addresses are not the run's: {br.RX_TRIALS}")
nctr = int(re.search(r"#define FM1BLE_N_CTR\s+(\d+)", hdr).group(1))
check(len(br.COUNTERS) == nctr and nctr % 8 == 0, f"{len(br.COUNTERS)} counters, the firmware sends {nctr} (8 to a page)")

# the first FM-1 run's report, from the device: the 24 counters of that firmware, by their own names
old = br.decode(open(os.path.join(ROOT, "tests/ble/report_48ea034.bin"), "rb").read())
oc = old["counters"]
check(oc["events"] == 2569 and oc["events_v0"] == 4 and oc["pal_adv_ops"] == 7695 and oc["pal_conn_ops"] == 0 and
      oc["pal_rx_timeout"] == 7695 and oc["pal_stuck"] == 5130 and oc["pal_timer_late_us"] == 293 and "midi_out" not in oc,
      f"the 48ea034 report's counters: {oc}")
check([s["result"] for s in old["steps"]][:2] == ["pass", "fail"] and br.air_trials(old) is None,
      "the 48ea034 report: block FAIL, no air trials in it")

raw = open(REPORT, "rb").read()
rep = br.decode(raw)

# the report's text is what the pages drew, line for line in page order (the footers and blank lines left out)
drawn = []
for line in open(PAGES):
    if " ble page " in line and ": " in line.split(" ble page ", 1)[1]:
        n, text = line.split(" ble page ", 1)[1].rstrip("\n").split(": ", 1)
        if not text.startswith("PLAY: PAGE") and text:
            drawn.append(text)
npages = 1 + len(br.STEPS) + nctr // 8
start = drawn.index(f"BLUETOOTH TEST (STAGE 5)   1/{npages}")      # the run's page 1, then PLAY through to the last
text = [t for t in rep["text"].split("\n") if t]
first = text[:text.index("STEPS IN FULL: NAME, RESULT, MS, NOTE")]
check(drawn[start:start + len(first)] == first, "the report's text is not the pages' text:\n" +
      "\n".join(f"  page   {a!r}\n  report {b!r}" for a, b in zip(drawn[start:], first) if a != b)[:2000])
check(len(first) > 80, f"the report holds {len(first)} page lines")
full = text[len(first) + 1:len(first) + 9]
check([f.split()[0] for f in full] == br.STEPS and all(f.split()[1] == "PASS" for f in full), f"steps in full: {full}")
check(text[-1] == "ADDRESS 02:46:4D:31:00:01", f"the address line: {text[-1]}")
check(rep["counters"]["pal_rx_ok"] == 185 and rep["counters"]["events"] == 37 and rep["counters"]["air_trials"] == 7 and
      rep["counters"]["pal_done_39"] == 3, f"counters: {rep['counters']}")
check(list(rep["counters"]) == br.COUNTERS, "counter names")
st = {s["step"]: s for s in rep["steps"]}
check([s["step"] for s in rep["steps"]] == br.STEPS and all(s["result"] == "pass" for s in rep["steps"]),
      f"steps: {[(s['step'], s['result']) for s in rep['steps']]}")
check(st["hci"]["had"] == 41 and st["hci"]["data"][:6].hex() == "0e0401030c00" and st["hci"]["data"][-9:].hex() == "0e07010220001b0002",
      "hci evidence: the four events' bytes")
check(st["radio"]["data"].hex() == "0700000098000000" and st["adv_set"]["data"] == b"", "radio evidence, adv_set has none")
check(not rep["cut_text"] and not rep["cut_steps"], "the simulator's report was cut short")
check(f"COUNTERS {nctr // 8}/{nctr // 8}   {npages}/{npages}" in text and "RX_STAT1" in rep["text"],
      "the report holds every counter page, the last counter included")

# the RX trials from their counters: a trial that received, one that did not, the stack's setup
rx = {"counters": dict(rep["counters"]), "steps": rep["steps"]}
rx["counters"].update({f"rxt{k}_{w}": 0 for k in range(4) for w in ("counts", "stat", "pkt", "bytes")})
rx["counters"].update(rxt0_counts=0x1D, rxt0_stat=0x00009800, rxt0_pkt=0x27FF0000, rxt2_counts=0x07071D1D, rxt2_stat=0x00030001,
                      rxt2_pkt=0x25000C00, rxt2_bytes=0xA4A3A2A1, rx_stack_trial=2, rx_win=0x32, rx_agc_table=1)
r2 = br.rx_trials(rx)
check(r2[2]["rx"] == 7 and r2[2]["scan_req"] == 7 and r2[2]["rxdhdr"] >> 8 == 12 and r2[2]["buf"] == 0 and r2[2]["chan"] == 0x25 and
      r2[0]["buf"] == 0xFF and r2[0]["rxstat"] == 0x9800, f"rx_trials: {r2}")
out = io.StringIO()
br.show_rx(rx, out)
o = out.getvalue()
check("C2:46:4D:31:00:15  RX 2: the gain table, WINCNTL2 0x32 (JieLi's): 29 events, 29 TXTOG changes, 7 received, 7 SCAN_REQ; "
      "first: RXSTAT 0001, type 3 (RXAHDR 0003), length 12, buffer 0, bytes a1 a2 a3 a4" in o and
      "C2:46:4D:31:00:13  RX 0: no gain table, WINCNTL2 0x32: 29 events, 0 TXTOG changes, 0 received, 0 SCAN_REQ; nothing received: "
      "RXSTAT0 9800" in o and "the stack ran on RX trial 2's window: WINCNTL2 32, gain table written" in o, f"show_rx:\n{o}")
out = io.StringIO()
br.show(old, out)
check("RX TRIALS" not in out.getvalue(), "the 48ea034 report has no RX trials")

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
check("pal_rx_ok 185" in open(os.path.join(d, "counters.txt")).read() and "ram_end 01C16940" in open(os.path.join(d, "counters.txt")).read(),
      "--out: counters.txt")
files = sorted(f for f in os.listdir(d) if f.startswith("step-"))
check(files == ["step-1-radio.bin", "step-3-hci.bin", "step-6-connect.bin", "step-7-midi_in.bin", "step-8-midi_out.bin"],
      f"--out: step files {files}")
check(open(os.path.join(d, "step-3-hci.bin"), "rb").read() == st["hci"]["data"], "--out: the hci step's bytes")
rc, out, err = tool(dev, "--run", "--out", d)
check(rc == 0 and open(wait_out).read() == "20\n", f"--run without --wait: exit {rc} {err}, told {open(wait_out).read()!r}")
rc, out, err = tool(dev, "--read")
check(rc == 0 and "Bluetooth test run 2" in out, f"--read after the runs: exit {rc} {err}")
# --central local: the central (tools/ble_central.py, on the stand-in BlueZ tools) during the run, its result in the output
os.environ.update(FAKE_BLUEZ_STATE=os.path.join(OUT, "fake_bluez_report"), FAKE_BLUEZ_MODE="ok",
                  PATH=os.path.join(ROOT, "tests/ble/fake_bluez") + os.pathsep + os.environ["PATH"])
os.makedirs(os.environ["FAKE_BLUEZ_STATE"], exist_ok=True)
rc, out, err = tool(dev, "--run", "--wait", "3", "--timeout", "20", "--central", "local", "--out", d)
check(rc == 0 and "CENTRAL (tools/ble_central.py)" in out and "C2:46:4D:31:00:01  the stack (Cordio, NimBLE): seen" in out and
      "connect: yes; MIDI port: 128:0" in out, f"--central local: exit {rc} {err}\n{out}")
check(os.path.exists(os.path.join(d, "central.json")) and "central.json" in out, "--central --out: central.json")
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
check(any(t.startswith("hci FAIL") and "made-up failure" in t for t in ftext) and frep["counters"]["bb_err"] == 0x12,
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
