#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Facts for the FM-1 WiFi radio's power-up, read out of JieLi's wl_rf_common.a (docs/WIFI.md stages 1 and 3).

The archive's members are LLVM bitcode. This tool turns the eight members docs/WIFI.md clears as a source of facts
(wf_cfr_init, wf_pa_dpd, wf_phy_mac_init, wf_phy_mac_lib, wf_rf_cal, wf_rf_init, wf_soft_adj, wl_rfd_init) back into
LLVM IR and runs that IR in a small interpreter against a register model, so the register writes it makes come out
as data: which register, which value, in which order, which delays. It writes:

  default   firmware/wifi/fm1wifi_tables.h: the straight-line parts of the power-up as tables of register steps, and
            the constant arrays the rest uses (docs/WIFI_STAGE3.md says which is which); plus the receive thresholds
            and the RF RAM table from the SDK's own C source apps/common/net/wifi_conf.c (Apache-2.0).
  --trace   the whole stage 3 sequence's writes against the register model, as the fixture format of
            tests/wifi/sdio_model.h (window level: CPU and MAC register writes, BBP writes, BBP window writes,
            delays; reads left out), gzip-compressed: tests/wifi/radio_trace.txt.gz. tests/wifi_model.c runs the
            fm1wifi library against the same model (tests/wifi/sdio_model.c) and diffs the two.
  --check   regenerates both in memory and fails if the committed files differ.

What it emits is numbers with a provenance comment (the archive's SHA-256, the member, the function); no control
flow, no IR text, no identifiers other than register names. It reads nothing from the archive's other five members
(wl_hw_init, wl_usb_to_sdio, wl_sdio, wl_sdio_host, wf_deep_sleep), from wl_wifi*.a or from the Bluetooth archives
(docs/WIFI.md "What stages 1 and 3 may take from wl_rf_common.a"). Where the IR calls into those (the core's register
window, wl30_mmc_io_rw_extended), the interpreter does what docs/WIFI.md's stage 2 facts say the call does.

Needs the JieLi AC79 SDK ($AC79_SDK, default ~/fw-AC79_AIoT_SDK) and either the JieLi toolchain's clang 4.0.1
($JIELI_TOOLCHAIN, default ~/.jieli/toolchain; it wrote the bitcode) or a system llvm-dis.
"""
import argparse
import gzip
import hashlib
import io
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLES = ROOT / "firmware/wifi/fm1wifi_tables.h"
TRACE = ROOT / "tests/wifi/radio_trace.txt.gz"
ARCHIVE = "cpu/wl82/liba/wl_rf_common.a"
ARCHIVE_SHA256 = "ec50bb31af02ee3e2db9d87726c1b18b317f2acd72f233c9ce7b79a89ea324b2"
WIFI_CONF = "apps/common/net/wifi_conf.c"
MEMBERS = ("wf_cfr_init", "wf_pa_dpd", "wf_phy_mac_init", "wf_phy_mac_lib", "wf_rf_cal", "wf_rf_init", "wf_soft_adj",
           "wl_rfd_init")
OSC_HZ = 24000000          # clk_get("osc") on the FM-1 (docs/WIFI.md "Where the tuning values come from")

# The calibration the trace runs with: fm1-base's / the SDK board's defaults (docs/WIFI.md), as fm1wifi_cal_default.
CAL_XOSC = (0x0B, 0x0B)
CAL_PA = (1, 7, 4, 7, 11, 1, 7)
CAL_MCS = (0x32, 0x32, 0x32, 0x32, 0x48, 0x48, 0x55, 0x50, 0x40, 0x40, 0x3E, 0x34, 0x48, 0x5A, 0x50, 0x40, 0x40, 0x40,
           0x32, 0x2B)

M32 = 0xFFFFFFFF


def tdiv(a, b):                       # C's division: towards zero
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def sext(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


# ======================================================================================================== the model
# The register model, the same as tests/wifi/sdio_model.c's (its header describes it; keep the two in step). It is a
# register file of the WiFi blocks with a baseband behind BBP_CSR_CFG, the four BBP windows on top of BBP registers,
# and made-up answers for what the power-up reads back, chosen so every search lands somewhere particular: none of
# them is measured.

WLA = lambda n: 0x11900 + 4 * n
CORE = 0x30000
BBP_CSR = CORE + 0x101C
WIN_STROBE = {0xC8: 0, 0xCC: 1, 0xD0: 2, 0xD4: 3}     # cca, rx11n, tx_backend, rx_frontend
WIN_ADDR = {0: 0xC9, 1: 0xCD, 2: 0xD1, 3: 0xD5}
WIN_DATA = {0: 0xCA, 1: 0xCE, 2: 0xD2, 3: 0xD6}
WIN_READ = {0: 0xCB, 1: 0xCF, 2: 0xD3, 3: 0xD7}
WIN_NAME = ("cca", "rx11n", "txbe", "rxfe")


class Model:
    def __init__(self):
        self.reg = {}
        self.bbp = [0] * 256
        self.win = [[0] * 256 for _ in range(4)]
        self.log = []

    # -- what the model answers
    def wla(self, n):
        return self.reg.get(WLA(n), 0)

    def status(self):                  # WLA_CON30, read after WLA_CON26 bits 31-28 select what (wlrf_get_)
        sel, w26 = self.wla(26) >> 28, self.wla(26)
        if sel != 1:
            return 0
        v = (w26 & 1) << 5                                         # bit 5: the filter tune is done once started
        v |= ((((w26 >> 1) & 0x1FF) * 5 // 9) & 0xFF) << 8         # bits 15-8: its code
        cap, fbds = (self.wla(14) >> 19) & 0x7F, (self.wla(15) >> 5) & 0xFF
        lo = 250 - 2 * cap                                         # the VCO: below, inside or above its range
        res = 2 if fbds < lo else 0 if fbds < lo + 5 else 1
        return v | (res & 1) << 18 | (res >> 1) << 17

    def measure(self):                 # what the receiver's measurement registers show: (I, Q) in 24-bit units
        tx_cal, rx_cal = self.wla(11) >> 14 & 1, self.wla(11) >> 11 & 1
        t = self.win[2]
        r = self.win[3]
        if tx_cal:
            lo_i, lo_q = (self.wla(7) >> 8) & 0x7F, self.wla(8) & 0x7F
            di = sext(t[8] << 2 | t[98] & 3, 10)
            dq = sext(t[9] << 2 | t[98] >> 2 & 3, 10)
            gi = t[4] << 2 | t[97] & 3
            gq = t[6] << 2 | t[97] >> 4 & 3
            pi = sext(t[5] << 2 | t[97] >> 2 & 3, 10)
            pq = sext(t[7] << 2 | t[97] >> 6 & 3, 10)
            i = 40 * (lo_i - 41) + 24 * (di - 23) + 2 * (gi - gq - 37)
            q = 40 * (lo_q - 52) + 24 * (dq + 17) + 3 * (pq - pi + 9)
            return 256 * i, 256 * q
        if rx_cal:
            i = 5 * (r[9] - r[11] + 20)
            q = 4 * (sext(r[10], 8) - sext(r[12], 8) - 7)
            return 256 * i, 256 * q
        w2 = self.wla(2)
        di = (w2 >> 7) & 0x1FF if w2 >> 16 & 1 else -((w2 >> 7) & 0x1FF)
        dq = (w2 >> 17) & 0x1FF if w2 >> 26 & 1 else -((w2 >> 17) & 0x1FF)
        rng, g = (self.wla(4) >> 8) & 0xF, (self.wla(3) >> 6) & 3
        return 600 * (di - (123 - 9 * rng - 20 * g)), 600 * (dq - (-77 + 5 * rng + 11 * g))

    def adc_peak(self):
        w4, w5 = self.wla(4), self.wla(5)
        g = (w5 >> 15 & 7) + 8 * (w5 >> 5 & 7) + 2 * (w5 >> 12 & 7) + (w4 >> 22 & 7) + 3 * (w4 >> 25 & 3)
        ch = self.reg.get(0x1405C, 0) & 0xFF
        return min(255, 6 + 3 * g + (ch * 7) % 11)

    def win_read(self, w, a):
        if w == 3:
            if a == 0x8D:
                return 0                                           # the measurement is ready
            if a == 0x80:
                return self.adc_peak()
            if 0x81 <= a <= 0x8C:
                i, q = self.measure()
                k = (a - 0x81) // 3
                v = (i, q, 0, q)[k] & 0xFFFFFF
                return v >> (8 * (2 - (a - 0x81) % 3)) & 0xFF
        return self.win[w][a]

    # -- the port
    def rd32(self, a):
        if a == WLA(30):
            return self.status()
        if a == 0x14028:                                           # WL_ANL_CON0: bits 22-20 (busy) always clear
            return self.reg.get(a, 0) & ~0x700000 & M32
        return self.reg.get(a, 0)

    def wr32(self, a, v):
        v &= M32
        if a == BBP_CSR and v & 0x20000:
            r = (v >> 8) & 0xFF
            if v & 0x10000:
                v = (v & ~0xFF) | self.bbp[r]
            else:
                self.bbp_write(r, v & 0xFF)
            v &= ~0x20000
        elif a != BBP_CSR:
            self.log.append(("wr32", a, v))
        self.reg[a] = v

    def bbp_write(self, r, v):
        self.bbp[r] = v
        if r in WIN_STROBE:
            w = WIN_STROBE[r]
            a = self.bbp[WIN_ADDR[w]]
            if v == 1:
                self.win[w][a] = self.bbp[WIN_DATA[w]]
                self.log.append(("win", w, a, self.win[w][a]))
            elif v == 2:
                self.bbp[WIN_READ[w]] = self.win_read(w, a)
            else:
                self.log.append(("bbp", r, v))
        elif r not in WIN_ADDR.values() and r not in WIN_DATA.values():
            self.log.append(("bbp", r, v))

    def delay(self, us):
        self.log.append(("delay", us))


def rec_text(r):
    if r[0] == "wr32":
        return "wr32 0x%05x 0x%08x" % (r[1], r[2])
    if r[0] == "bbp":
        return "bbp 0x%02x 0x%02x" % (r[1], r[2])
    if r[0] == "win":
        return "win %s 0x%02x 0x%02x" % (WIN_NAME[r[1]], r[2], r[3])
    return "delay %d" % r[1]


# ================================================================================================= the IR reader

TOK = re.compile(r'\s*(c?"(?:[^"\\]|\\.)*"|[%@][-\w.$]+|0x[0-9A-Fa-f]+|-?\d+\.\d+e[-+]\d+|-?\d+|\.\.\.|[\w.]+|[\[\]{}(),=*<>!#])')


def tokens(s):
    out, i = [], 0
    s = s.rstrip()
    while i < len(s):
        m = TOK.match(s, i)
        if not m:
            if s[i:].strip() == "":
                break
            raise ValueError("cannot read IR at: " + s[i:i + 40])
        out.append(m.group(1))
        i = m.end()
    return out


class P:
    """A cursor over one line's tokens."""

    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else None

    def next(self):
        self.i += 1
        return self.t[self.i - 1]

    def want(self, x):
        got = self.next()
        if got != x:
            raise ValueError("IR: %r expected, %r found in %s" % (x, got, " ".join(self.t)))

    def opt(self, *xs):
        if self.peek() in xs:
            return self.next()
        return None


ATTRS = {"zeroext", "signext", "nonnull", "noundef", "inreg", "nocapture", "readonly", "writeonly", "returned",
         "noalias", "immarg", "dereferenceable", "align", "local_unnamed_addr", "unnamed_addr", "tail", "notail",
         "musttail", "fastcc", "ccc", "nuw", "nsw", "exact", "inbounds", "volatile", "internal", "private",
         "weak", "dso_local", "hidden", "nnan", "ninf", "nsz", "arcp", "contract", "afn", "reassoc", "fast",
         "disjoint", "nneg", "samesign", "range", "initializes", "nofree", "partial"}


def parse_type(p):
    t = p.next()
    if t == "[":
        n = int(p.next())
        p.want("x")
        e = parse_type(p)
        p.want("]")
        ty = ("arr", n, e)
    elif t == "{":
        fs = []
        while p.peek() != "}":
            fs.append(parse_type(p))
            p.opt(",")
        p.next()
        ty = ("struct", fs)
    elif t == "<":
        raise ValueError("IR: vector types are not handled")
    elif t.startswith("%"):
        ty = ("named", t)
    elif t == "ptr":
        ty = ("ptr",)
    elif t == "void":
        ty = ("void",)
    elif t in ("double", "float"):
        ty = ("double",)
    elif re.fullmatch(r"i\d+", t):
        ty = ("i", int(t[1:]))
    elif t in ("label", "metadata"):
        ty = (t,)
    else:
        raise ValueError("IR: unknown type " + t)
    while True:
        if p.peek() == "*":
            p.next()
            ty = ("ptr",)
        elif p.peek() == "(" and ty[0] != "label":        # a function type: ret (args)
            depth = 0
            while True:
                x = p.next()
                depth += x == "("
                depth -= x == ")"
                if depth == 0:
                    break
            ty = ("fn",)
        else:
            return ty


def skip_attrs(p):
    while True:
        x = p.peek()
        if x in ATTRS:
            p.next()
            if x in ("align", "dereferenceable") and p.peek() and re.fullmatch(r"\d+", p.peek()):
                p.next()
            elif x == "range" and p.peek() == "(":
                while p.next() != ")":
                    pass
        else:
            return


def parse_value(p, ty=None):
    """A value: ('r', name) | ('g', name) | ('c', number) | ('ce', op, ...) | ('agg', [values])."""
    t = p.next()
    if t.startswith("%"):
        return ("r", t)
    if t.startswith("@"):
        return ("g", t)
    if re.fullmatch(r"-?\d+", t):
        return ("c", int(t))
    if re.fullmatch(r"-?\d+\.\d+e[-+]\d+", t):
        return ("c", float(t))
    if t.startswith("0x") and ty == ("double",):
        return ("c", struct.unpack("<d", struct.pack("<Q", int(t, 16)))[0])
    if t.startswith("0x"):
        return ("c", int(t, 16))
    if t in ("true", "false"):
        return ("c", int(t == "true"))
    if t in ("null", "undef", "poison", "zeroinitializer"):
        return ("c", 0)
    if t in ("inttoptr", "ptrtoint", "bitcast", "zext", "sext", "trunc"):
        p.want("(")
        sty = parse_type(p)
        v = parse_value(p, sty)
        p.want("to")
        parse_type(p)
        p.want(")")
        return v
    if t == "getelementptr":
        skip_attrs(p)
        p.want("(")
        ety = parse_type(p)
        p.want(",")
        parse_type(p)
        base = parse_value(p)
        idx = []
        while p.opt(","):
            skip_attrs(p)
            parse_type(p)
            idx.append(parse_value(p))
        p.want(")")
        return ("gep", ety, base, idx)
    if t.startswith('c"'):
        return ("str", t)
    if t == "[" or t == "{":
        close = "]" if t == "[" else "}"
        items = []
        while p.peek() != close:
            ity = parse_type(p)
            items.append(parse_value(p, ity))
            p.opt(",")
        p.next()
        return ("agg", items)
    raise ValueError("IR: unknown value " + t)


def cstr(t):
    body, out, i = t[2:-1], [], 0
    while i < len(body):
        if body[i] == "\\":
            if body[i + 1] == "\\":
                out.append(92)
                i += 2
            else:
                out.append(int(body[i + 1:i + 3], 16))
                i += 3
        else:
            out.append(ord(body[i]))
            i += 1
    return bytes(out)


META = re.compile(r",\s*!(?:[\w.]+)\s+!\d+|,\s*!(?:[\w.]+)\s+!\{[^}]*\}|\s+#\d+\s*$|\s+#\d+(?=\s*,)")


class Module:
    def __init__(self):
        self.types, self.globals, self.funcs = {}, {}, {}

    def size(self, ty):
        k = ty[0]
        if k == "i":
            return 1 if ty[1] <= 8 else 2 if ty[1] <= 16 else 4 if ty[1] <= 32 else 8
        if k in ("ptr", "fn"):
            return 4
        if k == "double":
            return 8
        if k == "arr":
            return ty[1] * self.size(ty[2])
        if k == "named":
            return self.size(self.types[ty[1]])
        if k == "struct":
            return self.layout(ty)[1]
        raise ValueError("size of " + repr(ty))

    def align(self, ty):
        k = ty[0]
        if k == "arr":
            return self.align(ty[2])
        if k == "named":
            return self.align(self.types[ty[1]])
        if k == "struct":
            return max([self.align(f) for f in ty[1]] or [1])
        return min(4, self.size(ty))                 # datalayout: i64 and double aligned to 32 bits

    def layout(self, ty):
        offs, o = [], 0
        for f in ty[1]:
            a = self.align(f)
            o = (o + a - 1) // a * a
            offs.append(o)
            o += self.size(f)
        a = self.align(ty)
        return offs, (o + a - 1) // a * a

    def read(self, path, member):
        lines = Path(path).read_text().splitlines()
        i = 0
        while i < len(lines):
            ln = lines[i]
            if ln.startswith("%") and " = type " in ln:
                name, rest = ln.split(" = type ", 1)
                if rest.strip() != "opaque":
                    self.types[name.strip()] = parse_type(P(tokens(META.sub("", rest))))
            elif ln.startswith("@") and " = " in ln:
                self.read_global(META.sub("", ln), member)
            elif ln.startswith("define "):
                body = []
                i += 1
                while lines[i] != "}":
                    body.append(lines[i])
                    i += 1
                self.read_func(ln, body, member)
            i += 1

    def read_global(self, ln, member):
        name, rest = ln.split(" = ", 1)
        p = P(tokens(rest))
        ext = False
        while p.peek() in ATTRS or p.peek() in ("external", "global", "constant", "common", "linkonce_odr",
                                                 "available_externally", "thread_local"):
            x = p.next()
            ext |= x == "external"
            if x in ("global", "constant"):
                break
        ty = parse_type(p)
        init = None if ext else parse_value(p, ty)
        if init is None and name in self.globals:
            return
        self.globals[name] = {"ty": ty, "init": init, "member": member}

    def read_func(self, head, body, member):
        h = META.sub("", head)
        m = re.match(r"define\s+(.*?)@([-\w.$]+)\((.*)\)(.*)\{\s*$", h)
        name, params = "@" + m.group(2), []
        depth, cur = 0, ""
        for ch in m.group(3) + ",":
            if ch == "," and depth == 0:
                if cur.strip():
                    params.append(cur.strip().split()[-1])
                cur = ""
                continue
            depth += ch in "([{"
            depth -= ch in ")]}"
            cur += ch
        blocks, order, label, pending = {}, [], "entry", None
        for ln in body:
            s_ = ln.split(";", 1)[0] if '"' not in ln else ln
            if not s_.strip() or s_.strip().startswith("#dbg_"):
                continue
            if pending is not None:                     # a switch's case list, over several lines
                pending += " " + META.sub("", s_.strip())
                if "]" in s_:
                    blocks.setdefault(label, []).append(pending)
                    pending = None
                continue
            if re.match(r"^[-\w.$]+:", s_):
                label = s_.split(":")[0]
                continue
            st = META.sub("", s_.strip())
            if label not in order:
                order.append(label)
            if st.split(" = ", 1)[-1].startswith("switch") and "]" not in st:
                pending = st
                continue
            blocks.setdefault(label, []).append(st)
        self.funcs[name] = {"params": params, "raw": blocks, "order": order, "member": member}


# ============================================================================================= the interpreter

class Ret(Exception):
    pass


MMIO_TOP = 0x100000


class Interp:
    """Runs the IR. Memory below MMIO_TOP is the hardware (the model); globals and stack live above it."""

    def __init__(self, mod, model, externals):
        self.m, self.model, self.ext = mod, model, externals
        self.mem = bytearray(0x40000)
        self.base = 0x1000000
        self.addr = {}
        self.brk = 0x100
        self.sp = 0x20000
        self.compiled = {}
        self.hooks = {}                  # function name -> callback(args) run before the body (recording)
        self.shape = None                # a shape tracker for the table generator, or None
        for g, d in mod.globals.items():
            sz = max(4, mod.size(d["ty"]))
            self.brk = (self.brk + 7) & ~7
            self.addr[g] = self.base + self.brk
            self.brk += sz
        for g, d in mod.globals.items():
            if d["init"] is not None:
                self.store_init(self.addr[g], d["ty"], d["init"])

    # -- memory
    def load(self, ty, a):
        if a < MMIO_TOP:
            assert ty == ("i", 32), "MMIO access of %r at %#x" % (ty, a)
            return self.model.rd32(a)
        o = a - self.base
        sz = self.m.size(ty)
        if ty[0] == "double":
            return struct.unpack_from("<d", self.mem, o)[0]
        v = int.from_bytes(self.mem[o:o + sz], "little")
        return v & ((1 << ty[1]) - 1) if ty[0] == "i" else v

    def store(self, ty, a, v):
        if a < MMIO_TOP:
            assert ty == ("i", 32), "MMIO access of %r at %#x" % (ty, a)
            self.model.wr32(a, v)
            return
        o = a - self.base
        if ty[0] == "double":
            struct.pack_into("<d", self.mem, o, v)
            return
        sz = self.m.size(ty)
        self.mem[o:o + sz] = (int(v) & ((1 << (8 * sz)) - 1)).to_bytes(sz, "little")

    def store_init(self, a, ty, init):
        k = init[0]
        if k == "c":
            if init[1] or ty[0] != "arr" and ty[0] != "struct" and ty[0] != "named":
                self.store(ty, a, init[1])
        elif k == "str":
            b = cstr(init[1])
            self.mem[a - self.base:a - self.base + len(b)] = b
        elif k == "agg":
            if ty[0] == "arr":
                es = self.m.size(ty[2])
                for n, it in enumerate(init[1]):
                    self.store_init(a + n * es, ty[2], it)
            else:
                st = self.m.types[ty[1]] if ty[0] == "named" else ty
                offs, _ = self.m.layout(st)
                for n, it in enumerate(init[1]):
                    self.store_init(a + offs[n], st[1][n], it)
        elif k in ("g", "gep"):
            self.store(("i", 32), a, self.const(init))

    def const(self, v):
        k = v[0]
        if k == "c":
            return v[1]
        if k == "g":
            if v[1] in self.addr:
                return self.addr[v[1]]
            return ("fn", v[1])
        if k == "gep":
            return self.gep(v[1], self.const(v[2]), [self.const(x) for x in v[3]])
        raise ValueError("not a constant: " + repr(v))

    def gep(self, ety, base, idx):
        a = base + idx[0] * self.m.size(ety)
        ty = ety
        for x in idx[1:]:
            if ty[0] == "named":
                ty = self.m.types[ty[1]]
            if ty[0] == "arr":
                ty = ty[2]
                a += x * self.m.size(ty)
            elif ty[0] == "struct":
                a += self.m.layout(ty)[0][x]
                ty = ty[1][x]
            else:
                raise ValueError("gep into " + repr(ty))
        return a & M32

    def alloca(self, ty):
        sz = (self.m.size(ty) + 7) & ~7
        self.sp += sz
        return self.base + self.sp - sz

    # -- compiling instructions into closures
    def compile(self, fname):
        f = self.m.funcs[fname]
        blocks = {}
        for lab, ins in f["raw"].items():
            blocks[lab] = [self.compile_ins(s) for s in ins]
        self.compiled[fname] = (f["params"], blocks, f["order"][0] if f["order"] else "entry")
        return self.compiled[fname]

    def val(self, v):
        if v[0] == "r":
            name = v[1]
            return lambda fr: fr[name]
        c = self.const(v)
        return lambda fr: c

    def compile_ins(self, s):
        dst = None
        if "@llvm.dbg." in s or "@llvm.lifetime." in s:
            return lambda fr: None
        if s.startswith("%") and " = " in s:
            dst, s = s.split(" = ", 1)
        p = P(tokens(s))
        op = p.next()
        if op in ("tail", "notail", "musttail"):
            op = p.next()
        V = self.val
        sh = self.shape
        if op in ("add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr", "sdiv", "udiv", "srem", "urem"):
            skip_attrs(p)
            ty = parse_type(p)
            a, _, b = parse_value(p), p.want(","), parse_value(p)
            bits = ty[1]
            mask = (1 << bits) - 1
            fa, fb = V(a), V(b)
            fn = {
                "add": lambda x, y: x + y, "sub": lambda x, y: x - y, "mul": lambda x, y: x * y,
                "and": lambda x, y: x & y, "or": lambda x, y: x | y, "xor": lambda x, y: x ^ y,
                "shl": lambda x, y: x << y, "lshr": lambda x, y: x >> y,
                "ashr": lambda x, y: sext(x, bits) >> y,
                "sdiv": lambda x, y: tdiv(sext(x, bits), sext(y, bits)),
                "udiv": lambda x, y: x // y,
                "srem": lambda x, y: sext(x, bits) - tdiv(sext(x, bits), sext(y, bits)) * sext(y, bits),
                "urem": lambda x, y: x % y}[op]
            names = (a[1] if a[0] == "r" else None, b[1] if b[0] == "r" else None)
            consts = (None if a[0] == "r" else self.const(a), None if b[0] == "r" else self.const(b))

            def ex(fr):
                x, y = fa(fr) & mask, fb(fr) & mask
                fr[dst] = fn(x, y) & mask
                if sh:
                    sh.binop(fr, dst, op, names, consts, bits)
            return ex
        if op in ("fadd", "fsub", "fmul", "fdiv"):
            skip_attrs(p)
            parse_type(p)
            a = parse_value(p, ("double",))
            p.want(",")
            b = parse_value(p, ("double",))
            fa, fb = V(a), V(b)
            fn = {"fadd": lambda x, y: x + y, "fsub": lambda x, y: x - y, "fmul": lambda x, y: x * y,
                  "fdiv": lambda x, y: x / y}[op]

            names = [v[1] for v in (a, b) if v[0] == "r"]

            def ex(fr):
                fr[dst] = fn(float(fa(fr)), float(fb(fr)))
                if sh:
                    sh.generic(fr, dst, names)
            return ex
        if op in ("zext", "sext", "trunc", "bitcast", "inttoptr", "ptrtoint", "uitofp", "sitofp", "fptosi", "fptoui"):
            skip_attrs(p)
            sty = parse_type(p)
            a = parse_value(p, sty)
            p.want("to")
            dty = parse_type(p)
            fa = V(a)
            sb = sty[1] if sty[0] == "i" else 32
            db = dty[1] if dty[0] == "i" else 32
            name = a[1] if a[0] == "r" else None

            def ex(fr):
                x = fa(fr)
                if op == "sext":
                    r = sext(x, sb) & ((1 << db) - 1)
                elif op == "trunc":
                    r = x & ((1 << db) - 1)
                elif op == "zext":
                    r = x & ((1 << sb) - 1)
                elif op == "uitofp":
                    r = float(x & ((1 << sb) - 1))
                elif op == "sitofp":
                    r = float(sext(x, sb))
                elif op in ("fptosi", "fptoui"):
                    r = int(x) & ((1 << db) - 1)
                else:
                    r = x
                fr[dst] = r
                if sh:
                    sh.cast(fr, dst, op, name, sb, db)
            return ex
        if op == "icmp":
            cond = p.next()
            ty = parse_type(p)
            a, _, b = parse_value(p), p.want(","), parse_value(p)
            bits = ty[1] if ty[0] == "i" else 32
            fa, fb = V(a), V(b)
            sg = cond in ("slt", "sle", "sgt", "sge")
            fn = {"eq": lambda x, y: x == y, "ne": lambda x, y: x != y, "ult": lambda x, y: x < y,
                  "ule": lambda x, y: x <= y, "ugt": lambda x, y: x > y, "uge": lambda x, y: x >= y,
                  "slt": lambda x, y: x < y, "sle": lambda x, y: x <= y, "sgt": lambda x, y: x > y,
                  "sge": lambda x, y: x >= y}[cond]
            mask = (1 << bits) - 1

            names = [v[1] for v in (a, b) if v[0] == "r"]

            def ex(fr):
                x, y = fa(fr) & mask, fb(fr) & mask
                if sg:
                    x, y = sext(x, bits), sext(y, bits)
                fr[dst] = int(fn(x, y))
                if sh:
                    sh.generic(fr, dst, names)
            return ex
        if op == "select":
            parse_type(p)
            c = parse_value(p)
            p.want(",")
            ty = parse_type(p)
            a = parse_value(p, ty)
            p.want(",")
            parse_type(p)
            b = parse_value(p, ty)
            fc, fa, fb = V(c), V(a), V(b)
            names = (a[1] if a[0] == "r" else None, b[1] if b[0] == "r" else None)

            cname = c[1] if c[0] == "r" else None

            def ex(fr):
                pick = fc(fr) & 1
                fr[dst] = fa(fr) if pick else fb(fr)
                if sh:
                    if sh.get(fr, cname):
                        sh.s[-1][dst] = ("dyn",)
                    else:
                        sh.copy(fr, dst, names[0] if pick else names[1])
            return ex
        if op == "load":
            skip_attrs(p)
            ty = parse_type(p)
            p.want(",")
            parse_type(p)
            a = parse_value(p)
            fa = V(a)

            def ex(fr):
                ad = fa(fr)
                fr[dst] = self.load(ty, ad)
                if sh:
                    sh.load(fr, dst, ad, ty)
            return ex
        if op == "store":
            skip_attrs(p)
            ty = parse_type(p)
            v = parse_value(p, ty)
            p.want(",")
            parse_type(p)
            a = parse_value(p)
            fv, fa = V(v), V(a)
            vname = v[1] if v[0] == "r" else None

            def ex(fr):
                ad, x = fa(fr), fv(fr)
                if sh:
                    sh.store(fr, ad, x, vname, ty)
                self.store(ty, ad, x)
            return ex
        if op == "alloca":
            ty = parse_type(p)

            def ex(fr):
                fr[dst] = self.alloca(ty)
            return ex
        if op == "getelementptr":
            skip_attrs(p)
            ety = parse_type(p)
            p.want(",")
            parse_type(p)
            base = parse_value(p)
            idx = []
            while p.opt(","):
                skip_attrs(p)
                parse_type(p)
                idx.append(parse_value(p))
            fb = V(base)
            fi = [V(x) for x in idx]
            ibits = 32

            names = [v[1] for v in [base] + idx if v[0] == "r"]

            def ex(fr):
                fr[dst] = self.gep(ety, fb(fr), [sext(f(fr), ibits) for f in fi])
                if sh:
                    sh.generic(fr, dst, names)
            return ex
        if op == "phi":
            ty = parse_type(p)
            inc = []
            while p.peek() == "[":
                p.next()
                v = parse_value(p, ty)
                p.want(",")
                lab = p.next()[1:]
                p.want("]")
                p.opt(",")
                inc.append((lab, V(v), v[1] if v[0] == "r" else None))
            table = {lab: (f, n) for lab, f, n in inc}

            def ex(fr):
                f, n = table[fr["$prev"]]
                fr.setdefault("$phi", []).append((dst, f(fr), n))
            ex.phi = True
            return ex
        if op == "br":
            if p.peek() == "label":
                p.next()
                lab = p.next()[1:]
                return lambda fr: lab
            parse_type(p)
            c = parse_value(p)
            p.want(",")
            p.want("label")
            t = p.next()[1:]
            p.want(",")
            p.want("label")
            e = p.next()[1:]
            fc = V(c)
            return lambda fr: t if fc(fr) & 1 else e
        if op == "switch":
            ty = parse_type(p)
            v = parse_value(p)
            p.want(",")
            p.want("label")
            d = p.next()[1:]
            p.want("[")
            cases = {}
            mask = (1 << ty[1]) - 1
            while p.peek() != "]":
                parse_type(p)
                c = parse_value(p)
                p.want(",")
                p.want("label")
                cases[c[1] & mask] = p.next()[1:]
            fv = V(v)
            return lambda fr: cases.get(fv(fr) & mask, d)
        if op == "ret":
            if p.peek() == "void":
                def ex(fr):
                    raise Ret(None)
                return ex
            ty = parse_type(p)
            v = parse_value(p, ty)
            fv = V(v)
            vname = v[1] if v[0] == "r" else None

            def ex(fr):
                if sh:
                    sh.ret(fr, vname)
                raise Ret(fv(fr))
            return ex
        if op == "unreachable":
            def ex(fr):
                raise RuntimeError("unreachable reached")
            return ex
        if op == "call":
            skip_attrs(p)
            rty = parse_type(p)
            if p.peek() == "(":                       # an explicit function type: (args...)
                depth = 0
                while True:
                    x = p.next()
                    depth += x == "("
                    depth -= x == ")"
                    if depth == 0:
                        break
                p.opt("*")
            callee = parse_value(p)
            while callee[0] not in ("g",):
                raise ValueError("indirect call: " + s)
            name = callee[1]
            p.want("(")
            args = []
            while p.peek() != ")":
                aty = parse_type(p)
                skip_attrs(p)
                if aty == ("metadata",):
                    while p.peek() not in (",", ")"):
                        p.next()
                    args.append(("c", 0))
                else:
                    args.append(parse_value(p, aty))
                p.opt(",")
            p.next()
            if name.startswith("@llvm.dbg") or name.startswith("@llvm.lifetime"):
                return lambda fr: None
            fs = [V(a) for a in args]
            anames = [a[1] if a[0] == "r" else None for a in args]

            def ex(fr):
                vals = [f(fr) for f in fs]
                r = self.call(name, vals, anames, fr)
                if dst:
                    fr[dst] = r
                    if sh:
                        sh.callret(fr, dst)
            return ex
        raise ValueError("IR: instruction not handled: " + s)

    # -- calls
    def call(self, name, args, anames=None, caller=None):
        if name.startswith("@llvm.memcpy"):
            d, s_, n = args[0], args[1], args[2]
            for k in range(n):
                self.store(("i", 8), d + k, self.load(("i", 8), s_ + k))
            return None
        hook = self.hooks.get(name)
        if hook:
            hook(args)
        try:
            if name not in self.m.funcs:
                if name not in self.ext:
                    raise KeyError("no stub for external " + name)
                r = self.ext[name](self, args)
                if self.shape:
                    self.shape.s[-1]["$ret"] = None
                return r
            return self.run(name, args, anames, caller)
        finally:
            if hook:
                hook.after()

    def run(self, name, args, anames, caller):
        params, blocks, first = self.compiled.get(name) or self.compile(name)
        fr = dict(zip(params, args))
        if self.shape:
            self.shape.enter(fr, params, anames, caller)
        sp = self.sp
        lab, prev = first, None
        try:
            while True:
                fr["$prev"] = prev
                nxt = None
                ins = blocks[lab]
                k = 0
                while k < len(ins) and getattr(ins[k], "phi", False):
                    ins[k](fr)
                    k += 1
                for dst, v, n in fr.pop("$phi", []):
                    fr[dst] = v
                    if self.shape:
                        self.shape.copy(fr, dst, n)
                for e in ins[k:]:
                    nxt = e(fr)
                prev, lab = lab, nxt
        except Ret as r:
            if self.shape:
                self.shape.leave(fr)
            return r.args[0]
        finally:
            self.sp = sp


# ============================================================================================ the SDK side

def find_tool(env, default, rel):
    base = Path(os.environ.get(env, default)).expanduser()
    p = base / rel
    return p if p.exists() else None


def unpack(sdk, work):
    arc = sdk / ARCHIVE
    if not arc.exists():
        sys.exit("wifi_ir: %s not found (set AC79_SDK)" % arc)
    digest = hashlib.sha256(arc.read_bytes()).hexdigest()
    if digest != ARCHIVE_SHA256:
        sys.exit("wifi_ir: %s has SHA-256 %s, not the %s this tool was checked against" % (arc, digest, ARCHIVE_SHA256))
    names = [m + ".c.o" for m in MEMBERS]
    subprocess.run(["ar", "x", str(arc)] + names, cwd=work, check=True)   # these eight members only
    clang = find_tool("JIELI_TOOLCHAIN", "~/.jieli/toolchain", "common/bin/clang")
    out = []
    for m in MEMBERS:
        src, ll = work / (m + ".c.o"), work / (m + ".ll")
        if clang:
            subprocess.run([str(clang), "-target", "pi32v2", "-x", "ir", "-S", "-emit-llvm", "-O0", str(src), "-o",
                            str(ll)], check=True, capture_output=True)
        else:
            subprocess.run(["llvm-dis", str(src), "-o", str(ll)], check=True)
        out.append((m, ll))
    return digest, out


def wifi_conf(sdk):
    """The receive thresholds and the RF RAM table of the SDK's compiled branch (the #else of "#if 0 // WIFI RX
    moderate strategy"), from its C source."""
    text = (sdk / WIFI_CONF).read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    start = next(i for i, ln in enumerate(lines) if ln.startswith("#if 0") and "WIFI RX" in ln)
    other = next(i for i in range(start, len(lines)) if lines[i].startswith("#else"))
    end = next(i for i in range(other, len(lines)) if lines[i].startswith("#endif"))
    body = "\n".join(lines[other + 1:end])
    thr = {m.group(1): int(m.group(2)) for m in re.finditer(r"const u32 (\w+_VAL) = (\d+);", body)}
    lut_text = body[body.index("wl_rfd_ram_lut"):]
    pairs = re.findall(r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\}", lut_text)
    lut = [(int(a, 16), int(b, 16)) for a, b in pairs]
    if len(lut) != 256 or len(thr) != 10:
        sys.exit("wifi_ir: %s: expected 10 thresholds and 256 table rows, found %d and %d" % (WIFI_CONF, len(thr), len(lut)))
    return thr, lut


def load(sdk):
    work = Path(tempfile.mkdtemp(prefix="wifi_ir."))
    try:
        digest, lls = unpack(sdk, work)
        mod = Module()
        for m, ll in lls:
            mod.read(ll, m)
    finally:
        for f in work.iterdir():
            f.unlink()
        work.rmdir()
    thr, lut = wifi_conf(sdk)
    return digest, mod, thr, lut


def externals():
    """What the IR calls outside the eight members, as stage 2's facts and fm1-base's choices have it."""

    def mmc_rw(it, a):                    # wl30_mmc_io_rw_extended(card, write, fn, addr, incr, buf, blocks, len)
        write, addr, buf = a[1], a[3], a[5]
        assert a[7] == 4 and addr not in (0, 12), "frame FIFO access in the power-up"
        if write:
            it.model.wr32(CORE + addr, it.load(("i", 32), buf))
        else:
            it.store(("i", 32), buf, it.model.rd32(CORE + addr))
        return 0

    def put(data):
        def f(it, a):
            for k, b in enumerate(data):
                it.store(("i", 8), a[0] + k, b)
            return 1
        return f

    return {
        "@wl30_mmc_io_rw_extended": mmc_rw,
        "@delay_us": lambda it, a: it.model.delay(a[0]),
        "@clk_get": lambda it, a: OSC_HZ,
        "@wifi_get_xosc": put(CAL_XOSC),
        "@wifi_get_pa_trim_data": put(CAL_PA),
        "@wifi_get_mcs_dgain": put(CAL_MCS),
        "@wifi_get_rf_trim_data": lambda it, a: M32,      # fm1-base: never a stored trim
        "@wifi_set_rf_trim_data": lambda it, a: 0,
        "@zalloc": lambda it, a: 0,
        "@free": lambda it, a: None,
        "@CRC16": lambda it, a: 0,
        "@printf": lambda it, a: 0,
        "@puts": lambda it, a: 0,
        "@sys_timeout_add_to_task": lambda it, a: 0,
        "@set_wf_phy_init_flag": lambda it, a: None,
    }


def interp(mod, thr, lut, model):
    it = Interp(mod, model, externals())
    # the SDK-source constants the archive refers to (wifi_conf.c, compiled branch), and RFIinitUseTrimValue = 0
    for g in list(mod.globals):
        if mod.globals[g]["init"] is None:
            nm = g[1:]
            if nm in thr:
                it.store(("i", 32), it.addr[g], thr[nm])
            elif nm == "wl_rfd_ram_lut":
                for k, (h, l) in enumerate(lut):
                    it.store(("i", 32), it.addr[g] + 8 * k, h)
                    it.store(("i", 32), it.addr[g] + 8 * k + 4, l)
            elif nm in ("RFIinitUseTrimValue", "RTDebugLevel"):
                it.store(("i", 8), it.addr[g], 0)
            elif nm not in ("rx_dc_tune", "pa_config_res"):
                raise KeyError("no value for external global " + g)
    return it


# The stage 3 sequence: wfhw_init's calls in docs/WIFI.md's "Power-up and calibration sequence" (the order is a fact
# read there; wfhw_init itself is in a member this tool does not read). MAC 0x0308 = 0x9407 is written by wfhw_init
# itself between wf_sifs_reset and cfr_int(0).
SEQUENCE = (("@wf_phy_mac_rst", ()), ("@wf_sifs_reset", ()), ("mac", (0x308, 0x9407)), ("@cfr_int", (0,)),
            ("@wf_phy_init", ()), ("@wf_rf_init", ()), ("@wf_phy_init", ()), ("@cfr_int", (1,)), ("@rapp_init", ()),
            ("@wl_anl_actl_init", (0,)), ("@wf_rf_bsb_auto_ctl", ()), ("@wifi_set_pwr", (6,)),
            ("@wifi_set_channel", (6,)), ("@pa_mcs_dgain_init", ()), ("@rf_dc_get_reload", ()))


def run_sequence(it):
    for name, args in SEQUENCE:
        if name == "mac":
            it.call("@sdio_mac_wreg", list(args))
        else:
            it.call(name, list(args))


# ================================================================================================= --trace

def trace(mod, thr, lut):
    model = Model()
    it = interp(mod, thr, lut, model)
    run_sequence(it)
    return model.log


def trace_text(digest, log):
    out = io.StringIO()
    out.write("# SPDX-License-Identifier: GPL-3.0-only\n")
    out.write("# Generated by tools/wifi_ir.py --trace; do not edit. The writes of docs/WIFI_STAGE3.md's sequence, as\n")
    out.write("# JieLi's wl_rf_common.a (SHA-256 %s) makes them against the register model\n" % digest)
    out.write("# (tests/wifi/sdio_model.h): register values and order, facts only. Reads are left out.\n")
    for r in log:
        out.write(rec_text(r) + "\n")
    return out.getvalue()


# ================================================================================================= the tables

class Shape:
    """Tracks, for the table generator, how each value was made, so a register store becomes one table step:
    None a constant (literals and what is computed from them alone, external results such as clk_get); ('rmw', addr,
    clear, set) a register read with bits cleared and set by constants; ('p', name, mask, shift) a calibration byte
    masked and shifted; ('rmwp', ...) the two or-ed; ('dyn',) anything else, which no table step may hold."""

    def __init__(self):
        self.s = [{}]
        self.params = {}               # memory address -> calibration byte name (wifi_get_xosc's buffer)
        self.tainted = set()           # memory addresses holding a value read from the hardware
        self.rows = []
        self.depth = 0                 # > 0 inside a function recorded as one row, or a hand-written one

    def get(self, fr, n):
        return self.s[-1].get(n) if n else None

    def enter(self, fr, params, anames, caller):
        prev = self.s[-1]
        self.s.append({p_: prev.get(a) for p_, a in zip(params, anames or []) if a})

    def leave(self, fr):
        self.s.pop()

    def ret(self, fr, n):
        self.s[-2]["$ret"] = self.s[-1].get(n) if n else None

    def callret(self, fr, dst):
        self.s[-1][dst] = self.s[-1].pop("$ret", None)

    def copy(self, fr, dst, n):
        self.s[-1][dst] = self.s[-1].get(n) if n else None

    def generic(self, fr, dst, names):
        self.s[-1][dst] = ("dyn",) if any(self.s[-1].get(n) for n in names) else None

    def load(self, fr, dst, a, ty):
        if a < MMIO_TOP:
            self.s[-1][dst] = ("rmw", a, 0, 0)
        elif a in self.params:
            self.s[-1][dst] = ("p", self.params[a], 0xFF, 0)
        elif a in self.tainted:
            self.s[-1][dst] = ("dyn",)
        else:
            self.s[-1][dst] = None

    def cast(self, fr, dst, op, n, sb, db):
        x = self.get(fr, n)
        if x is None:
            r = None
        elif x[0] == "p" and op in ("zext", "trunc", "sext"):
            r = ("p", x[1], x[2] & ((1 << min(sb, db)) - 1), x[3])
        elif x[0] in ("rmw", "rmwp") and sb == db == 32:
            r = x
        else:
            r = ("dyn",)
        self.s[-1][dst] = r

    def binop(self, fr, dst, op, names, consts, bits):
        a, b = self.get(fr, names[0]), self.get(fr, names[1])
        if a is None and b is None:
            self.s[-1][dst] = None
            return
        ca, cb = consts
        r = ("dyn",)
        x = k = None
        if a and b is None:
            x, k = a, (cb if cb is not None else fr[names[1]]) & M32
        elif b and a is None and op in ("and", "or"):
            x, k = b, (ca if ca is not None else fr[names[0]]) & M32
        if x and x[0] == "rmw" and bits == 32:
            _, ad, clr, st = x
            if op == "and":
                r = ("rmw", ad, (clr | ~k) & M32, st & k)
            elif op == "or":
                r = ("rmw", ad, (clr | k) & M32, st | k)
        elif x and x[0] == "p":
            _, nm, msk, shf = x
            if op == "and":
                r = ("p", nm, msk & (k >> shf), shf)
            elif op == "shl":
                r = ("p", nm, msk, shf + k)
        elif a and b and op == "or":
            rm, pp = (a, b) if a[0] == "rmw" else (b, a)
            if rm[0] == "rmw" and pp[0] == "p":
                field = (pp[2] << pp[3]) & M32
                if rm[2] & field == field and rm[3] & field == 0:
                    r = ("rmwp", rm[1], rm[2], rm[3], pp[1], pp[2], pp[3])
        self.s[-1][dst] = r

    def store(self, fr, a, v, n, ty):
        x = self.get(fr, n)
        if a >= MMIO_TOP:
            if x:
                self.tainted.add(a)
            else:
                self.tainted.discard(a)
            return
        if self.depth:
            return
        if x is None:
            self.rows.append(("wr", a, v & M32))
        elif x[0] == "rmw" and x[1] == a:
            self.rows.append(("rmw", a, x[2], x[3] & M32))
        elif x[0] == "rmwp" and x[1] == a:
            self.rows.append(("rmwp", a, x[2], x[3] & M32, x[4], x[5], x[6]))
        else:
            raise ValueError("a register store the tables cannot express: %#x = %#x (%r)" % (a, v, x))


# Hand-written in fm1wifi.c, from docs/WIFI_STAGE3.md's description: these have loops or values that depend on what
# the hardware answers. Their calls (and the top-level steps of SEQUENCE) split the sequence into the tables between.
HAND = {"@dig_agc_tale_set", "@rapp_init", "@wfa_pll_bank_scan", "@wf_rx_tune_test", "@txiq_tune_soft",
        "@rxdc_tune_soft", "@rxiq_tune_soft", "@pa_config_tune", "@keep_tx_iq_set", "@tx_rx_bw_set_save",
        "@wl_anl_actl_init", "@pa_mcs_dgain_init", "@rf_dc_get_reload", "@wifi_trim_is_already",
        "@wfrf_flt_vga_gain", "@wifi_set_pwr", "@wifi_set_channel", "@enter_wifi_rf_trim_region",
        "@exit_wifi_rf_trim_region"}
# Recorded as one row each (the window, BBP and MAC accesses), not as the register traffic inside them.
ROWS = {"@cca_reg_set": "cca", "@rx11n_reg_set": "rx11n", "@tx_backend_wr": "txbe", "@rx_frontend_wr": "rxfe",
        "@bbp_set": "bbp", "@sdio_mac_wreg": "mac"}


def record(mod, thr, lut, entries):
    """Run entries (a list of (function, args)) recording rows: register steps, and ('call', name, args...) where a
    hand-written function ran or a top-level entry began ('mark')."""
    model = Model()
    it = interp(mod, thr, lut, model)
    sh = Shape()
    it.shape = sh
    xosc_ext, delay_ext = it.ext["@wifi_get_xosc"], it.ext["@delay_us"]

    def xosc(it_, a):
        xosc_ext(it_, a)
        sh.params[a[0]], sh.params[a[0] + 1] = "XOSC_L", "XOSC_R"
        return 1

    def delay(it_, a):
        if not sh.depth:
            sh.rows.append(("delay", a[0]))
        return delay_ext(it_, a)
    it.ext["@wifi_get_xosc"], it.ext["@delay_us"] = xosc, delay

    def hook(kind):
        def h(args):
            if not sh.depth:
                sh.rows.append(kind(args))
            sh.depth += 1
        h.after = lambda: setattr(sh, "depth", sh.depth - 1)
        return h
    for n, k in ROWS.items():
        it.hooks[n] = hook(lambda args, k=k: (k,) + tuple(args))
    for n in HAND:
        it.hooks[n] = hook(lambda args, n=n: ("call", n) + tuple(args))
    for name, args in entries:
        sh.rows.append(("mark", name) + tuple(args))
        if name == "mac":
            it.call("@sdio_mac_wreg", list(args))
        else:
            it.call(name, list(args))
    return sh.rows


def split(rows):
    """Cut a row list at the hand-written calls and marks: [(rows, the call or mark that ends them), ...]."""
    out, cur = [], []
    for r in rows:
        if r[0] in ("call", "mark"):
            out.append((cur, r))
            cur = []
        else:
            cur.append(r)
    out.append((cur, None))
    return out


# ---- encoding (fm1wifi.c's run_steps reads it; the format is described in fm1wifi_tables.h's header)
OPS = {"field": 1, "rmw": 2, "wr": 3, "mac": 4, "bbp": 5, "win": 6, "delay": 7, "pfield": 8}
WINS = {"cca": 0, "rx11n": 1, "txbe": 2, "rxfe": 3}
PARAMS = ("XOSC_L", "XOSC_R")


def reg_index(a):
    if 0x11900 <= a < 0x11900 + 4 * 40:
        return (a - 0x11900) // 4
    if 0x14000 <= a < 0x14000 + 4 * 40:
        return 64 + (a - 0x14000) // 4
    raise ValueError("register %#x outside JL_ANA / JL_WL" % a)


def field_of(clr, st):
    """(lo, width) if clr is one run of bits and st lies inside it with a value of at most 8 bits, else None."""
    if not clr or st & ~clr:
        return None
    lo = (clr & -clr).bit_length() - 1
    run = clr >> lo
    if run & (run + 1):
        return None
    width = run.bit_length()
    if (st >> lo) > 0xFF or width > 32:
        return None
    return lo, width


def encode(rows):
    words = []
    for r in rows:
        k = r[0]
        if k == "rmw":
            f = field_of(r[2], r[3])
            if f:
                lo, w = f
                words.append(OPS["field"] << 28 | reg_index(r[1]) << 20 | lo << 15 | (w - 1) << 10 | (r[3] >> lo) << 2)
            else:
                words += [OPS["rmw"] << 28 | reg_index(r[1]) << 20, r[2], r[3]]
        elif k == "rmwp":
            _, a, clr, st, name, msk, shf = r
            if st or msk & (msk + 1) or clr != (msk << shf):
                raise ValueError("a calibration field the tables cannot express: %r" % (r,))
            words.append(OPS["pfield"] << 28 | reg_index(a) << 20 | shf << 15 | (msk.bit_length() - 1) << 10 |
                         PARAMS.index(name) << 2)
        elif k == "wr":
            words += [OPS["wr"] << 28 | reg_index(r[1]) << 20, r[2]]
        elif k == "mac":
            words += [OPS["mac"] << 28 | r[1], r[2] & M32]
        elif k == "bbp":
            words.append(OPS["bbp"] << 28 | (r[1] & 0xFF) << 8 | (r[2] & 0xFF))
        elif k in WINS:
            words.append(OPS["win"] << 28 | WINS[k] << 16 | (r[1] & 0xFF) << 8 | (r[2] & 0xFF))
        elif k == "delay":
            words.append(OPS["delay"] << 28 | r[1])
        else:
            raise ValueError("row " + repr(r))
    return words


def rows_comment(rows):
    """One comment line per row, so the header reads as the list of facts it is."""
    def nm(a):
        if 0x11900 <= a < 0x119A0:
            return "WLA_CON%d" % ((a - 0x11900) // 4)
        if 0x14040 <= a < 0x14068:
            return "WF_CON%d" % ((a - 0x14040) // 4)
        if 0x14028 <= a < 0x14038:
            return "WL_ANL_CON%d" % ((a - 0x14028) // 4)
        return "%#x" % a
    out = []
    for r in rows:
        k = r[0]
        if k == "rmw":
            f = field_of(r[2], r[3])
            if f:
                lo, w = f
                bits = "bit %d" % lo if w == 1 else "bits %d-%d" % (lo + w - 1, lo)
                out.append("%s %s = %d" % (nm(r[1]), bits, r[3] >> lo))
            else:
                out.append("%s: clear %08x, set %08x" % (nm(r[1]), r[2], r[3]))
        elif k == "rmwp":
            lo, w = r[6], r[5].bit_length()
            out.append("%s bits %d-%d = %s" % (nm(r[1]), lo + w - 1, lo, r[4]))
        elif k == "wr":
            out.append("%s = %08x" % (nm(r[1]), r[2]))
        elif k == "mac":
            out.append("MAC %04x = %08x" % (r[1], r[2] & M32))
        elif k == "bbp":
            out.append("BBP %d = %02x" % (r[1] & 0xFF, r[2] & 0xFF))
        elif k in WINS:
            out.append("%s %d = %02x" % (k, r[1] & 0xFF, r[2] & 0xFF))
        elif k == "delay":
            out.append("delay %d us" % r[1])
    return out


def c_table(name, rows, what, ram=False):
    words = encode(rows)
    com = rows_comment(rows)
    out = ["/* %s: %d steps */" % (what, len(rows)),
           "static const uint32_t %s[]%s = {" % (name, " FM1WIFI_RAMDATA" if ram else "")]
    i = 0
    for r, c in zip(rows, com):
        n = len(encode([r]))
        out.append("    " + ", ".join("0x%08x" % w for w in words[i:i + n]) + ",  /* %s */" % c)
        i += n
    out.append("};")
    return out


def c_array(name, ctype, vals, per=8, fmt="0x%08x", ram=False):
    out = ["static const %s %s[%d]%s = {" % (ctype, name, len(vals), " FM1WIFI_RAMDATA" if ram else "")]
    for i in range(0, len(vals), per):
        out.append("    " + ", ".join(fmt % v for v in vals[i:i + per]) + ",")
    out.append("};")
    return out


def global_array(it, mod, g):
    d = mod.globals[g]
    ty = d["ty"]
    a = it.addr[g]
    flat = []

    def walk(t, ad):
        if t[0] == "arr":
            es = mod.size(t[2])
            for k in range(t[1]):
                walk(t[2], ad + k * es)
        else:
            flat.append(it.load(t, ad))
    walk(ty, a)
    return flat


# Which table is which, in sequence order: (name, what it covers, the call or mark that ends it).
PLAN = (
    ("PHY_MAC_RST", "wf_phy_mac_rst: MAC_SYS_CTRL 3, MAC 0x030C, then wf_phy_rst", "@wf_sifs_reset"),
    ("SIFS", "wf_sifs_reset: the MAC's inter-frame and back-off timing", "mac"),
    ("MAC_0308", "wfhw_init: MAC 0x0308", "@cfr_int"),
    ("CFR0", "cfr_int(0): the transmit back end's crest-factor settings", "@wf_phy_init"),
    ("PHY_INIT_A", "wf_phy_init up to its AGC table: wf_phy_rst, wf_sifs_reset, MAC 0x0308", "@dig_agc_tale_set"),
    ("PHY_INIT_B", "the rest of wf_phy_init: the CCA, receive front end, 11n and transmit back end windows",
     "@wf_rf_init"),
    ("RF_INIT", "wf_rf_init up to the PLL bank scan: the radio's analog set-up, the crystal caps, wf_rf_en_set(5), "
     "the PLL restart", "@wfa_pll_bank_scan"),
    ("RF_PLL", "wf_rf_init after the bank scan: the PLL's fractional divider, a PLL restart, 80 us",
     "@wifi_trim_is_already"),
    ("TRIM_TX", "wf_rf_trim after the filter tune: wf_rf_en_set(2), the calibration attenuator", "@txiq_tune_soft"),
    ("TRIM_RXDC", "wf_rf_trim after the TX trim: the high-pass off, wf_rf_en_set(0), WLA_CON1 bit 15",
     "@rxdc_tune_soft"),
    ("TRIM_RXIQ", "wf_rf_trim after the RX DC trim: the high-pass on, wf_rf_en_set(3), the attenuator",
     "@rxiq_tune_soft"),
    ("RF_DONE", "wf_rf_init after the trim: wf_rf_en_set(0)", "@pa_config_tune"),
    ("RF_GAIN", "wf_rf_init's last register settings (gains)", "@wfrf_flt_vga_gain"),
    ("CFR1", "cfr_int(1)", "@rapp_init"),
    ("BSB_AUTO", "wf_rf_bsb_auto_ctl", "@wifi_set_pwr"),
)
# helpers the hand-written code runs as tables of their own
HELPERS = (("PLL_RESTART", "set_pll_rn_en: the PLL's reset released in steps", "@set_pll_rn_en", ()),
           ("PHY_RST", "wf_phy_rst", "@wf_phy_rst", ()),
           ("PHY_LOOPBACK", "wf_phy_loop_always_test", "@wf_phy_loop_always_test", ()))
# read during the trim, when the flash must not be: placed in RAM (FM1WIFI_RAMDATA, defined by fm1wifi.c)
IN_RAM = {"TRIM_TX", "TRIM_RXDC", "TRIM_RXIQ", "PHY_RST", "PHY_LOOPBACK", "VGA"}


def tables(digest, mod, thr, lut, show=False):
    rows = record(mod, thr, lut, list(SEQUENCE))
    named, seq, seen, pi = [], [], {}, 0
    for rs, end in split(rows):
        if show:
            print("%4d rows, then %s" % (len(rs), end and " ".join(str(x) for x in end[1:])))
            continue
        if rs:
            key = tuple(rs)
            if key in seen:
                seq.append("table " + seen[key])
            else:
                if pi >= len(PLAN):
                    raise SystemExit("wifi_ir: more straight-line parts than PLAN names")
                nm, what, until = PLAN[pi]
                if end is None or end[1] != until:
                    raise SystemExit("wifi_ir: table %s should end at %s, ends at %s" % (nm, until, end and end[1]))
                named.append((nm, what, rs))
                seen[key] = nm
                seq.append("table " + nm)
                pi += 1
        if end and end[0] == "call":
            seq.append("%s(%s)" % (end[1][1:], ", ".join(str(a) for a in end[2:] if a < 0x10000)))
    if show:
        return ""
    if pi != len(PLAN):
        raise SystemExit("wifi_ir: PLAN names %d tables, the sequence has %d" % (len(PLAN), pi))
    for nm, what, entry, args in HELPERS:
        rs = [r for r in record(mod, thr, lut, [(entry, args)]) if r[0] != "mark"]
        if any(r[0] == "call" for r in rs):
            raise SystemExit("wifi_ir: helper %s calls hand-written code" % nm)
        named.append((nm, what, rs))
    out = []
    it = interp(mod, thr, lut, Model())
    arrays = {
        "DAGC": ("uint32_t", global_array(it, mod, "@dagc_ram_set"), "wf_soft_adj: dagc_ram_set, the digital AGC table "
                 "dig_agc_tale_set loads"),
        "RAPP_RAM": ("uint16_t", global_array(it, mod, "@rapp_ram_data"), "wf_cfr_init: rapp_ram_data, the 128 words "
                     "rapp_init loads"),
        "VGA": ("uint8_t", global_array(it, mod, "@rx_flt_vga_gain"), "wf_rf_init: rx_flt_vga_gain, 42 gain steps of 4 "
                "bytes (wfrf_flt_vga_gain)"),
        "MCS_GAIN": ("uint8_t", global_array(it, mod, "@pa_mcs_dgain_init.pa_mcs_gain"), "wf_cfr_init: the 20 per-MCS "
                     "digital gains pa_mcs_dgain_init starts from"),
    }
    out.append("/* SPDX-License-Identifier: GPL-3.0-only AND Apache-2.0")
    out.append(" * Generated by tools/wifi_ir.py; do not edit (tests/run_host.sh checks it is current when the SDK is")
    out.append(" * there). fm1wifi's stage 3 register facts (docs/WIFI_STAGE3.md), as data:")
    out.append(" *  - the register steps and arrays below are register values read from JieLi's wl_rf_common.a, SHA-256")
    out.append(" *    %s, members wf_cfr_init, wf_phy_mac_init, wf_phy_mac_lib, wf_rf_cal," % digest)
    out.append(" *    wf_rf_init, wf_soft_adj, wl_rfd_init (functions named per table): which register, which value, in")
    out.append(" *    which order, the delays; facts, no code (LICENSING.md);")
    out.append(" *  - FM1WIFI_THR and FM1WIFI_RAM_LUT are the receive thresholds and the RF RAM table of the JieLi AC79 SDK's")
    out.append(" *    apps/common/net/wifi_conf.c (its compiled branch), Copyright (c) Zhuhai Jieli Technology Co., Ltd.,")
    out.append(" *    Apache-2.0 (LICENSING.md), copied as numbers.")
    out.append(" * Run on a %d MHz reference clock (the FM-1's). A step table is uint32 words; each step starts with a word" % (OSC_HZ // 1000000))
    out.append(" * whose bits 31-28 say what it is (fm1wifi.c, run_steps): 1 a field of a register (bits 27-20 the register,")
    out.append(" * 19-15 its lowest bit, 14-10 width - 1, 9-2 the value); 2 a register read, bits cleared and set (the next")
    out.append(" * two words); 3 a register written whole (the next word); 4 a MAC register (bits 15-0, value the next word);")
    out.append(" * 5 a BBP register (15-8, value 7-0); 6 a BBP window (17-16 which: CCA, RX 11n, TX back end, RX front end;")
    out.append(" * 15-8 address, 7-0 value); 7 a delay (23-0 us); 8 a register field from a calibration byte (as 1, bits 9-2")
    out.append(" * naming the byte). A register number below 64 is WLA_CON n, from 64 JL_WL word n - 64.")
    out.append(" */")
    out.append("#pragma once")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("#ifndef FM1WIFI_RAMDATA          /* what the trim reads: fm1wifi.c places it in RAM */")
    out.append("#define FM1WIFI_RAMDATA")
    out.append("#endif")
    out.append("enum { FM1WIFI_P_XOSC_L, FM1WIFI_P_XOSC_R };   /* the calibration bytes op 8 names */")
    out.append("")
    out.append("/* The order fm1wifi_radio_up runs these tables and its hand-written steps in (docs/WIFI_STAGE3.md):")
    for s in seq:
        out.append(" *   " + s)
    out.append(" */")
    out.append("")
    for nm, what, rs in named:
        out += c_table("FM1WIFI_T_" + nm, rs, what, nm in IN_RAM)
        out.append("")
    for nm, (ct, vals, what) in arrays.items():
        out.append("/* %s */" % what)
        fmt = "0x%08x" if ct == "uint32_t" else "0x%04x" if ct == "uint16_t" else "0x%02x"
        out += c_array("FM1WIFI_" + nm, ct, vals, per=8 if ct != "uint8_t" else 16, fmt=fmt, ram=nm in IN_RAM)
        out.append("")
    out.append("/* wifi_conf.c: the receive thresholds wf_phy_init writes into the CCA window (STF_END_CNT_THR_VAL ..) */")
    for k in ("STF_END_CNT_THR_VAL", "STF_DET_CNT_THR_VAL", "DS_DET_CNT_THR0_VAL", "STF_DET_THR1_VAL", "STF_DET_THR2_VAL",
              "STF_DET_THR3_VAL", "DSSS_XCDET_THR1_VAL", "DSSS_XCDET_THR2_VAL", "DS_VLD_CNT_THR_VAL",
              "DS_DET_CNT_THR1_VAL"):
        out.append("#define FM1WIFI_THR_%s %du" % (k[:-4], thr[k]))
    out.append("")
    out.append("/* wifi_conf.c: wl_rfd_ram_lut, the RF digital block's RAM (entries 0-127 frequency, 128-223 AGC, 224- PA),")
    out.append(" * {high word, low word} per entry */")
    out += c_array("FM1WIFI_RAM_LUT", "uint32_t", [w for pr in lut for w in pr], per=8)
    out.append("")
    return "\n".join(out)


# ================================================================================================= main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--trace", action="store_true", help="write tests/wifi/radio_trace.txt.gz")
    ap.add_argument("--check", action="store_true", help="fail if the committed tables or trace differ")
    ap.add_argument("--sdk", default=os.environ.get("AC79_SDK", "~/fw-AC79_AIoT_SDK"))
    ap.add_argument("--print", action="store_true", help="print the trace as text instead of writing it")
    ap.add_argument("--segments", action="store_true", help="print how the sequence splits into tables")
    a = ap.parse_args()
    sdk = Path(a.sdk).expanduser()
    digest, mod, thr, lut = load(sdk)
    if a.segments:
        tables(digest, mod, thr, lut, show=True)
        return
    if a.check:
        bad = 0
        want = tables(digest, mod, thr, lut)
        if TABLES.read_text() != want:
            print("wifi_ir: %s is not what the SDK gives (run tools/wifi_ir.py)" % TABLES.relative_to(ROOT))
            bad = 1
        tt = trace_text(digest, trace(mod, thr, lut))
        if gzip.decompress(TRACE.read_bytes()).decode() != tt:
            print("wifi_ir: %s is not what the SDK gives (run tools/wifi_ir.py --trace)" % TRACE.relative_to(ROOT))
            bad = 1
        if not bad:
            print("wifi_ir: tables and trace match the SDK's archive")
        sys.exit(bad)
    if a.trace or a.print:
        tt = trace_text(digest, trace(mod, thr, lut))
        if a.print:
            sys.stdout.write(tt)
            return
        TRACE.write_bytes(gzip.compress(tt.encode(), compresslevel=9, mtime=0))
        print("wifi_ir: %s, %d records" % (TRACE.relative_to(ROOT), tt.count("\n") - 4))
        return
    TABLES.write_text(tables(digest, mod, thr, lut))
    print("wifi_ir: %s" % TABLES.relative_to(ROOT))


if __name__ == "__main__":
    main()
