#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mutation check of the stage 3 test (tests/wifi_model.c against tests/wifi/radio_trace.txt): each mutant is a copy
of firmware/wifi/fm1wifi.c (or of its generated tables) with one deliberate bug, built into wifi_model and run. The
test must fail on every one: a swapped write and a dropped delay as a difference from JieLi's trace, a wait that lost
its bound as the register model's "unbounded wait" (exit 3) instead of a hang.

    wifi_mutants.py <out>      <out> is build/host (radio_trace.txt must be there); exit 1 if a mutant survives
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "firmware/wifi"

# (name, file, text, replacement, what the test must do: "diff" = fail its checks, "unbounded" = exit 3)
MUTANTS = (
    ("a swapped write (TX trim: TX back end 3 before 2)", "fm1wifi.c",
     "    win_w(W_TXBE, 2, 0);\n    win_w(W_TXBE, 3, 0);\n",
     "    win_w(W_TXBE, 3, 0);\n    win_w(W_TXBE, 2, 0);\n", "diff"),
    ("a swapped write (table RF_INIT: its first two steps)", "fm1wifi_tables.h",
     "    0x10c00004,  /* WLA_CON12 bit 0 = 1 */\n    0x10c08004,  /* WLA_CON12 bit 1 = 1 */\n",
     "    0x10c08004,  /* WLA_CON12 bit 1 = 1 */\n    0x10c00004,  /* WLA_CON12 bit 0 = 1 */\n", "diff"),
    ("a dropped delay (the ADC peak's 1 ms)", "fm1wifi.c",
     "    win_w(W_RXFE, 23, 0x82);\n    DELAY(1000);\n",
     "    win_w(W_RXFE, 23, 0x82);\n", "diff"),
    ("a dropped delay (table PLL_RESTART's 100 us)", "fm1wifi_tables.h",
     "    0x70000064,  /* delay 100 us */\n};\n\n/* wf_phy_rst",
     "};\n\n/* wf_phy_rst", "diff"),
    ("an unbounded wait (the measurement)", "fm1wifi.c",
     "    for (k = 1; (win_r(W_RXFE, 0x8D) & 3u) && !e3; k++) {\n        if (k >= WAIT_TRIES) { e3 = FM1WIFI_ETIMEOUT; break; }\n",
     "    for (k = 1; (win_r(W_RXFE, 0x8D) & 3u) && !e3; k++) {\n", "unbounded"),
    ("an unbounded wait (the filter tune)", "fm1wifi.c",
     "        for (n = 1; !(status1() & 0x20u) && !e3; n++) {\n            if (n >= WAIT_TRIES) { e3 = FM1WIFI_ETIMEOUT; break; }\n",
     "        for (n = 1; !(status1() & 0x20u) && !e3; n++) {\n", "unbounded"),
    ("an unbounded wait (the RF digital block)", "fm1wifi.c",
     "    for (n = 1; (RD(ANL(0)) & 0x700000u) && !e3; n++) {\n        if (n >= WAIT_TRIES) { e3 = FM1WIFI_ETIMEOUT; break; }\n",
     "    for (n = 1; (RD(ANL(0)) & 0x700000u) && !e3; n++) {\n", "unbounded"),
    ("an unbounded wait (BBP_CSR_CFG's BUSY)", "fm1wifi.c",
     "    for (unsigned i = 0; i < FM1WIFI_BBP_TRIES; i++) {",
     "    for (unsigned i = 0;; i++) {", "unbounded"),
)


def main():
    out = Path(sys.argv[1])
    work = out / "mutants"
    survived = 0
    for n, (name, fname, old, new, want) in enumerate(MUTANTS):
        d = work / str(n)
        if d.exists():
            shutil.rmtree(d)
        shutil.copytree(LIB, d, ignore=shutil.ignore_patterns("bearssl", "*.md"))
        f = d / fname
        text = f.read_text()
        if text.count(old) != 1:
            print("FAIL: mutant %r: its text is not in %s exactly once (the code moved: update the mutant)" % (name, fname))
            survived += 1
            continue
        f.write_text(text.replace(old, new))
        exe = d / "wifi_model"
        subprocess.run(["cc", "-std=c11", "-O1", "-w", "-I" + str(d), "-I" + str(ROOT / "tests/wifi"),
                        "-I" + str(LIB), "-o", str(exe), str(ROOT / "tests/wifi_model.c"), str(ROOT / "tests/wifi/sdio_model.c"),
                        str(d / "fm1wifi.c"), str(d / "fm1wifi_frame.c"), str(ROOT / "tests/wifi/wpa_ap.c"),
                        str(LIB / "wpa_crypto.c"), *map(str, sorted((LIB / "bearssl").glob("*.c")))], check=True)
        try:
            r = subprocess.run([str(exe), str(ROOT / "tests/wifi"), str(out)], capture_output=True, text=True, timeout=120)
            code, err = r.returncode, r.stderr
        except subprocess.TimeoutExpired:
            code, err = None, "hung"
        caught = (code == 3 and "unbounded wait" in err) if want == "unbounded" else (code == 1 and "FAIL" in err)
        print("%s: %s" % ("caught" if caught else "SURVIVED", name))
        if not caught:
            print("  exit %s; %s" % (code, err.strip().splitlines()[-1] if err.strip() else ""))
            survived += 1
    shutil.rmtree(work)
    if survived:
        sys.exit("FAIL: %d mutant(s) not caught" % survived)
    print("wifi mutants: all %d caught" % len(MUTANTS))


if __name__ == "__main__":
    main()
