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


def cut(r, n):
    """the report r with only its first n counters, as a firmware that sent n"""
    at = 10 + int.from_bytes(r[8:10], "little")
    k = r[at]
    return r[:at] + bytes([n]) + r[at + 1:at + 1 + 4 * n] + r[at + 1 + 4 * k:]


# f0e6d08's report: 192 counters, the window probe's where the TX side's are now; told apart by its probe_delays word.
# a349877's: the same 192 with the TX side's, no header trace
r192 = cut(raw, 192)
k0 = 10 + int.from_bytes(raw[8:10], "little") + 1
pd = k0 + 4 * br.COUNTERS_F0E6D08.index("probe_delays")
old192 = br.decode(r192[:pd] + br.PROBE_DELAYS_F0E6D08.to_bytes(4, "little") + r192[pd + 4:])
check("probe_n" in old192["counters"] and "pal_stale_rx" not in old192["counters"] and
      old192["counters"]["probe_delays"] == br.PROBE_DELAYS_F0E6D08 and "pal_stale_rx" in rep["counters"],
      "f0e6d08's 192 counters read with its names (the window probe's), this firmware's with the TX side's")
a349 = br.decode(r192)
out = io.StringIO()
br.show(a349, out)
check(list(a349["counters"]) == br.COUNTERS[:192] and "LINK LAYER" not in out.getvalue() and
      "TX modes, one per connection in the order arq, air, intime:" in out.getvalue() and len(br.COUNTERS) == 248,
      f"a349877's 192 counters: the TX side's names, three TX modes, no link layer:\n{out.getvalue()[-3000:]}")

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
check("WINDOW PROBE" not in out and "TX modes, one per connection in the order arq, air, intime, jieli:" in out and
      "    arq: the chip's SN and NESN (MD in TXDHDR bit 3), the slot TXTOG names only, Cordio's SN and NESN the PAL's: 12 "
      "events, 10 received, TXTOG changed in 9, 0 packets with RXSTAT bit 8" in out and
      "    jieli: the IR's slot words (bit 2 the slot's, SN in bit 3), nothing written at the arm: 40 events, 38 received, "
      "TXTOG changed in 37, 2 packets with RXSTAT bit 8" in out and
      "under 50 us 30, 50-99 5, 100-149 2, 150 and more 1 (least 21 us); intime left out 0 (over 100 us)" in out and
      "stale packet flags dropped 1 (starting 400 us or more before the window); events halted at their deadline 1" in out and
      "at the first connection packet (before Cordio's response): TXDHDR0 01, TXDHDR1 05, TXTOG 1, RXTOG 0" in out and
      "at the end: TXTOG 0001, RXTOG 0001, TXDHDR0 0905, TXDHDR1 0001, TXBUF0CNTL 01, TXBUF1CNTL 00, IFSCNT 8295" in out,
      f"--run: the TX side (no window probe since f0e6d08):\n{out}")
check("connections: 2 started on the PAL, NimBLE 2 connects and 1 disconnects; kept setup: none, kept TX mode: none" in out and
      "connection 1: padw intime, 6 events, 0 received; missed events ended 262..846 us after dueUsec; TXTOG changed in 0, "
      "stale 1, halted 1" in out and
      "connection 2: padb jieli, 40 events, 38 received; packets 31 (first) and 29 (last) us after dueUsec; TXTOG changed in "
      "37, stale 0, halted 0" in out and
      "(each connection runs one TX mode, the next connection the next one; window setups used: padb, padw)" in out and
      "each connection runs one window setup" not in out,
      f"--run: each connection's summary:\n{out}")
check("LINK LAYER (the PAL's header trace" in out and
      "  HCI ACL: host to controller 3, controller to host 2; Number Of Completed Packets 2; NimBLE's ATT MTU 185, the "
      "central's writes 1" in out and
      "  ARQ: packets Cordio was told were repeats 1, responses that did not go in 1; at the end RXTOG 0001, TXTOG 0000" in out and
      "  packets after the first in a connection event 4, given to Cordio 3 (both 0 in a report from before round 6: not "
      "counted)" in out and
      "  connection 1 (intime): the central's control PDUs FEATURE_REQ, VERSION_IND; its L2CAP PDUs Cordio took 0; Cordio's "
      "non-empty responses put in 1; told not acknowledged 0; RXTOG changed 2" in out and
      "  connection 2 (jieli): the central's control PDUs none; its L2CAP PDUs Cordio took 1; Cordio's non-empty responses "
      "put in 0; told not acknowledged 3; RXTOG changed 38" in out and
      "     0  03 LL_FEATURE_REQ         RT  ack new  09 empty" in out and
      "     1  03 LL_VERSION_IND         RT  ack new  07 LL_FEATURE_RSP" in out and
      "     2  01 empty                  crc --           05 empty (not put in)" in out and
      "     0  02 L2CAP                  R-  nak new  01 empty" in out,
      f"--run: the link layer:\n{out[out.find('LINK LAYER'):]}")
check("PDU CAPTURE (what went each way" in out and
      "  ARQ connections 2: ahead 1, the others in T_IFS; kept: none (after 3 L2CAP PDUs from the central); responses "
      "written ahead 40" in out and
      "  the chip's SN: bit 2 changed at a connection's first packet 1, after 3 packets the central took none of 0" in out and
      "     0    1     4  central  02    7  R-    ATT Exchange MTU Req: MTU 517" in out and
      "     1    1     5  Cordio+  0A    7  18us  ATT Exchange MTU Rsp: MTU 23" in out and
      "     2    1     7  central  02   11  R-    ATT Read By Group Type Req: 0001-FFFF, type 2800" in out and
      "     3    1     8  Cordio+  0E    9  24us  ATT Error Rsp: ATT Read By Group Type Req, handle 0001, Attribute Not Found"
      in out and "05 00 04 00 01 10 01 00 0a" in out,
      f"--run: the PDU capture:\n{out[out.find('PDU CAPTURE'):]}")
check(rep["capture"] is not None and len(rep["capture"]) == 16 + 4 * 20 and
      [s["step"] for s in rep["steps"]] == br.STEPS, "the capture frame decoded apart from the steps")
check(open(wait_out).read() == "3\n", "--wait 3: the FM-1 was not told 3 s")
check(open(os.path.join(d, "report.txt")).read() == rep["text"], "--out: report.txt is not the report's text")
check(open(os.path.join(d, "report.bin"), "rb").read()[10:] == raw[10:], "--out: report.bin is not the report")
check("pal_rx_ok 185" in open(os.path.join(d, "counters.txt")).read() and "ram_end 01C16940" in open(os.path.join(d, "counters.txt")).read(),
      "--out: counters.txt")
files = sorted(f for f in os.listdir(d) if f.startswith("step-"))
check(files == ["step-1-radio.bin", "step-3-hci.bin", "step-6-connect.bin", "step-7-midi_in.bin", "step-8-midi_out.bin"],
      f"--out: step files {files}")
check(open(os.path.join(d, "step-3-hci.bin"), "rb").read() == st["hci"]["data"], "--out: the hci step's bytes")
check(open(os.path.join(d, "capture.bin"), "rb").read() == rep["capture"], "--out: capture.bin is not the capture")
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
