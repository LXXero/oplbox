#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Facts about JieLi's BLE baseband, read out of btctrler.a's LLVM IR (docs/BLUETOOTH_RADIO.md).

The archive's members are LLVM bitcode. This tool turns the members that hold the LE baseband port
(RF_ble, analog, analog_ble, RF, le_low_power, direct_test_dut, btagc, and cpu.a's wlc) back into LLVM IR with
llvm-dis and prints what that IR does to the hardware as data: which absolute address is read or written, with what
constant or and/or mask, in which order, which calls follow with which constant arguments, and the byte offsets and
IR value names of the fields of the per-link parameter block that the baseband reads from RAM.

It is read-only. It writes nothing but its own output to stdout, and the unpacked archives live in a temporary
directory that is removed at exit. Nothing it prints is code: no control flow beyond "branch on this comparison",
no IR text, only numbers, offsets and the names the IR carries (docs/BLUETOOTH_RADIO.md "Clean-room rules").

  default            the register sequences and tables docs/BLUETOOTH_RADIO.md quotes, one block per function
  --layout           the parameter block (struct ble_param inside struct ble4_hw): offset, size, IR value name
  --registers        every absolute address above 0x10000 that each member touches, by function
  --steps MEMBER FN  the step view of the named functions of one member (e.g. --steps RF_ble le_hw_init)
  --check            fails when the archive is not the one the doc was written from, or when a fact the doc
                     quotes no longer comes out of the IR

Needs the JieLi AC79 SDK ($AC79_SDK, default ~/fw-AC79_AIoT_SDK) and a system llvm-dis (LLVM 23 was used; the
bitcode is clang 4.0.1's, whose debug information llvm-dis drops with a warning, which is fine).
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SDK = Path(os.environ.get("AC79_SDK", Path.home() / "fw-AC79_AIoT_SDK"))
LIBA = SDK / "cpu/wl82/liba"
ARCHIVES = {
    "btctrler": "9ef4262444b49171c4a5eab906e3939b1d4943350ac4993c18d8474d01d773de",
    "cpu": "00cea21d3c8d99487821f2c2b203f2c5a04b2cc8421bc05e9b875a0d70e42c62",
}
# (archive, member) -> source file the member was built from (read from the IR's source_filename)
MEMBERS = [("btctrler", m) for m in ("RF_ble", "RF_ble5", "RF", "analog", "analog_ble", "le_low_power", "direct_test_dut", "btagc")] + \
          [("cpu", "wlc")]
REGION = 0x10000          # the register blocks this tool reports start here (lsfr_base, bsfr_base, psfr_base ...)


# ---------------------------------------------------------------- unpacking
def sha256(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def load_irs(tmp):
    """llvm-dis every wanted member; return {(archive, member): cleaned IR text}."""
    out = {}
    for arc in ARCHIVES:
        d = Path(tmp) / arc
        d.mkdir()
        subprocess.run(["ar", "x", str(LIBA / f"{arc}.a")], cwd=d, check=True)
    for arc, mem in MEMBERS:
        obj = Path(tmp) / arc / f"{mem}.c.o"
        r = subprocess.run(["llvm-dis", str(obj), "-o", "-"], capture_output=True, text=True, check=True)
        keep = [re.sub(r", !(dbg|tbaa|llvm\.loop) ![0-9]+", "", l) for l in r.stdout.split("\n")
                if not l.startswith("!") and "llvm.dbg" not in l]
        out[(arc, mem)] = "\n".join(keep)
    return out


# ---------------------------------------------------------------- type layout (pi32v2: i64 and ptr align 4)
class Types:
    def __init__(self, src):
        self.src = src
        self.defs = {m.group(1): m.group(2) for m in re.finditer(r"^(%[\w.]+) = type (.*)$", src, re.M)}
        self.cache = {}

    def split(self, inner):
        out, depth, cur = [], 0, ""
        for ch in inner:
            depth += ch in "[{<"
            depth -= ch in "]}>"
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
            else:
                cur += ch
        if cur.strip():
            out.append(cur.strip())
        return out

    def size(self, t):
        """(size, alignment) of an IR type."""
        t = t.strip()
        if t in ("i1", "i8"):
            return (1, 1)
        if t == "i16":
            return (2, 2)
        if t in ("i32", "ptr", "float"):
            return (4, 4)
        if t in ("i64", "double"):
            return (8, 4)
        m = re.fullmatch(r"\[(\d+) x (.*)\]", t)
        if m:
            s, a = self.size(m.group(2))
            return (int(m.group(1)) * s, a)
        if t.startswith("%"):
            if t not in self.cache:
                self.cache[t] = self.size(self.defs[t]) if t in self.defs else (4, 4)
            return self.cache[t]
        if t.startswith("{") or t.startswith("<{"):
            return self.aggregate(self.fields(t))[0:2]
        return (4, 4)

    def fields(self, t):
        body = self.defs.get(t, t) if t.startswith("%") else t
        inner = body[2:-2] if body.startswith("<{") else body[1:-1]
        return self.split(inner)

    def aggregate(self, fl, packed=False):
        off, top = 0, 1
        for f in fl:
            s, a = self.size(f)
            a = 1 if packed else a
            off = (off + a - 1) // a * a + s
            top = max(top, a)
        return ((off + top - 1) // top * top, top, None)

    def field_offsets(self, t):
        body = self.defs.get(t, t) if t.startswith("%") else t
        packed = body.startswith("<{")
        off, res = 0, []
        for f in self.fields(t):
            s, a = self.size(f)
            a = 1 if packed else a
            off = (off + a - 1) // a * a
            res.append((off, f))
            off += s
        return res

    def gep(self, ty, idx):
        """Byte offset and final type of a getelementptr ty, idx... with constant indices."""
        off, cur = idx[0] * self.size(ty)[0], ty.strip()
        for i in idx[1:]:
            m = re.fullmatch(r"\[(\d+) x (.*)\]", cur)
            if m:
                cur = m.group(2)
                off += i * self.size(cur)[0]
            else:
                o, f = self.field_offsets(cur)[i]
                off += o
                cur = f
        return off, cur


# ---------------------------------------------------------------- the access view of one function
def fmt(e, addr=None):
    t = e[0]
    if t == "c":
        n = e[1] & 0xFFFFFFFF
        return "0x%X" % n if n > 9 else str(n)
    if t in ("arg", "?"):
        return e[1] if len(e) > 1 else "?"
    if t == "ld":
        return "rd[%s]" % addr_str(e[1])
    if t == "ptr":
        return addr_str(e)
    if t == "cmp":
        return "(%s %s %s)" % (fmt(e[2]), e[1], fmt(e[3]))
    if t == "ret":
        return "%s()" % e[1]
    if t == "sel":
        return "(%s ? %s : %s)" % (fmt(e[1]), fmt(e[2]), fmt(e[3]))
    if t in ("and", "or"):
        ops, cur = [], e
        while cur[0] in ("and", "or") and cur[2][0] == "c":
            ops.append((cur[0], cur[2][1] & 0xFFFFFFFF))
            cur = cur[1]
        if addr is not None and cur[0] == "ld" and cur[1] == addr:
            return "rmw " + " ".join("or 0x%X" % c if o == "or" else "clear 0x%X" % (~c & 0xFFFFFFFF)
                                     for o, c in reversed(ops))
    return "(%s %s %s)" % (t, fmt(e[1]), fmt(e[2])) if len(e) > 2 else "(%s %s)" % (t, fmt(e[1]))


def addr_str(p):
    if p[0] == "ptr":
        base, off = p[1], p[2]
        if base[0] == "c":
            return "%05X" % (base[1] + off)
        return "%s+0x%X" % (fmt(base), off) if off else fmt(base)
    return fmt(p)


def steps(src, types, name, quiet_branches=False):
    """The access view of function `name` as a list of lines."""
    m = re.search(r"define [^\n]*@" + re.escape(name) + r"\((.*?)\)[^\n]*\{\n(.*?)\n}\n", src, re.S)
    if not m:
        return ["NOFUNC " + name]
    out = ["===== %s" % name]
    val = {pn: ("arg", pn) for pn in re.findall(r"%[\w.]+", m.group(1))}

    def v(x):
        x = x.strip()
        if re.fullmatch(r"-?\d+", x):
            return ("c", int(x))
        if x == "null":
            return ("c", 0)
        return val.get(x, ("?", x))

    for l in m.group(2).split("\n"):
        l = l.strip()
        if not l:
            continue
        mm = re.match(r"^([\w.]+):", l)
        if mm:
            out.append(" [%s]" % mm.group(1))
            continue
        mm = re.match(r"(%[\w.]+) = getelementptr (?:inbounds )?(?:nuw )?(.*?), ptr (%[\w.]+|@[\w.]+|null)"
                      r"((?:, i\d+ -?\d+|, i\d+ %[\w.]+)*)$", l)
        if mm:
            base = mm.group(3)
            bv = val.get(base, ("ptr", ("?", base), 0)) if base.startswith("%") else \
                ("ptr", ("c", 0), 0) if base == "null" else ("ptr", ("?", base), 0)
            if bv[0] != "ptr":
                bv = ("ptr", bv, 0)
            try:
                off, _ = types.gep(mm.group(2), [int(x) for x in re.findall(r", i\d+ (-?\d+|%[\w.]+)", mm.group(4))])
                val[mm.group(1)] = ("ptr", bv[1], bv[2] + off)
            except (ValueError, IndexError, KeyError):
                val[mm.group(1)] = ("ptr", ("?", mm.group(1)), 0)
            continue
        mm = re.match(r"(%[\w.]+) = (?:bitcast|inttoptr|ptrtoint) \w+ (%[\w.]+) to", l)
        if mm:
            val[mm.group(1)] = val.get(mm.group(2), ("?", mm.group(2)))
            continue
        mm = re.match(r"(%[\w.]+) = (?:zext|trunc|sext) \S+ ([^ ]+) to", l)
        if mm:
            val[mm.group(1)] = v(mm.group(2))
            continue
        mm = re.match(r"(%[\w.]+) = load (?:volatile )?(\w+), ptr (.*?)(?:,|$)", l)
        if mm:
            p = mm.group(3).strip()
            im = re.match(r"inttoptr \(i32 (\d+) to ptr\)", p)
            pv = val.get(p) if p.startswith("%") else None
            if im:
                val[mm.group(1)] = ("ld", ("ptr", ("c", 0), int(im.group(1))))
            elif pv and pv[0] == "ptr":
                val[mm.group(1)] = ("ld", pv)
            elif p.startswith("@"):
                val[mm.group(1)] = ("ld", ("ptr", ("?", p), 0))
            else:
                val[mm.group(1)] = ("?", mm.group(1))
            continue
        mm = re.match(r"(%[\w.]+) = (and|or|xor|shl|lshr|ashr|add|sub|mul) (?:nsw |nuw |exact )*i\d+ ([^,]+), ([^,]+)$", l)
        if mm:
            val[mm.group(1)] = (mm.group(2), v(mm.group(3)), v(mm.group(4)))
            continue
        mm = re.match(r"(%[\w.]+) = icmp (\w+) i\d+ ([^,]+), ([^,]+)$", l)
        if mm:
            val[mm.group(1)] = ("cmp", mm.group(2), v(mm.group(3)), v(mm.group(4)))
            continue
        mm = re.match(r"(%[\w.]+) = select i1 ([^,]+), i\d+ ([^,]+), i\d+ ([^,]+)$", l)
        if mm:
            val[mm.group(1)] = ("sel", v(mm.group(2)), v(mm.group(3)), v(mm.group(4)))
            continue
        mm = re.match(r"store (?:volatile )?(\w+) (.*), ptr (inttoptr \(i32 \d+ to ptr\)|[%@][\w.]+|null)(?:, align \d+)?$", l)
        if mm:
            p, e = mm.group(3).strip(), v(mm.group(2)) if not mm.group(2).startswith("ptrtoint") else ("arg", "&table")
            im = re.match(r"inttoptr \(i32 (\d+) to ptr\)", p)
            if im:
                pv = ("ptr", ("c", 0), int(im.group(1)))
            elif p.startswith("%"):
                pv = val.get(p, ("?", p))
            else:
                pv = ("ptr", ("?", p), 0)
            isp = pv[0] == "ptr"
            out.append("  st.%s %s <- %s" % (mm.group(1), addr_str(pv) if isp else fmt(pv), fmt(e, pv if isp else None)))
            continue
        mm = re.match(r"(?:%[\w.]+ = )?(?:tail )?call [^@]*@([\w.]+)\((.*)\)", l)
        if mm:
            fn = mm.group(1)
            if fn.startswith("llvm") or fn in ("log_print", "printf", "puts", "putchar", "printf_buf"):
                continue
            args = re.findall(r"(?:\w+ )?(?:zeroext |signext |nonnull |readonly )*(?:i\d+|ptr) "
                              r"(?:zeroext |signext |nonnull )*([^,]+?)(?:,|$)", mm.group(2))
            out.append("  call %s(%s)" % (fn, ", ".join(fmt(v(x)) for x in args)))
            rm = re.match(r"(%[\w.]+) = ", l)
            if rm:
                val[rm.group(1)] = ("ret", fn)
            continue
        mm = re.match(r"br i1 (%[\w.]+), label %([\w.]+), label %([\w.]+)", l)
        if mm and not quiet_branches:
            out.append("  br %s ? %s : %s" % (fmt(val.get(mm.group(1), ("?", mm.group(1)))), mm.group(2), mm.group(3)))
    return out


def names_of(src):
    """IR names the code gives the fields of the per-link block: {name: field index} (ble4_hw -> ble_param)."""
    res = {}
    for m in re.finditer(r"%([A-Z][A-Z0-9_]*) = getelementptr (?:inbounds )?%struct\.ble4_hw, ptr %[\w.]+, "
                         r"i32 0, i32 0, i32 (\d+)\n", src + "\n"):
        res[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"%([A-Z][A-Z0-9_]*) = getelementptr (?:inbounds )?%struct\.ble_param, ptr %[\w.]+, "
                         r"i32 0, i32 (\d+)\n", src + "\n"):
        res.setdefault(m.group(1), int(m.group(2)))
    return res


def registers(src):
    """{function: sorted absolute addresses >= REGION} for one member."""
    out, fn = {}, None
    for l in src.split("\n"):
        m = re.match(r"define .*@([\w.]+)\(", l)
        if m:
            fn = m.group(1)
        for a in re.findall(r"inttoptr \(i32 (\d+) to ptr\)", l):
            if int(a) >= REGION and fn:
                out.setdefault(fn, set()).add(int(a))
    return {k: sorted(v) for k, v in out.items()}


# ---------------------------------------------------------------- what the default report quotes
REPORT = [
    ("RF", ["bt_rf_init", "bt_process_before_init", "bt_pll_reset", "bt_rf_close", "bt_clk_close", "RF_poweroff_recover"]),
    ("analog", ["bt_rf_analog_init", "update_mdm_txiq_cmp", "bt_reload_trim", "bt_set_single_carrier", "bta_pll_config_init",
                "get_bta_pll_bank", "analog_rx_control"]),
    ("btagc", ["btagc_init"]),
    ("analog_ble", ["ble_txpwr_normal_set", "ble_agc_normal_set"]),
    ("RF_ble", ["le_hw_init", "le_hw_open", "__set_hw_frame_init_4_2", "__set_ble_anchor_con", "__get_ble_anchor_con",
                "__set_anchor_cnt", "__set_anchor_cnt_disable", "__set_interval", "__set_hw_state", "__set_adv_channel_index",
                "__set_fixed_channel_index", "__set_channel_map", "__set_local_addr", "__set_peer_addr",
                "__set_adv_device_filter_param", "ble_hw_inst_set", "ble_hw_tx", "ble_hw_rx", "hw_alloc_tx", "hw_alloc_rx",
                "ble_hw_enable", "ble_hw_disable", "__set_hw_irq", "ble_event_irq_handler", "ble_rx_irq_handler",
                "le_hw_advertising", "__adv_pdu", "__hw_rx_buf_process", "le_hw_close"]),
    ("direct_test_dut", ["dut_le_hw_open", "le_test_frame_init", "le_transmitter_test", "le_receive_test"]),
    ("le_low_power", ["__power_suspend_probe", "__power_resume", "__power_get_timeout_post"]),
    ("cpu_wlc", ["wlc_wreg", "wlc_rreg", "wlc_main"]),
]

# facts docs/BLUETOOTH_RADIO.md quotes that --check re-derives: (member, function, substring of its step view)
GOLDEN = [
    ("RF_ble", "le_hw_init", "st.i32 28000 <- 1"),
    ("RF_ble", "le_hw_init", "st.i32 28034 <- 0xA01"),
    ("RF_ble", "le_hw_init", "st.i32 28008 <- rmw clear 0x3FF or 0x46"),
    ("RF_ble", "le_hw_init", "call request_irq(0x2D, 2, @ble_event_irq_handler, 0)"),
    ("RF_ble", "le_hw_init", "call request_irq(0x1D, 2, @ble_rx_irq_handler, 0)"),
    ("RF_ble", "__set_ble_anchor_con", "st.i32 28020 <- %value"),
    ("RF_ble", "__set_ble_anchor_con", "st.i32 2801C <- (or (or (shl %index 0xA) (shl HW_ID() 4)) 5)"),
    ("RF_ble", "__get_ble_anchor_con", "st.i32 2801C <- (or (or (shl %index 0xA) (shl HW_ID() 4)) 2)"),
    ("RF_ble", "__set_hw_frame_init_4_2", "st.i16 rd[%ble_hw]+0xA <- 0xFFFFBED6"),
    ("RF_ble", "__set_hw_frame_init_4_2", "st.i16 rd[%ble_hw]+0x44 <- 0x5555"),
    ("RF_ble", "__set_hw_frame_init_4_2", "st.i16 rd[%ble_hw]+0x46 <- 0x55"),
    ("RF_ble", "ble_hw_inst_set", "(or PHY_TO_BLE() 0xFFFF8000)"),
    ("RF", "bt_rf_init", "st.i32 2FC78 <- 0xF"),
    ("RF", "bt_rf_init", "st.i32 20000 <- rmw or 0xC0000000"),
    ("RF", "bt_process_before_init", "st.i32 20050 <- 0x87"),
    ("RF", "bt_process_before_init", "st.i32 20050 <- 0xAC"),
    ("analog", "bt_rf_analog_init", "st.i32 2FC00 <- 0xFFD144"),
    ("analog", "bt_rf_analog_init", "st.i32 2FC04 <- 0x4E143CDF"),
    ("analog", "bt_rf_analog_init", "st.i32 2FC88 <- &table"),
    ("btagc", "btagc_init", "st.i32 2FD84 <- 0x1872BF14"),
    ("cpu_wlc", "wlc_wreg", "st.i32 14004 <- (or (or (shl %type 4) (shl %wdat 0x10)) 1)"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--layout", action="store_true")
    ap.add_argument("--registers", action="store_true")
    ap.add_argument("--steps", nargs="+", metavar=("MEMBER", "FN"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    for arc, want in ARCHIVES.items():
        have = sha256(LIBA / f"{arc}.a")
        if have != want:
            print(f"ble_ir: {arc}.a is {have}, the doc was written from {want}", file=sys.stderr)
            if a.check:
                return 1
    with tempfile.TemporaryDirectory() as tmp:
        irs = load_irs(tmp)
        by = {mem if arc == "btctrler" else f"{arc}_{mem}": irs[(arc, mem)] for arc, mem in MEMBERS}
        types = {k: Types(v) for k, v in by.items()}

        def run(member, fn):
            return steps(by[member], types[member], fn, quiet_branches=True)

        if a.check:
            bad = 0
            for member, fn, want in GOLDEN:
                if want not in "\n".join(run(member, fn)):
                    print(f"FAIL {member}.{fn}: no {want!r}")
                    bad += 1
            lay = names_of(by["RF_ble"])
            for name, idx in (("ANCHOR", 0), ("ADVIDX", 2), ("FORMAT", 3), ("OPTCNTL", 4), ("BDADDR0", 5), ("TXPTR0", 9),
                              ("RXPTR0", 15), ("WINCNTL0", 17), ("CHMAP0", 23), ("CRCWORD0", 34), ("WIDEN0", 36),
                              ("LOCALADRL", 41), ("INTFRAME", 49), ("PLL_COMP", 72)):
                if lay.get(name) != idx:
                    print(f"FAIL layout: {name} is field {lay.get(name)}, the doc says {idx}")
                    bad += 1
            print("ble_ir --check:", "FAILED %d" % bad if bad else "%d facts reproduce" % (len(GOLDEN) + 14))
            return 1 if bad else 0
        if a.layout:
            t = types["RF_ble"]
            names = {}
            for n, i in names_of("\n".join(by.values())).items():
                names.setdefault(i, []).append(n)
            print("# struct ble4_hw begins with struct ble_param; offsets in bytes; names are the IR's value names")
            for i, (off, f) in enumerate(t.field_offsets("%struct.ble_param")):
                nm = sorted(set(names.get(i, [])), key=len)      # LLVM numbers repeated names (TXPTR0, TXPTR010): keep the first
                print("field %2d  +0x%03X  %-12s %s" % (i, off, f, nm[0] if nm else ""))
            print("# sizeof(ble_param) = %d; sizeof(ble4_hw) = %d" % (t.size("%struct.ble_param")[0], t.size("%struct.ble4_hw")[0]))
            return 0
        if a.registers:
            for mem, src in by.items():
                print("=====", mem)
                for fn, addrs in registers(src).items():
                    print("  %-34s %s" % (fn, " ".join("%05X" % x for x in addrs)))
            return 0
        if a.steps:
            for line in sum((run(a.steps[0], fn) for fn in a.steps[1:]), []):
                print(line)
            return 0
        for member, fns in REPORT:
            print("######", member)
            for fn in fns:
                for line in run(member, fn):
                    print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
