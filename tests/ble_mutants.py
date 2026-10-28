#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mutation check of the stage 1 test (tests/ble_hci.c against the fake controller, docs/BLUETOOTH.md): each mutant is
a copy of firmware/ble/hci.c with one deliberate bug, built into ble_hci and run. The test must fail on every one
(exit 1 with a FAIL line), and must not hang.

    ble_mutants.py <out>      <out> is build/host; exit 1 if a mutant survives
"""
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import limits
import mutation

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "firmware/ble"

# (name, text, replacement)
MUTANTS = (
    ("the LE event mask left at Connection Complete only",
     "#define HCI_LE_EVENT_MASK             0x0000000000000005ull",
     "#define HCI_LE_EVENT_MASK             0x0000000000000001ull"),
    ("the event mask without LE Meta",
     "#define HCI_EVENT_MASK                0x2000000000008010ull",
     "#define HCI_EVENT_MASK                0x0000000000008010ull"),
    ("a command sent without a credit",
     "        if (h->ncmd_bytes && h->ncmd) {",
     "        if (h->ncmd_bytes) {"),
    ("ACL sent beyond the controller's free buffers",
     "    while (h->conn != HCI_NO_CONN && h->tx_off < h->ntx && h->acl_free) {",
     "    while (h->conn != HCI_NO_CONN && h->tx_off < h->ntx) {"),
    ("a fragment longer than the controller's ACL size",
     "        if (k > h->acl_len)\n            k = h->acl_len;\n",
     ""),
    ("every fragment sent as a first fragment",
     "off ? HCI_PB_CONT : HCI_PB_FIRST_NOFLUSH",
     "HCI_PB_FIRST_NOFLUSH"),
    ("advertising not started again after a disconnection",
     "        if (h->state == HCI_ST_CONNECTED) {          /* a peripheral advertises again */",
     "        if (0) {"),
    ("buffers not given back on a disconnection",
     "        h->acl_free = h->acl_total;                  /* the controller drops what it held for the link (4.3) */\n",
     ""),
    ("a refused command does not fail the start",
     "    if (e->status) {\n        fail(h, HCI_FAIL_STATUS, e->opcode, e->status);\n        return;\n    }\n",
     ""),
    ("no command timeout",
     "            if (h->wait_ms >= HCI_CMD_TIMEOUT_MS)",
     "            if (0)"),
    ("a refused send does not count towards the command timeout",
     "                h->wait_ms = waited;\n",
     "                h->wait_ms = 0;\n                sent = 1;\n"),
    ("the shared buffer size never asked",
     "h->acl_len && h->acl_total ? HCI_STEP_BD_ADDR : HCI_STEP_BUFFER_SIZE",
     "HCI_STEP_BD_ADDR"),
    ("an incoming PDU delivered before it is whole",
     "    if (h->nrx < want)\n        return;\n",
     ""),
    ("a continuing fragment taken with no PDU started",
     "    } else if (pb != HCI_PB_CONT || !h->rx_on) {",
     "    } else if (pb != HCI_PB_CONT) {"),
    ("ACL from another handle taken",
     "&d, &len) != HCI_OK || handle != h->conn) {",
     "&d, &len) != HCI_OK) {"),
    ("the advertising data's length byte left at 0",
     "        p[0] = h->adv.nadv;\n",
     ""),
    ("the advertising channel map left at 0",
     "        p[13] = 0x07;\n",
     ""),
    ("the host's state moved on only after the send (an answer inside send is lost)",
     "            h->ncmd_bytes = 0;\n            h->ncmd--;\n            h->cmds++;\n            h->wait_ms = 0;\n"
     "            if (h->t->send(h->t->ctx, HCI_CMD, pkt, nb) != 0) {",
     "            if (h->t->send(h->t->ctx, HCI_CMD, pkt, nb) == 0) {\n                h->ncmd_bytes = 0;\n"
     "                h->ncmd--;\n                h->cmds++;\n                h->wait_ms = 0;\n            }\n"
     "            if (0) {"),
)


HARNESS = ("tests/ble_mutants.py", "tests/ble_hci.c", "tests/ble/")


def target(old):
    return "hci.h" if old.startswith("#define") else "hci.c"


def one(work, n):
    name, old, new = MUTANTS[n]
    d = work / str(n)
    if d.exists():
        shutil.rmtree(d)
    shutil.copytree(LIB, d)
    f = d / target(old)
    f.write_text(f.read_text().replace(old, new))
    exe = d / "ble_hci"
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-w", "-I" + str(d),
                    "-I" + str(ROOT / "tests/ble"), "-o", str(exe), str(ROOT / "tests/ble_hci.c"),
                    str(ROOT / "tests/ble/hci_model.c"), str(d / "hci.c")], check=True)
    try:
        r = limits.run([str(exe), str(ROOT / "tests/ble")], capture_output=True, text=True, timeout=60)
        code, err = r.returncode, r.stderr
        if code < 0:                                          # killed by a signal, e.g. the memory limit
            code, err = None, "killed (signal %d)" % -code
    except subprocess.TimeoutExpired:
        code, err = None, "hung"
    caught = code == 1 and "FAIL" in err
    return caught, "  exit %s; %s" % (code, err.strip().splitlines()[-1] if err.strip() else "")


def main():
    out = Path(sys.argv[1])
    work = out / "ble_mutants"
    todo, stale = mutation.plan("ble mutants", [("firmware/ble/" + target(m[1]), m[1]) for m in MUTANTS], HARNESS)
    res = mutation.run(todo, lambda n: one(work, n))
    survived = 0
    for n, (name, old, _) in enumerate(MUTANTS):
        if n in stale:
            print("FAIL: mutant %r: its text is not in %s exactly once (the code moved: update the mutant)" % (name, target(old)))
            survived += 1
        elif n in res:
            caught, why = res[n]
            print("%s: %s" % ("caught" if caught else "SURVIVED", name))
            if not caught:
                print(why)
                survived += 1
    if work.exists():
        shutil.rmtree(work)
    if survived:
        sys.exit("FAIL: %d mutant(s) not caught" % survived)
    print(mutation.summary("ble mutants", len(MUTANTS), len(res)))

if __name__ == "__main__":
    main()
