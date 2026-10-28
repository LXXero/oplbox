#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mutation check of the PAL test (tests/ble_pal.c: Cordio on firmware/ble/cordio_port/pal_fm1.c and the radio driver's
one-packet operations against the register model's air): each mutant is a copy of pal_fm1.c or fm1ble.c with one
deliberate bug, built into ble_pal and run. The test must fail on every one (exit 1 with a FAIL line) and must not hang.

    ble_pal_mutants.py <out>      <out> is build/host (its cordio/ objects are reused); exit 1 if a mutant survives
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PAL = "firmware/ble/cordio_port/pal_fm1.c"
DRV = "firmware/ble/fm1ble.c"

# (name, file, text, replacement)
MUTANTS = (
    ("a packet's timestamp is when it was seen, not when it started", PAL,
     "g_held_ts = t - air_us(g_held_n);", "g_held_ts = t;"),
    ("the response always counted late", PAL,
     "    if (g_done) {                       /* the event is over: too late for T_IFS */",
     "    if (1) {                            /* the event is over: too late for T_IFS */"),
    ("the advertising packet's completion waits for the event's end", PAL,
     "if (g_wait == W_TX && (g_held || g_done)) {", "if (g_wait == W_TX && g_done) {"),
    ("a received packet reported as a CRC failure", PAL,
     "rx(g_held_ok ? BB_STATUS_SUCCESS : BB_STATUS_CRC_FAILED", "rx(g_held_ok ? BB_STATUS_CRC_FAILED : BB_STATUS_SUCCESS"),
    ("a connection event armed at once, not at its due time", PAL,
     "fm1ble_op_conn(g_cfg.b, delay_to_due(), g_rx_timeout)", "fm1ble_op_conn(g_cfg.b, 0, g_rx_timeout)"),
    ("the receive window Cordio asks for not used", PAL,
     "fm1ble_op_conn(g_cfg.b, delay_to_due(), g_rx_timeout)", "fm1ble_op_conn(g_cfg.b, delay_to_due(), 20)"),
    ("each of Cordio's advertising packets its own hardware event (three events where Cordio has one)", PAL,
     "    if (g_chan >= 37 && g_chan <= 39 && adv_covered())\n        return;\n", ""),
    ("a CONNECT_IND does not end the advertising event", PAL,
     "g_armed && (p.hdr[0] & 0xFu) == 5u)", "g_armed && (p.hdr[0] & 0xFu) == 0x10u)"),
    ("no deadline for an operation the baseband never ends", PAL,
     "if (g_armed && !g_done && (int32_t)(t - g_deadline) > 0) {", "if (0) {"),
    ("the scheduler's timer fires a millisecond late", PAL,
     "if (g_tmr_on && (int32_t)(t - g_tmr_due) >= 0) {", "if (g_tmr_on && (int32_t)(t - g_tmr_due) >= 1000) {"),
    ("the end of the event never stops the operation", PAL,
     "                g_done = 1;\n                g_armed = 0;\n                fm1ble_op_stop(g_cfg.b);\n",
     "                g_armed = 0;\n                fm1ble_op_stop(g_cfg.b);\n"),
    ("a data channel Cordio asks for not passed on", PAL,
     "pChan->crcInit, pChan->chanIdx) != FM1BLE_OK", "pChan->crcInit, (uint8_t)(pChan->chanIdx >= 37 ? pChan->chanIdx : 3)) != FM1BLE_OK"),
    ("a data PDU sent without its SN and NESN", DRV,
     "len << 8 | (t[0x10] & 0x1Fu)", "len << 8 | (t[0x10] & 0x03u)"),
    ("the bit offset inside the slot dropped", DRV,
     "    ax_wr(FM1BLE_AX_BITOFF, bitoff);\n", "    ax_wr(FM1BLE_AX_BITOFF, 0);\n"),
    ("a connection event on a fixed wrong channel", DRV,
     "ax_wr(FM1BLE_AX_CHANNEL, b->op_chan | 0x2000u);", "ax_wr(FM1BLE_AX_CHANNEL, 7u | 0x2000u);"),
    ("the CRC init's high byte lost", DRV,
     "e16(b, FM1BLE_E_CRCWORD1, (crc >> 16) & 0xFFu);", "e16(b, FM1BLE_E_CRCWORD1, 0);"),
    ("the scan response put in the advertising PDU's slot", DRV,
     "op_buf(b, 1, pdu, n, 1, b->op_slot ^ 1u);", "op_buf(b, 1, pdu, n, 1, b->op_slot);"),
    ("an advertising PDU's length left out of TXDHDR (the first FM-1 run's bug)", DRV,
     "e16(b, dh, len << 8 | (g16(b, dh) & 4u)", "e16(b, dh, (g16(b, dh) & 4u)"),
    ("the advertising PDU always in slot 0, whatever TXTOG says", DRV,
     "b->op_slot = (uint8_t)tx_next(b);", "b->op_slot = 0; (void)tx_next(b);"),
    ("advertising events with hop 0 (122afe3's one-channel shape: never heard, never ended on 38 and 39)", DRV,
     "    b->hop = 1;\n", "    b->hop = 0;\n"),
    ("the kept scan response not in the other TX slot of later events", DRV,
     "tx_slot(b, b->op_slot ^ 1u, type == 2u || !b->op_rsp ? 0 : 1);", "tx_slot(b, b->op_slot ^ 1u, 0);"),
    ("a data PDU only in TX slot 0, whatever TXTOG says", DRV,
     "for (s = 0; s < 2u; s++) {", "for (s = 0; s < 1u; s++) {"),
    ("the received data header's SN and NESN dropped", DRV,
     "ahdr & 0xFFu : dhdr & 0x1Fu", "ahdr & 0xFFu : dhdr & 0x03u"),
    ("the advertising events' receive window (WINCNTL2) left at 0, as up to 3b39ab4", DRV,
     "    e16(b, FM1BLE_E_WINCNTL2, adv_win(b));\n    e16(b, FM1BLE_E_FILTERCNTL, 0);\n", "    e16(b, FM1BLE_E_FILTERCNTL, 0);\n"),
    ("the AGC gain table not written at power-up, as up to 3b39ab4", DRV,
     "    b->d.agc_table = (uint8_t)rx_agc(b);\n", "    b->d.agc_table = 0;\n"),
    ("the gain table written one word out of step", DRV,
     "wr(FM1BLE_AGC_TAB, FM1BLE_AGC_TABLE[i]);", "wr(FM1BLE_AGC_TAB, FM1BLE_AGC_TABLE[(i + 1u) % FM1BLE_AGC_WORDS]);"),
    ("a connection event polled like the rest, not waited through (58cadc5: 1 packet in 18 events on the FM-1)", PAL,
     "if (!progress && !g_cfg.no_busy_wait && g_kind == K_CONN", "if (0 && !progress && !g_cfg.no_busy_wait && g_kind == K_CONN"),
    ("a missed connection event keeps its window setup", PAL,
     "    if (s != g_set) {\n        g_set = s;", "    if (0) {\n        g_set = s;"),
    ("an event with its packet moves to the next window setup too", PAL,
     "    if (g_conn_rx)\n        return;\n    for (i = 1;", "    for (i = 1;"),
    ("the window setup not handed to the driver", PAL,
     "        fm1ble_op_conn_setup(g_cfg.b, g_set);\n", ""),
    ("the padded window not longer", DRV,
     "        window_us += 2u * FM1BLE_CONN_PAD_US;\n", ""),
    ("a connection event's window not the one Cordio asks for (WINCNTL2 = WINCNTL0's 0x32)", DRV,
     "e16(b, FM1BLE_E_WINCNTL2, window_us > 0xFFFFu ? 0xFFFFu : window_us);", "e16(b, FM1BLE_E_WINCNTL2, FM1BLE_WIN0);"),
)


def sh(cmd):
    return subprocess.run(["bash", "-c", cmd], capture_output=True, text=True, cwd=ROOT)


def main():
    out = Path(sys.argv[1]).resolve()
    work = out / "ble_pal_mutants"
    cr_inc = sh("find firmware/ble/cordio -name '*.h' -printf '-I%h\\n' | sort -u | tr '\\n' ' '").stdout.split()
    cr_def = ["-D" + l.strip() for l in (ROOT / "firmware/ble/cordio_port/DEFINES.txt").read_text().splitlines() if l.strip()]
    objs = [str(p) for p in sorted((out / "cordio").glob("*.o")) if "pal_stubs_host" not in p.name]
    survived = 0
    for n, (name, path, old, new) in enumerate(MUTANTS):
        d = work / str(n)
        if d.exists():
            shutil.rmtree(d)
        d.mkdir(parents=True)
        files = {PAL: d / "pal_fm1.c", DRV: d / "fm1ble.c"}
        for src, dst in files.items():
            shutil.copy(ROOT / src, dst)
        f = files[path]
        text = f.read_text()
        if text.count(old) != 1:
            print("FAIL: mutant %r: its text is not in %s exactly once (the code moved: update the mutant)" % (name, path))
            survived += 1
            continue
        f.write_text(text.replace(old, new))
        exe = d / "ble_pal"
        inc = ["-I" + str(ROOT / "firmware/ble"), "-I" + str(ROOT / "firmware/ble/cordio_port"), "-I" + str(ROOT / "tests/ble")]
        subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-fshort-enums", *cr_def, *["-I" + str(ROOT / i[2:]) for i in cr_inc], *inc, "-c", "-o", str(d / "pal_fm1.o"), str(files[PAL])], check=True)
        subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-fshort-enums", *inc, "-Wl,--gc-sections", "-o", str(exe),
                        str(ROOT / "tests/ble_pal.c"), str(ROOT / "tests/ble/bb_model.c"), str(ROOT / "tests/ble/air_central.c"), str(files[DRV]),
                        str(ROOT / "firmware/ble/ble_midi.c"), str(d / "pal_fm1.o"), *objs], check=True)
        try:
            r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=120)
            code, err = r.returncode, r.stderr
        except subprocess.TimeoutExpired:
            code, err = None, "hung"
        caught = code == 1 and "FAIL" in err
        print("%s: %s" % ("caught" if caught else "SURVIVED", name))
        if not caught:
            print("  exit %s; %s" % (code, err.strip().splitlines()[-1] if err.strip() else ""))
            survived += 1
    shutil.rmtree(work)
    if survived:
        sys.exit("FAIL: %d mutant(s) not caught" % survived)
    print("ble_pal mutants: all %d caught" % len(MUTANTS))


if __name__ == "__main__":
    main()
