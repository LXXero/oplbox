#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Mutation check of the NimBLE port's test (tests/ble_nimble.c, docs/BLUETOOTH.md "Cordio path as built" step 1): each
mutant is a copy of firmware/ble/ble_nimble.c or firmware/ble/ble_nimble_npl.c with one deliberate bug, built with the
vendored NimBLE objects tests/run_host.sh made and run through every scenario. The test must fail on every one (a FAIL
line, a non-zero exit, or a hang killed by the time limit).

    ble_nimble_mutants.py <out>      <out> is build/host (with nimble/*.o from run_host.sh); exit 1 if a mutant survives
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIB = ROOT / "firmware/ble"
NB = "firmware/ble/nimble"
INC = ["-Ifirmware/ble/nimble_port", "-Ifirmware/ble", "-Itests/ble", f"-I{NB}/nimble/include", f"-I{NB}/nimble/host/include",
       f"-I{NB}/nimble/transport/include", f"-I{NB}/porting/nimble/include", f"-I{NB}/nimble/host/services/gap/include",
       f"-I{NB}/nimble/host/services/gatt/include", f"-I{NB}/nimble/host/util/include"]
SCENARIOS = ("session", "silent", "refused", "refusing", "never", "npl")

# (file, name, text, replacement): the text is in the file exactly once
MUTANTS = (
    ("ble_nimble.c", "a refused packet is dropped instead of kept",
     "    g_npending = (uint16_t)(1 + n);\n    return 0;", "    return 0;"),
    ("ble_nimble.c", "refusals are not counted when retried",
     "    else\n        g_st.hci_refused++;\n}", "}"),
    ("ble_nimble.c", "a command goes out with one byte too few",
     "tx(HCI_CMD, c, (uint16_t)(3 + c[2]))", "tx(HCI_CMD, c, (uint16_t)(2 + c[2]))"),
    ("ble_nimble.c", "commands still go out after the start failed",
     "g_halt ? -1 : tx(", "tx("),
    ("ble_nimble.c", "a failed start does not stop the commands",
     "    g_st.failures++;\n    g_halt = 1;", "    g_st.failures++;"),
    ("ble_nimble.c", "a failed start is not reported",
     "    g_st.state = BLE_NIMBLE_FAILED;\n    g_st.fail = 0x100 + reason;", "    g_st.fail = 0x100 + reason;"),
    ("ble_nimble.c", "an event with a wrong length byte goes to the host",
     "p[1] + 2u != n || ", ""),
    ("ble_nimble.c", "the first read of the characteristic is refused",
     "        return 0;                       /* the first read answers", "        return BLE_ATT_ERR_READ_NOT_PERMITTED; /* the first read answers"),
    ("ble_nimble.c", "a written packet does not reach the MIDI input",
     "            ble_midi_in_write(g_cfg.in, pkt, n);", "            (void)pkt;"),
    ("ble_nimble.c", "writes are not counted",
     "        g_st.writes++;\n", ""),
    ("ble_nimble.c", "the characteristic cannot notify",
     " | BLE_GATT_CHR_F_NOTIFY;", ";"),
    ("ble_nimble.c", "the characteristic has no write without response",
     " | BLE_GATT_CHR_F_WRITE_NO_RSP", ""),
    ("ble_nimble.c", "the characteristic takes the service's UUID",
     "memcpy(g_chr_uuid.value, ble_midi_char_uuid, 16);", "memcpy(g_chr_uuid.value, ble_midi_service_uuid, 16);"),
    ("ble_nimble.c", "no connection interval request",
     "if (d.conn_itvl > BLE_MIDI_CONN_INTERVAL_MAX)", "if (0)"),
    ("ble_nimble.c", "the interval asked for is 30 ms",
     "up.itvl_max = BLE_MIDI_CONN_INTERVAL_MAX;", "up.itvl_max = 24;"),
    ("ble_nimble.c", "no advertising after a disconnection",
     "        g_st.mtu = BLE_ATT_MTU_DFLT;\n        adv_start();\n        break;\n    case BLE_GAP_EVENT_CONN_UPDATE",
     "        g_st.mtu = BLE_ATT_MTU_DFLT;\n        break;\n    case BLE_GAP_EVENT_CONN_UPDATE"),
    ("ble_nimble.c", "a notification longer than MTU - 3 is sent",
     "n + 3u > g_st.mtu", "n > g_st.mtu + 100u"),
    ("ble_nimble.c", "a notification is sent before the central subscribes",
     " || !g_st.notify_on ||", " ||"),
    ("ble_nimble.c", "the MIDI input keeps the old connection's messages",
     "        if (g_cfg.in)\n            ble_midi_in_reset(g_cfg.in);\n", ""),
    ("ble_nimble.c", "advertising every 12.5 ms",
     "g_cfg.adv_itvl_min : 0x30;", "g_cfg.adv_itvl_min : 0x14;"),
    ("ble_nimble.c", "the scan response has the wrong name",
     "ble_midi_scan_rsp(rsp, g_cfg.name)", "ble_midi_scan_rsp(rsp, \"OPL\")"),
    ("ble_nimble.c", "subscription taken from any attribute",
     "ev->subscribe.attr_handle == g_val_handle", "ev->subscribe.attr_handle != g_val_handle"),
    ("ble_nimble.c", "the MTU is not followed",
     "        g_st.mtu = ev->mtu.value;", "        (void)ev;"),
    ("ble_nimble.c", "the host ends a connection with the wrong reason",
     "BLE_ERR_REM_USER_CONN_TERM) == 0", "BLE_ERR_CONN_TERM_LOCAL) == 0"),
    ("ble_nimble.c", "a poll does not retry a held packet",
     "void ble_nimble_poll(void)\n{\n    tx_retry();\n", "void ble_nimble_poll(void)\n{\n"),
    ("ble_nimble.c", "a wait does not retry a held packet",
     "static void pump(void *ctx)\n{\n    tx_retry();\n", "static void pump(void *ctx)\n{\n"),
    ("ble_nimble.c", "retry does not start again",
     "    g_st.state = BLE_NIMBLE_STARTING;\n    g_st.fail = 0;\n    g_npending = 0;\n    g_halt = 0;",
     "    g_st.state = BLE_NIMBLE_STARTING;\n    g_st.fail = 0;\n    g_npending = 0;"),
    ("ble_nimble_npl.c", "an event queued twice is queued twice",
     "    if (ev->q)                      /* already queued: stays where it is */\n        return;\n", ""),
    ("ble_nimble_npl.c", "the queue's tail is not cleared when it empties",
     "    if (!evq->head)\n        evq->tail = 0;\n", ""),
    ("ble_nimble_npl.c", "a wait never times out",
     "if (timeout != BLE_NPL_TIME_FOREVER && (uint32_t)(ble_npl_time_get() - t0) >= timeout)",
     "if (timeout != BLE_NPL_TIME_FOREVER && 0)"),
    ("ble_nimble_npl.c", "a semaphore wait does not run the pump",
     "        g_pump(g_ctx);\n    }\n    sem->tokens--;", "    }\n    sem->tokens--;"),
    ("ble_nimble_npl.c", "re-arming a callout leaves it in the list",
     "    if (co->active)\n        callout_unlink(co);\n    if (co->ev.q)\n        ble_npl_eventq_remove(co->ev.q, &co->ev);\n    co->due",
     "    if (co->ev.q)\n        ble_npl_eventq_remove(co->ev.q, &co->ev);\n    co->due"),
    ("ble_nimble_npl.c", "stopping a callout leaves it armed",
     "    if (co->active)\n        callout_unlink(co);\n    if (co->ev.q)\n        ble_npl_eventq_remove(co->ev.q, &co->ev);\n}",
     "    if (co->ev.q)\n        ble_npl_eventq_remove(co->ev.q, &co->ev);\n}"),
    ("ble_nimble_npl.c", "callouts fire a second early",
     "if ((int32_t)(now - co->due) >= 0) {\n            callout_unlink", "if ((int32_t)(now - co->due) >= -1000) {\n            callout_unlink"),
    ("ble_nimble_npl.c", "the main loop runs no more than one event a step",
     "#define BLE_NPL_RUN_MAX 32", "#define BLE_NPL_RUN_MAX 1"),
    ("ble_nimble_npl.c", "the main loop never stops running events",
     "while (n < BLE_NPL_RUN_MAX && (ev", "while ((ev"),
)


def run(cmd, **kw):
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, **kw)


def main():
    out = Path(sys.argv[1]).resolve()
    work = out / "nimble_mut"
    work.mkdir(parents=True, exist_ok=True)
    keep = [str(p) for p in sorted((out / "nimble").glob("*.o")) if p.name not in ("ble_nimble.o", "ble_nimble_npl.o")]
    if not keep:
        sys.exit("no vendored objects in build/host/nimble: run tests/run_host.sh")
    survived = []
    for i, (fname, name, old, new) in enumerate(MUTANTS):
        src = (LIB / fname).read_text()
        if src.count(old) != 1:
            sys.exit(f"mutant '{name}': text found {src.count(old)} times in {fname}")
        mutated = work / fname
        mutated.write_text(src.replace(old, new, 1))
        other = "ble_nimble_npl.c" if fname == "ble_nimble.c" else "ble_nimble.c"
        exe = work / "ble_nimble_mut"
        cc = run(["cc", "-std=gnu11", "-O1", "-w", "-D_POSIX_C_SOURCE=200809L", *INC, "-o", str(exe), "tests/ble_nimble.c",
                  "tests/ble/hci_model.c", "firmware/ble/hci.c", "firmware/ble/ble_midi.c", str(mutated), str(LIB / other), *keep])
        if cc.returncode:
            sys.exit(f"mutant '{name}' does not build:\n{cc.stderr[:2000]}")
        caught = False
        for sc in SCENARIOS:
            try:
                r = run([str(exe), "tests/ble", str(work), sc], timeout=30)
            except subprocess.TimeoutExpired:
                caught = True
                break
            if r.returncode != 0:
                caught = True
                break
        print(f"  {'caught ' if caught else 'SURVIVED'} {name}")
        if not caught:
            survived.append(name)
    print(f"ble_nimble mutants: {len(MUTANTS) - len(survived)} of {len(MUTANTS)} caught")
    if survived:
        sys.exit("FAIL: survived: " + "; ".join(survived))


main()
