#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mutation check of the stage 2 test (tests/ble_midi.c, docs/BLUETOOTH.md): each mutant is a copy of
firmware/ble/ble_midi.c with one deliberate bug, built into ble_midi and run. The test must fail on every one
(exit 1 with a FAIL line), and must not hang or crash.

    ble_midi_mutants.py <out>      <out> is build/host; exit 1 if a mutant survives
"""
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import limits

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "firmware/ble"

# (name, text, replacement): the text is in ble_midi.c exactly once
MUTANTS = (
    ("the service UUID with one byte wrong",
     "0x00, 0xC7, 0xC4, 0x4E,", "0x00, 0xC7, 0xC4, 0x4F,"),
    ("the characteristic UUID with one byte wrong",
     "0xF3, 0x6B, 0x10, 0x9D,", "0xF3, 0x6B, 0x10, 0x9C,"),
    ("the first read answers with a payload",
     "    return 0;                   /* section 5", "    return 1;                   /* section 5"),
    ("the advertising flags without BR/EDR not supported",
     "    out[2] = 0x06;", "    out[2] = 0x02;"),
    ("a long name sent as the complete name",
     "out[1] = name[n] ? AD_NAME_SHORT : AD_NAME_COMPLETE;", "out[1] = AD_NAME_COMPLETE;"),
    ("pitch bend counted as 2 bytes",
     "    if (st < 0xE0)\n        return 2;               /* program change, channel pressure */\n    if (st < 0xF0)\n        return 3;",
     "    if (st < 0xE0)\n        return 2;               /* program change, channel pressure */\n    if (st < 0xF0)\n        return 2;"),
    ("control change counted as 2 bytes",
     "    if (st < 0xC0)\n        return 3;", "    if (st < 0xC0)\n        return 2;"),
    ("the header without bit 7",
     "e->buf[0] = (uint8_t)(0x80u | e->high);", "e->buf[0] = (uint8_t)(e->high);"),
    ("a timestamp byte without bit 7",
     "e->buf[e->len++] = (uint8_t)(0x80u | low);", "e->buf[e->len++] = (uint8_t)(low);"),
    ("timestampHigh 7 bits",
     "#define HIGH_MASK   0x3Fu", "#define HIGH_MASK   0x7Fu"),
    ("every time taken as one the receiver can rebuild",
     "    return high == ((ts >> 7) & HIGH_MASK);", "    return 1;"),
    ("the encoder does not count a wrap of timestampLow",
     "    if (e->have_prev && low < e->prev_low)\n        e->high", "    if (0)\n        e->high"),
    ("running status never used",
     "*running = st < 0xF0 && st == e->rs;", "*running = 0;"),
    ("running status for any status once one was sent",
     "*running = st < 0xF0 && st == e->rs;", "*running = st < 0xF0 && e->rs;"),
    ("a SysEx does not cancel running status",
     "    e->rs = 0;                  /* SysEx cancels running status */\n", ""),
    ("no timestamp byte after a real-time message at the same time",
     "*with_ts = !*running || e->force_ts ||", "*with_ts = !*running ||"),
    ("running status with a later time and no timestamp byte",
     "(ts & 0x7Fu) != e->prev_low;", "0;"),
    ("data bytes not masked to 7 bits",
     "e->buf[e->len++] = d1 & 0x7Fu;", "e->buf[e->len++] = d1;"),
    ("a packet one byte over the size",
     "e->len + need > e->cap)", "e->len + need > e->cap + 1)"),
    ("a SysEx packet that leaves a byte unused",
     "        take = rem < space ? rem : space;", "        take = rem < space ? rem : space - 1;"),
    ("0xF7 with no timestamp byte",
     "            put_ts(e, ts);\n            e->buf[e->len++] = 0xF7;", "            e->buf[e->len++] = 0xF7;"),
    ("a status byte accepted inside SysEx",
     "        if (data[i] & 0x80u)\n            return BLE_MIDI_EBAD;", "        if (0)\n            return BLE_MIDI_EBAD;"),
    ("the decoder does not count a wrap of timestampLow",
     "                if (have_prev && low < prev_low)\n                    high", "                if (0)\n                    high"),
    ("running status after a real-time byte with no timestamp byte",
     "(state == ST_AFTER_TS || rs_no_ts)", "1"),
    ("real time inside SysEx ends the SysEx",
     "                if (b >= 0xF8) {        /* real time inside SysEx", "                if (0) {        /* real time inside SysEx"),
    ("a SysEx continuation not recognised",
     "        if (d->in_sysex) {              /* a continuation", "        if (0) {              /* a continuation"),
    ("a message cut off by the end of the packet not counted",
     "    if (state == ST_DATA)\n        d->bad++;                       /* the packet ended inside a message */\n", ""),
    ("the end of a SysEx does not close it",
     "                    d->in_sysex = 0;\n                    rs_no_ts = 0;\n                    continue;", "                    rs_no_ts = 0;\n                    continue;"),
    ("the queue never says it is full",
     "    if (in->w - in->r >= BLE_MIDI_RING) {", "    if (0) {"),
    ("a short message's missing data byte left as it was",
     "    slot[1] = n > 1 ? m[1] : 0;", "    slot[1] = m[1];"),
    ("a SysEx with no end given to the sink",
     "        if (in->sx_over || !in->sysex)", "        if (!in->sysex)"),
    ("a reset leaves a half-received SysEx",
     "    in->sx_len = 0;\n    in->sx_over = 0;\n    ble_midi_dec_reset(&in->dec);", "    ble_midi_dec_reset(&in->dec);"),
)


def main():
    out = Path(sys.argv[1])
    work = out / "ble_midi_mutants"
    survived = 0
    for n, (name, old, new) in enumerate(MUTANTS):
        d = work / str(n)
        if d.exists():
            shutil.rmtree(d)
        shutil.copytree(LIB, d)
        f = d / "ble_midi.c"
        text = f.read_text()
        if text.count(old) != 1:
            print("FAIL: mutant %r: its text is not in ble_midi.c exactly once (the code moved: update the mutant)" % name)
            survived += 1
            continue
        f.write_text(text.replace(old, new))
        exe = d / "ble_midi"
        subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-w", "-I" + str(d), "-o", str(exe),
                        str(ROOT / "tests/ble_midi.c"), str(f)], check=True)
        try:
            r = limits.run([str(exe)], capture_output=True, text=True, timeout=60)
            code, err = r.returncode, r.stderr
            if code < 0:                                          # killed by a signal, e.g. the memory limit
                code, err = None, "killed (signal %d)" % -code
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
    print("ble midi mutants: all %d caught" % len(MUTANTS))


if __name__ == "__main__":
    main()
