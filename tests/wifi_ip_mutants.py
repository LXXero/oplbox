#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mutation check of the stage 9 test (tests/wifi_ip.c): each mutant is a copy of firmware/wifi/fm1wifi_ip.c with one
deliberate bug, built into wifi_ip and run. The test must fail on every one.

    wifi_ip_mutants.py <out>      <out> is build/host (lwip/*.o and fm1wifi_net.o must be there); exit 1 if one survives
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "firmware/wifi"

# (name, text in fm1wifi_ip.c, replacement)
MUTANTS = (
    ("frames sent without the Protected bit",
     "    if (prot) tx_buf[1] |= FM1WIFI_FC_PROTECTED >> 8;\n", ""),
    ("frames sent without FM1WIFI_TX_CRYPT",
     "FM1WIFI_TX_ACK | (prot ? FM1WIFI_TX_CRYPT | FM1WIFI_TX_WCID(FM1WIFI_WCID_AP) : 0u)", "FM1WIFI_TX_ACK"),
    ("frames sent on WCID 2",
     "FM1WIFI_TX_CRYPT | FM1WIFI_TX_WCID(FM1WIFI_WCID_AP)", "FM1WIFI_TX_CRYPT | FM1WIFI_TX_WCID(2)"),
    ("frames in the clear taken after the 4-way handshake",
     "    if (!(f->fc & FM1WIFI_FC_PROTECTED) && (w->keys & WPA_EAPOL_NEW_PTK)) { ip->rx_clear++; return; }\n", ""),
    ("frames with a cipher error handed to lwIP",
     "        if (ce || !(f->info & FM1WIFI_RXD_DECRYPTED)) {", "        if (0) {"),
    ("the Protected bit left set for lwIP (it drops such frames)",
     "    rx_buf[1] &= (uint8_t)~(FM1WIFI_FC_PROTECTED >> 8);\n", ""),
    ("a CCMP header left in not taken off",
     "            body += 8; blen -= 8u;", "            blen -= 0u;"),
    ("a DHCP reply to another client kept",
     "if (m.op == 2u && same(m.chaddr, ip->w->sta.sa, 6))", "if (m.op == 2u)"),
    ("the ping wait not applied (late replies counted as answers)",
     "                if (rtt > ip->ping_wait) ip->ping_late++;", "                if (0) ip->ping_late++;"),
    ("an echo reply's sequence number not checked",
     "e.seq < ip->ping_n && ip->ping_rtt[e.seq] == FM1WIFI_IP_NO_REPLY", "e.seq < FM1WIFI_IP_PING_N"),
    ("the gateway's ARP request sent once only",
     "        if (r != FM1WIFI_ETIMEOUT) break;\n        left -= slot;", "        break;"),
    ("no DHCPRELEASE on stop",
     "    if (ip->w && ip->w->state == FM1WIFI_WPA_UP) fm1wifi_net_dhcp_stop();", ""),
    ("the station's sequence numbers not used",
     "    uint16_t seq = w->sta.seq++;", "    uint16_t seq = 0;"),
    ("a step that does not stop when the station left",
     "        if (ip->w->state != FM1WIFI_WPA_UP) return FM1WIFI_ESTATE;\n", ""),
    ("listening that counts replies outside the window",
     "{ ip->echo_out++; if (ip->listening) ip->listen_out++; }", "{ ip->echo_out++; ip->listen_out++; }"),
)


def main():
    out = Path(sys.argv[1])
    work = out / "ip_mutants"
    survived = 0
    lwip = sorted(str(p) for p in (out / "lwip").glob("*.o"))
    for n, (name, old, new) in enumerate(MUTANTS):
        d = work / str(n)
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
        text = (LIB / "fm1wifi_ip.c").read_text()
        if text.count(old) != 1:
            print("FAIL: mutant %r: its text is not in fm1wifi_ip.c exactly once (the code moved: update the mutant)" % name)
            survived += 1
            continue
        (d / "fm1wifi_ip.c").write_text(text.replace(old, new))
        exe = d / "wifi_ip"
        subprocess.run(["cc", "-std=c11", "-O1", "-w", "-DFM1WIFI_NET_DEBUG", "-I" + str(LIB), "-I" + str(ROOT / "tests/wifi"),
                        "-o", str(exe), str(ROOT / "tests/wifi_ip.c"), str(ROOT / "tests/wifi/sdio_model.c"),
                        str(LIB / "fm1wifi.c"), str(LIB / "fm1wifi_frame.c"), str(LIB / "fm1wifi_wpa.c"), str(LIB / "wpa_eapol.c"),
                        str(d / "fm1wifi_ip.c"), str(out / "fm1wifi_net.o"), str(ROOT / "tests/wifi/wpa_ap.c"),
                        str(LIB / "wpa_crypto.c"), *map(str, sorted((LIB / "bearssl").glob("*.c"))), *lwip], check=True)
        try:
            r = subprocess.run([str(exe), str(ROOT / "tests/wifi")], capture_output=True, text=True, timeout=120)
            code, err = r.returncode, r.stderr
        except subprocess.TimeoutExpired:
            code, err = None, "hung"
        caught = code not in (0, None) or code is None
        print("%s: %s" % ("caught" if caught else "SURVIVED", name))
        if not caught:
            survived += 1
    shutil.rmtree(work)
    if survived:
        sys.exit("FAIL: %d mutant(s) not caught" % survived)
    print("wifi_ip mutants: all %d caught" % len(MUTANTS))


if __name__ == "__main__":
    main()
