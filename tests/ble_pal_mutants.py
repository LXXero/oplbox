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

sys.path.insert(0, str(Path(__file__).resolve().parent))
import limits
import mutation

ROOT = Path(__file__).resolve().parent.parent
PAL = "firmware/ble/cordio_port/pal_fm1.c"
DRV = "firmware/ble/fm1ble.c"
MEM_MB = 512      # each mutant's run: its address space (the unmutated run is measured in the commit message)

# (name, file, text, replacement)
MUTANTS = (
    ("a packet's timestamp is when it was seen, not when it started", PAL,
     "g_held_ts = t - air_us(g_held_n);", "g_held_ts = t;"),
    ("the response always counted late", PAL,
     "    if (g_done && !ahead) {             /* the event is over",
     "    if (1) {                            /* the event is over"),
    ("the advertising packet's completion waits for the event's end", PAL,
     "if (g_wait == W_TX && (g_held || g_done)) {", "if (g_wait == W_TX && g_done) {"),
    ("a received packet reported as a CRC failure", PAL,
     "rx(g_held_ok ? BB_STATUS_SUCCESS : BB_STATUS_CRC_FAILED", "rx(g_held_ok ? BB_STATUS_CRC_FAILED : BB_STATUS_SUCCESS"),
    ("a connection event armed at once, not at its due time", PAL,
     "fm1ble_op_conn(g_cfg.b, delay, g_rx_timeout)", "fm1ble_op_conn(g_cfg.b, 0, g_rx_timeout)"),
    ("the receive window Cordio asks for not used", PAL,
     "fm1ble_op_conn(g_cfg.b, delay, g_rx_timeout)", "fm1ble_op_conn(g_cfg.b, delay, 20)"),
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
    ("advertising events with hop 0 (6d8b497's one-channel shape: never heard, never ended on 38 and 39)", DRV,
     "    b->hop = 1;\n", "    b->hop = 0;\n"),
    ("the scan response not in the other TX slot of the events it is armed with", DRV,
     "op_buf(b, 1, rsp, nrsp, 1, b->op_slot ^ 1u);", "tx_slot(b, b->op_slot ^ 1u, 0);"),
    ("each event armed with no scan response from Cordio's operation (Cordio's own, late from the loop, the only one)", PAL,
     "fm1ble_op_adv(g_cfg.b, delay_to_due(), pdu, n, rsp, nrsp)", "fm1ble_op_adv(g_cfg.b, delay_to_due(), pdu, n, NULL, 0)"),
    ("the scan response kept from the first event, not the one Cordio has now (the stale SCAN_RSP)", PAL,
     "    nrsp = adv_rsp(&rsp);\n",
     "    { static const uint8_t *k; static uint16_t kn; if (!kn) kn = adv_rsp(&k); rsp = k; nrsp = kn; }\n"),
    ("a data PDU only in TX slot 0, whatever TXTOG says", DRV,
     "for (s = 0; s < 2u; s++) {\n            e16(b, s ? FM1BLE_E_TXPTR1", "for (s = 0; s < 1u; s++) {\n            e16(b, s ? FM1BLE_E_TXPTR1"),
    ("the received data header's SN and NESN dropped", DRV,
     "ahdr & 0xFFu : dhdr & 0x1Fu", "ahdr & 0xFFu : dhdr & 0x03u"),
    ("the advertising events' receive window (WINCNTL2) left at 0, as up to 3b39ab4", DRV,
     "    e16(b, FM1BLE_E_WINCNTL2, adv_win(b));\n    e16(b, FM1BLE_E_WIDEN0, 0);", "    e16(b, FM1BLE_E_WIDEN0, 0);"),
    ("the AGC gain table not written at power-up, as up to 3b39ab4", DRV,
     "    b->d.agc_table = (uint8_t)rx_agc(b);\n", "    b->d.agc_table = 0;\n"),
    ("the gain table written one word out of step", DRV,
     "wr(FM1BLE_AGC_TAB, FM1BLE_AGC_TABLE[i]);", "wr(FM1BLE_AGC_TAB, FM1BLE_AGC_TABLE[(i + 1u) % FM1BLE_AGC_WORDS]);"),
    ("a connection event polled like the rest, not waited through (43817aa: 1 packet in 18 events on the FM-1)", PAL,
     "if (!progress && !g_cfg.no_busy_wait && g_kind == K_CONN", "if (0 && !progress && !g_cfg.no_busy_wait && g_kind == K_CONN"),
    ("every connection runs the first setup of the order", PAL,
     "setup_nth(g_st.conns)", "setup_nth(0)"),
    ("a missed event moves to the next window setup within the connection (fcbfd86, 5838183)", PAL,
     "    g_cev++;\n", "    g_cev++;\n    if (!g_conn_rx)\n        g_set = setup_nth(g_cev);\n"),
    ("a setup that received a connection not kept for the next one", PAL,
     "        g_st.good = g_set;\n", "        (void)0;\n"),
    ("the setups tried in 5838183's order (the wide window first, the IR's three last)", PAL,
     "g_order[FM1BLE_CONN_SETUPS] = PAL_FM1_SETUP_ORDER;",
     "g_order[FM1BLE_CONN_SETUPS] = { FM1BLE_CONN_WIDE, FM1BLE_CONN_PAD, FM1BLE_CONN_KEEP, FM1BLE_CONN_WIDEN, "
     "FM1BLE_CONN_PADJ, FM1BLE_CONN_PADB, FM1BLE_CONN_PADW };"),
    ("the connection's summary leaves out the missed events' ends", PAL,
     "            sum_minmax(&g_sum[2], (int32_t)(t - g_due));\n", "            (void)0;\n"),
    ("the connection's summary does not count its packets", PAL,
     "                        g_sum[0] = (g_sum[0] & 0xFFFFu) | (g_cpk > 0xFFFFu ? 0xFFFFu : g_cpk) << 16;\n", ""),
    ("the setups from padj on count their arrivals from dueUsec, not from the pad's start", PAL,
     "g_set == FM1BLE_CONN_PAD || g_set >= FM1BLE_CONN_PADJ ? FM1BLE_CONN_PAD_US : 0u;", "g_set == FM1BLE_CONN_PAD ? FM1BLE_CONN_PAD_US : 0u;"),
    ("padj writes the slots as op_arm does, not JieLi's slots minus one", DRV,
     "uint32_t cnt = slots ? slots - 1u : 0u;", "uint32_t cnt = slots;"),
    ("padb's bit offset without bit 15", DRV,
     "ax_wr(FM1BLE_AX_BITOFF, (how & ARM_BIT15) ? bitoff | 0x8000u : bitoff);", "ax_wr(FM1BLE_AX_BITOFF, bitoff);"),
    ("padw leaves WINCNTL0 at 0x32", DRV,
     "            win0 = window_us;\n", "            ;\n"),
    ("the scheduler's timer fires when Cordio asks, not PAL_FM1_TIMER_LEAD_US earlier (fcbfd86: armed 90-300 us ahead)", PAL,
     "g_tmr_due = now() + (expUsec > lead ? expUsec - lead : 0u);", "g_tmr_due = now() + expUsec; (void)lead;"),
    ("the event log leaves out when the event ended", PAL,
     "            g_log[1] = (g_log[1] & 0xFFFFu) | s16((int32_t)(t - g_due)) << 16;\n", "            (void)0;\n"),
    ("the event log leaves out the packet", PAL,
     "                        g_log[0] |= 1u << 8 | (p.ok ? 1u << 9 : 0u);\n", ""),
    ("the event log leaves out the response sent", PAL,
     "            g_log[0] |= 1u << 10;\n", "            ;\n"),
    ("the CONNECT_IND's poll gap not measured", PAL,
     "g_st.cind_gap_us = t - g_last_poll;", "g_st.cind_gap_us = 0;"),
    ("the time of the last poll never kept", PAL,
     "        g_last_poll = t;\n", ""),
    ("the wide window not longer", DRV,
     "        window_us += 2u * FM1BLE_CONN_WIDE_US;\n", ""),
    ("the wide window not earlier", DRV,
     "        delay_us = delay_us > FM1BLE_CONN_WIDE_US ? delay_us - FM1BLE_CONN_WIDE_US : 0u;\n", ""),
    ("the window setup not handed to the driver", PAL,
     "        fm1ble_op_conn_setup(g_cfg.b, g_set);\n", ""),
    ("the padded window not longer", DRV,
     "        window_us += 2u * FM1BLE_CONN_PAD_US;\n", ""),
    ("the padded window's arrival positions count a full 625 us before dueUsec when the start was clamped at 0", PAL,
     "g_win_early = delay > pad ? pad : delay;", "g_win_early = pad;"),
    ("the padded window's arrival positions count from dueUsec, not from the window's start", PAL,
     "int32_t arr = (int32_t)(g_held_ts - g_due) + (int32_t)g_win_early;", "int32_t arr = (int32_t)(g_held_ts - g_due);"),
    ("the padded window's start not clamped at 0 when the event is armed under 625 us before its due time", DRV,
     "delay_us = delay_us > FM1BLE_CONN_PAD_US ? delay_us - FM1BLE_CONN_PAD_US : 0u;",
     "delay_us = delay_us - FM1BLE_CONN_PAD_US;"),
    ("a connection event's window not the one Cordio asks for (WINCNTL2 = WINCNTL0's 0x32)", DRV,
     "e16(b, FM1BLE_E_WINCNTL2, window_us > 0xFFFFu ? 0xFFFFu : window_us);", "e16(b, FM1BLE_E_WINCNTL2, FM1BLE_WIN0);"),
    ("a stale packet flag given to Cordio as the central's packet (9c47113: the anchor 1192 us early)", PAL,
     "< -(int32_t)PAL_FM1_STALE_US) {", "< -(int32_t)1000000) {"),
    ("stale packets not counted in the connection's summary", PAL,
     "            g_cstale++;\n", ""),
    ("an event ended by its deadline stopped, not halted", PAL,
     "if (fm1ble_op_halt(g_cfg.b) != FM1BLE_OK)", "if (0 && fm1ble_op_halt(g_cfg.b) != FM1BLE_OK)"),
    ("INTIME puts a late response in anyway", PAL,
     "lat > PAL_FM1_TIFS_US && !g_done", "lat > 1000000u && !g_done"),
    ("every connection runs AIR, whatever the TX order", PAL,
     "tx_nth(g_st.conns);", "FM1BLE_TX_AIR;"),
    ("a TX mode that carried L2CAP not kept for the next connection", PAL,
     "        g_st.good_tx = g_tx;\n", "        (void)0;\n"),
    ("the event log's word d leaves out the TX mode", PAL,
     "g_log[3] = (uint32_t)(g_tx & 3u) << 22", "g_log[3] = 0u << 22"),
    ("the event log's word d leaves out Cordio's response time", PAL,
     "            g_log[3] = (g_log[3] & 0x00FFFFFFu) | sat(lat / 2u, 0xFFu) << 24;\n", ""),
    ("ARQ: every packet new to Cordio, whatever its SN says (the central's repeats taken twice)", PAL,
     "        isnew = snnew;", "        isnew = 1;"),
    # new by the central's SN, not RXTOG (round 2 of the resumed goal, pal_fm1.h PAL_FM1_SNX)
    ("ARQ: new when RXTOG moved, as up to 1b8dea6 (BlueZ's request at event 33 lost)", PAL,
     "        isnew = snnew;", "        isnew = rxch;"),
    ("ARQ: the SN expected next not changed at a new packet", PAL,
     "    if (snnew)\n        g_rx_exp ^= 1u;\n", ""),
    ("ARQ: a packet with a bad CRC changes the SN expected next", PAL,
     "        g_bad_since = 1;\n        return 0;", "        g_bad_since = 1;\n        g_rx_exp ^= sn == g_rx_exp;\n        return 0;"),
    ("ARQ: the SN read from RXDHDR bit 4 (MD), not bit 3", PAL,
     "sn = (uint8_t)(h >> 3 & 1u)", "sn = (uint8_t)(h >> 4 & 1u)"),
    ("ARQ: the SN expected at a connection's start left from the last connection", PAL,
     "    g_rx_exp = 0;                                       /* the central's first SN", "    /* the central's first SN"),
    ("the cross-check counts no packet new by the SN with RXTOG still", PAL,
     "        snx_add(0, h, rxch, tmoved, 0);", "        (void)0;"),
    ("the cross-check takes a NESN that changed as TXTOG still", PAL,
     "tmoved != (nesn != g_nesn_p)", "tmoved != (nesn == g_nesn_p)"),
    ("ARQ: every packet acknowledges Cordio's last PDU, whatever TXTOG says", PAL,
     "ack = (uint8_t)(g_wrote && moved);", "ack = 1;"),
    ("ARQ: a response that did not go in counted as acknowledged when TXTOG moves", PAL,
     "ack = (uint8_t)(g_wrote && moved);", "ack = moved;"),
    ("ARQ: Cordio's SN not followed", PAL,
     "        g_csn = (uint8_t)(pdu[0] >> 3 & 1u);\n", "        g_csn = 0;\n"),
    ("ARQ: a response too late for T_IFS put in anyway", PAL,
     "(g_tx == FM1BLE_TX_INTIME || g_tx == FM1BLE_TX_ARQ) && !ahead && lat > PAL_FM1_TIFS_US",
     "g_tx == FM1BLE_TX_INTIME && !ahead && lat > PAL_FM1_TIFS_US"),
    ("ARQ: no empty PDU in the other slot at the arm (an acknowledgement sends an older PDU again)", DRV,
     "            memset(b->blk + b->tx[s], 0, FM1BLE_TX_SIZE);\n            e16(b, s ? FM1BLE_E_TXPTR1 : FM1BLE_E_TXPTR0, (uint32_t)b->tx[s] + "
     "FM1BLE_TX_HDR);\n            e16(b, off, (g16(b, off) & 4u) | 1u);\n", "            (void)off;\n"),
    ("ARQ: the response in the other slot from the one TXTOG names", DRV,
     "        k = (int)(g16(b, FM1BLE_E_TXTOG) & 1u);     /* ahead", "        k = (int)((g16(b, FM1BLE_E_TXTOG) & 1u) ^ 1u); /* ahead"),
    ("ARQ: a response always into slot 0, whatever TXTOG names", DRV,
     "    if (!adv && b->conn_tx == FM1BLE_TX_ARQ) {       /* ARQ", "    if (0 && !adv && b->conn_tx == FM1BLE_TX_ARQ) {  /* ARQ"),
    # ARQ ahead and the chip's SN (round 7, pal_fm1.h PAL_FM1_L2_GOOD, PAL_FM1_ARQ_STUCK)
    ("ARQ ahead: the response into the slot TXTOG names, as in T_IFS", DRV,
     "            k ^= 1;\n", "            k ^= 0;\n"),
    ("ARQ ahead: the arm empties the slot a response waits in", DRV,
     "        if (!b->arq_ahead || b->ahead_pend != 1u + s) {", "        if (1) {"),
    ("ARQ ahead: Cordio told its PDU was acknowledged when TXTOG moved, as in T_IFS", PAL,
     "ack = (uint8_t)(g_aw && tog == g_aw - 1u);", "ack = (uint8_t)(g_wrote && moved);"),
    ("ARQ ahead: a response later than PAL_FM1_TIFS_US left out", PAL,
     "g_tx == FM1BLE_TX_ARQ) && !ahead && lat > PAL_FM1_TIFS_US", "g_tx == FM1BLE_TX_ARQ) && lat > PAL_FM1_TIFS_US"),
    ("bit 2 left as it is when TXTOG moved at a connection's first packet", PAL,
     "        if (tog != g_tog0) {", "        if (0) {"),
    ("bit 2 never changed after PAL_FM1_ARQ_STUCK packets the central took none of", PAL,
     "} else if (snnew && ++g_stuck >= PAL_FM1_ARQ_STUCK) {", "} else if (0) {"),
    ("ARQ's ways: every connection in T_IFS", PAL,
     "(uint8_t)!(g_st.arq_conns & 1u);", "(uint8_t)0;"),
    ("the capture leaves out Cordio's PDUs", PAL,
     "            cap_add((uint8_t)(1u | (ahead", "            if (0) cap_add((uint8_t)(1u | (ahead"),
    ("the capture takes Cordio's PDU each time it is handed again", PAL,
     "            if (memcmp(g_cap_last, key, sizeof key)) {", "            if (1) {"),
    ("a TX mode kept on packets alone (2e6d430 kept INTIME on 462, no GATT)", PAL,
     "if (g_st.conns && g_cl2)", "if (g_st.conns && g_cpk >= PAL_FM1_GOOD)"),
    ("the trace leaves out that Cordio's response went in", PAL,
     "        trace_or(1u << 22);\n", "        ;\n"),
    ("the link-layer counts leave out the central's L2CAP PDUs", PAL,
     "            *c = sat_add8(*c, 0);\n", "            ;\n"),
    ("ARQ: Cordio told MD 0, one exchange per event (5a47303: the central's L2CAP PDU after LL_LENGTH_REQ lost)", PAL,
     "(isnew ? g_cnesn : g_cnesn ^ 1u) << 3 | 1u << 4);", "(isnew ? g_cnesn : g_cnesn ^ 1u) << 3);"),
    ("Cordio's response completed only at the event's end (a later packet held over the next one)", PAL,
     "if (g_wait == W_TIFS_TX && (g_done || (g_held && g_kind == K_CONN))) {", "if (g_wait == W_TIFS_TX && g_done) {"),
    # round 8: the central's repeats in the PDU capture, the fragment trace (pal_fm1.h PAL_FM1_CAP, PAL_FM1_FRAG)
    ("the central's PDUs Cordio was told were repeats left out of the capture", PAL,
     "if (isnew || memcmp(key, g_cap_c, sizeof key))", "if (isnew)"),
    ("a repeat captured as new to Cordio", PAL,
     "(isnew ? 1u << 2 : 0u)", "1u << 2"),
    ("the fragment trace never names its connection", PAL,
     "g_st.frag_conn = g_st.conns;", "g_st.frag_conn = 0;"),
    ("a start that fits its data PDU taken for a fragment", PAL,
     "(uint32_t)(pdu[2] | pdu[3] << 8) + 4u > pdu[1]", "(uint32_t)(pdu[2] | pdu[3] << 8) + 4u >= pdu[1]"),
    ("the fragment trace keeps only the packet it started at", PAL,
     "g_st.frag_conn == g_st.conns && g_st.frag_n && g_st.frag_n < PAL_FM1_FRAG", "0"),
)


def sh(cmd):
    return subprocess.run(["bash", "-c", cmd], capture_output=True, text=True, cwd=ROOT)


HARNESS = ("tests/ble_pal_mutants.py", "tests/ble_pal.c", "tests/ble/")


def one(work, cr_def, cr_inc, objs, n):
    name, path, old, new = MUTANTS[n]
    d = work / str(n)
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    files = {PAL: d / "pal_fm1.c", DRV: d / "fm1ble.c"}
    for src, dst in files.items():
        shutil.copy(ROOT / src, dst)
    f = files[path]
    f.write_text(f.read_text().replace(old, new))
    exe = d / "ble_pal"
    inc = ["-I" + str(ROOT / "firmware/ble"), "-I" + str(ROOT / "firmware/ble/cordio_port"), "-I" + str(ROOT / "tests/ble")]
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-fshort-enums", *cr_def, *["-I" + str(ROOT / i[2:]) for i in cr_inc], *inc, "-c", "-o", str(d / "pal_fm1.o"), str(files[PAL])], check=True)
    subprocess.run(["cc", "-std=gnu11", "-O1", "-w", "-fshort-enums", *inc, "-Wl,--gc-sections", "-o", str(exe),
                    str(ROOT / "tests/ble_pal.c"), str(ROOT / "tests/ble/bb_model.c"), str(ROOT / "tests/ble/air_central.c"), str(files[DRV]),
                    str(ROOT / "firmware/ble/ble_midi.c"), str(d / "pal_fm1.o"), *objs], check=True)
    try:
        r = limits.run([str(exe)], mem_mb=MEM_MB, capture_output=True, text=True, timeout=120)
        code, err = r.returncode, r.stderr
        if code < 0:                                          # killed by a signal, e.g. the memory limit
            code, err = None, "killed (signal %d)" % -code
    except subprocess.TimeoutExpired:
        code, err = None, "hung"
    caught = code == 1 and "FAIL" in err
    return caught, "  exit %s; %s" % (code, err.strip().splitlines()[-1] if err.strip() else "")


def main():
    out = Path(sys.argv[1]).resolve()
    work = out / "ble_pal_mutants"
    cr_inc = sh("find firmware/ble/cordio -name '*.h' -printf '-I%h\\n' | sort -u | tr '\\n' ' '").stdout.split()
    cr_def = ["-D" + l.strip() for l in (ROOT / "firmware/ble/cordio_port/DEFINES.txt").read_text().splitlines() if l.strip()]
    objs = [str(p) for p in sorted((out / "cordio").glob("*.o")) if "pal_stubs_host" not in p.name]
    todo, stale = mutation.plan("ble_pal mutants", [(m[1], m[2]) for m in MUTANTS], HARNESS)
    res = mutation.run(todo, lambda n: one(work, cr_def, cr_inc, objs, n))
    survived = 0
    for n, (name, path, *_) in enumerate(MUTANTS):
        if n in stale:
            print("FAIL: mutant %r: its text is not in %s exactly once (the code moved: update the mutant)" % (name, path))
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
    print(mutation.summary("ble_pal mutants", len(MUTANTS), len(res)))

if __name__ == "__main__":
    main()
