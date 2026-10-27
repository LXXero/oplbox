#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Read the WiFi test page's results from an FM-1 over USB-MIDI, or start a run first.

  fm1_wifi_report.py [--read] [--out DIR] [--port NAME]         the last run's report
  fm1_wifi_report.py --run [--timeout S] [--out DIR] [--port NAME]  start a run, wait for it, then the report
  fm1_wifi_report.py --ssid NAME [--psk-file F | --psk-from-nm CONNECTION] [--run ...]
                                    first tell the FM-1 the WPA2 network its runs join (stage 8)
  fm1_wifi_report.py --forget [--run ...]   the runs join the open network OPLBOX again

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

--ssid sets the WPA2-PSK network (docs/WIFI_STAGE8.md). The FM-1 keeps it in RAM only,
until it restarts or the network is set again, and never shows or sends back the
passphrase. The passphrase comes from a file (--psk-file, default
~/.config/oplbox/wifi.psk; refused unless only its owner can read it, mode 600; its
first line) or from NetworkManager (--psk-from-nm CONNECTION, which runs
`nmcli -s -g 802-11-wireless-security.psk connection show CONNECTION`). It is never
an argument of this tool or of any program it starts, and never printed. The report's
EAPOL frames (msg 1 and 2) are enough to test passphrase guesses offline: keep --out
directories of a WPA2 run private.

Exit codes: 0 done, 1 the FM-1 refused or has no report (no run since it started),
2 bad arguments, 3 FM-1 not found, 4 no reply (connection lost, or a firmware without
the WiFi report), 5 the run did not finish within --timeout.
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm1_install as fi  # noqa: E402
from fm1_slot import SlotError, message, wait_reply  # noqa: E402

REPORT, RUN, NET = 0x62, 0x63, 0x64
OK, E_REQ, E_NONE, E_RUNNING, E_EDIT = range(5)
STATUS = {E_REQ: "bad request", E_NONE: "no WiFi test run since the FM-1 started (--run starts one)",
          E_RUNNING: "a run is going on", E_EDIT: "EDIT is open on the FM-1: leave it (EDIT) and try again"}
STEPS = {1: "passive", 2: "active", 3: "join", 4: "eapol_from_ap", 5: "eapol_to_ap", 6: "rejoin_from_ap", 7: "rejoin_to_ap"}
MSGS = {1: "msg1", 2: "msg2", 3: "msg3", 4: "msg4", 5: "group1", 6: "group2", 0: "other"}   # an EAPOL frame's "ch"
COUNTERS = ["wl30_04_before", "wl30_04_after", "txrxq_before", "txrxq_after", "frames", "bad", "to_me", "sent",
            "send_errors", "crc_err", "phy_err", "rx_dup", "rx_overflow", "tx_ok", "tx_fail", "tx_retry",
            "tx_underflow", "beacons", "probe_responses",
            # stage 8: the join's WPA2 station (firmware/wifi/fm1wifi_wpa.h) and the key tables as read back
            "pmk_ms", "pmk_cached", "wpa_state", "wpa_why", "rsn", "eapol_reason", "msgs_seen", "keys_loaded",
            "t_4way_ms", "eapol_rx", "eapol_refused", "protected_rx", "protected_ok", "protected_err", "group_rx",
            "group_ok", "group_handshakes", "key_err", "ptk_wcid_lo", "ptk_wcid_hi", "ptk_attr", "ptk_iv", "ptk_eiv",
            "ptk_key_same", "gtk_mode", "gtk_attr", "gtk_iv", "gtk_eiv", "gtk_key_same", "rand_0", "rand_1",
            "tx_ok_before_join", "tx_fail_before_join", "tx_retry_before_join", "crc_err_before_join",
            "tx_sec_before_join", "tx_sec_after", "rx_sec_before_join", "rx_sec_after"]
HEX_COUNTERS = {"wl30_04_before", "wl30_04_after", "txrxq_before", "txrxq_after", "msgs_seen", "ptk_wcid_lo",
                "ptk_wcid_hi", "ptk_attr", "ptk_iv", "ptk_eiv", "ptk_key_same", "gtk_mode", "gtk_attr", "gtk_iv",
                "gtk_eiv", "gtk_key_same", "rand_0", "rand_1", "tx_sec_before_join", "tx_sec_after",
                "rx_sec_before_join", "rx_sec_after"}
PSK_FILE = "~/.config/oplbox/wifi.psk"
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


def passphrase(a):
    """the passphrase for --ssid, from --psk-from-nm or the --psk-file; never printed"""
    if a.psk_from_nm:
        try:
            r = subprocess.run(["nmcli", "-s", "-g", "802-11-wireless-security.psk", "connection", "show", a.psk_from_nm],
                               capture_output=True, text=True, timeout=20)
        except (OSError, subprocess.TimeoutExpired) as e:
            raise ReportError("usage", f"nmcli did not run ({type(e).__name__})")
        pw = r.stdout.split("\n", 1)[0].rstrip("\r") if r.returncode == 0 else ""
        if not pw:
            why = (r.stderr.strip().splitlines() or [f"exit {r.returncode}"])[0]
            raise ReportError("usage", f"NetworkManager gave no passphrase for connection {a.psk_from_nm!r} ({why}; "
                                       "nmcli -s needs the right to read secrets)")
        return pw
    path = os.path.expanduser(a.psk_file or PSK_FILE)
    try:
        st = os.stat(path)
    except OSError:
        raise ReportError("usage", f"no passphrase file {path} (write the passphrase on its first line, chmod 600; "
                                   "or --psk-from-nm CONNECTION)")
    if st.st_mode & 0o077:
        raise ReportError("usage", f"{path} can be read by others (mode {st.st_mode & 0o777:o}): chmod 600 it")
    with open(path, encoding="ascii", errors="replace") as f:
        return f.readline().rstrip("\r\n")


def set_network(link, ssid, pw):
    """the network command: ssid (bytes, b"" forgets the network) and the passphrase"""
    if ssid and not 1 <= len(ssid) <= 32:
        raise ReportError("usage", f"the SSID is {len(ssid)} bytes: WPA2 takes 1-32")
    if ssid and not (8 <= len(pw) <= 63 and all(32 <= ord(c) <= 126 for c in pw)):
        raise ReportError("usage", "the passphrase must be 8-63 printable ASCII characters (WPA2-PSK; a 64-hex-digit "
                                   "key is not taken)")
    body = bytes([len(ssid)]) + ssid + bytes([len(pw)]) + pw.encode("ascii") if ssid else b"\0"
    b = ask(link, NET, body)
    if len(b) < 2:
        raise ReportError("noreply", f"a {len(b)}-byte reply to the network command (a firmware before stage 8?)")
    if b[0] != OK:
        raise ReportError("refused", "the FM-1 refused the network: " + STATUS.get(b[0], f"status {b[0]}"))


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


def frame_name(i, fr):
    if fr["step"].startswith(("eapol", "rejoin")):
        return f"frame-{i:02d}-{fr['step']}-{MSGS.get(fr['ch'], 'msg%d' % fr['ch'])}.bin"
    return f"frame-{i:02d}-{fr['step']}-ch{fr['ch']:02d}.bin"


def write_out(d, rep, raw):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "report.bin"), "wb") as f:
        f.write(raw)
    with open(os.path.join(d, "report.txt"), "w") as f:
        f.write(rep["text"])
    with open(os.path.join(d, "counters.txt"), "w") as f:
        f.write(counters_text(rep["counters"]))
    for i, fr in enumerate(rep["frames"], 1):
        with open(os.path.join(d, frame_name(i, fr)), "wb") as f:
            f.write(fr["data"])


def show(rep, out):
    print(f"WiFi test run {rep['run']}", file=out)
    print(rep["text"], end="" if rep["text"].endswith("\n") else "\n", file=out)
    print("\nCOUNTERS", file=out)
    print(counters_text(rep["counters"]), end="", file=out)
    print(f"\nFRAMES {len(rep['frames'])}", file=out)
    for i, fr in enumerate(rep["frames"], 1):
        cut = "" if len(fr["data"]) >= fr["had"] else f", first {len(fr['data'])} kept"
        where = MSGS.get(fr["ch"], "?") if fr["step"].startswith(("eapol", "rejoin")) else f"ch {fr['ch']:2d}"
        print(f"{i:2d} {fr['step']:7s} {where}  {fr['had']} bytes{cut}  {fr['data'][:36].hex()}", file=out)
    if rep["cut_text"] or rep["cut_frames"]:
        print("(the FM-1 cut the report short: it did not fit its buffer)", file=out)


EXIT = {"refused": 1, "usage": 2, "notfound": 3, "noreply": 4, "timeout": 5}


def run(a, backend, out):
    up = fi.Updater(backend, a.port)
    dev = up.find()
    if not dev:
        raise ReportError("notfound", str(fi.not_found(up)))
    try:
        if a.ssid is not None or a.forget:
            ssid = a.ssid.encode("utf-8") if a.ssid is not None else b""
            set_network(dev.link, ssid, passphrase(a) if ssid else "")
            print(f"network {a.ssid} set" if ssid else "network forgotten: runs join the open OPLBOX", file=out)
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
        if any(fr["step"].startswith(("eapol", "rejoin")) for fr in rep["frames"]):
            print("   (it holds EAPOL frames: enough to test passphrase guesses offline, so keep it private)", file=out)
    return 0


def main(argv=None, backend=None, out=sys.stdout):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--read", action="store_true", help="read the last run's report (the default)")
    g.add_argument("--run", action="store_true", help="start a run, wait for it, then read its report")
    ap.add_argument("--timeout", type=float, default=60.0, metavar="S", help="--run: seconds to wait (default 60)")
    ap.add_argument("--out", metavar="DIR", help="write the text, the counters and each frame's bytes into DIR")
    ap.add_argument("--port", metavar="NAME", help="MIDI port to use (part of its name)")
    n = ap.add_mutually_exclusive_group()
    n.add_argument("--ssid", metavar="NAME", help="first set the WPA2 network the runs join (the FM-1 keeps it in RAM)")
    n.add_argument("--forget", action="store_true", help="first forget the network: runs join the open OPLBOX")
    k = ap.add_mutually_exclusive_group()
    k.add_argument("--psk-file", metavar="FILE", help=f"--ssid: the passphrase, first line of FILE, mode 600 (default {PSK_FILE})")
    k.add_argument("--psk-from-nm", metavar="CONNECTION", help="--ssid: the passphrase from NetworkManager's connection")
    a = ap.parse_args(argv)
    if (a.psk_file or a.psk_from_nm) and a.ssid is None:
        ap.error("--psk-file and --psk-from-nm go with --ssid")
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
