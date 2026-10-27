#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build OPLBOX (based on Dinghy by zvenson, on felucca-bsp's chip and board layer in firmware/bsp/): the app,
the update loader and an installable .fwsc package.

  tools/build.py [--release X.Y[-suffix]]

Outputs in build/: oplbox.bin (app), loader/ota.bin (update loader),
oplbox.fwsc (package; oplbox-X.Y.fwsc with --release X.Y). See README.md for the toolchain and the SDK.

The JieLi toolchain is Linux x86-64 only. JIELI_TOOLCHAIN points at it; on
macOS (or with JIELI_DOCKER=1) each tool runs in a linux/amd64 container.
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
BSP = FW / "bsp"                    # felucca-bsp, vendored (MPL-2.0; firmware/bsp/README.md)
BSP_INC = ["-Ifirmware/bsp/chip", "-Ifirmware/bsp/boards/fm1"]
OUT = SRC / "build"
GEN = OUT / "gen"
LDR = OUT / "loader"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

APP_XIP = 0x02000120                # app.bin offset 0 in the XIP map; the SPL jumps here
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]   # keep -Os for the app: see build_app
SCOPE_N = 512                       # the scope screen's taps per channel (dbopl2.h, DBOPL2_SCOPE_N)
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

# SDK files of AC79NN_SDK_V1.2.1_2023-12-13 (the tested version)
SDK_SHA256 = {
    "uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}

PRODUCT = "FM-1_960"                # OPLBOX: 96N (the installers resume update loaders named FM-1_9NN)
VERSION = None                      # APP_VER for release builds


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN")
    if not tc or not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("JIELI_TOOLCHAIN must point at the JieLi Linux toolchain "
                         "(the directory with pi32v2/ and common/; see tools/get_toolchain.sh)")
    return Path(tc).resolve()


def use_docker():
    native = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
    return os.environ.get("JIELI_DOCKER", "0" if native else "1") == "1"


def tc(tool, *args):
    """run a toolchain binary (pi32v2/bin/..., common/bin/...) with cwd SRC; paths relative to SRC"""
    rel = [str(Path(a).resolve().relative_to(SRC)) if isinstance(a, Path) else a for a in args]
    if tool == "cc":                # the toolchain's cc wrapper needs python3; call clang directly
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    if use_docker():
        cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{SRC}:/work",
               "-v", f"{toolchain()}:/opt/jieli:ro", "-w", "/work", DOCKER_IMAGE, f"/opt/jieli/{tool}", *rel]
    else:
        cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def tc_all(*cmds):
    with ThreadPoolExecutor(len(cmds)) as ex:
        return list(ex.map(lambda c: tc(*c), cmds))


# ---- update loader

def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
    blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader():
    LDR.mkdir(parents=True, exist_ok=True)
    src = BSP / "loader"
    flags = [*CFLAGS, *BSP_INC]
    tc_all(("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
           ("cc", *flags, "-c", src / "loader.c", "-o", LDR / "loader.o"))
    elf = LDR / "loader.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (LDR / "loader.dis").write_text(dis)
    for ln in hdr.splitlines():     # the loader rewrites the flash: nothing may run from XIP
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(image)
    (LDR / "ota.bin").write_bytes(ota)
    bss = [int(ln.split()[0], 16) for ln in syms.splitlines() if ln.rstrip().endswith("_bss_end")]
    print(f"loader: image {len(image)} B at {LOADER_LOAD:#x}" + (f", bss end {bss[0]:#x}" if bss else ""))
    print(f"loader: ota.bin {len(ota)} B")
    return ota


# ---- app

def build_app():
    inc = [*BSP_INC, "-Ifirmware/hal", "-Ifirmware/wifi", "-Ifirmware/src", "-Ifirmware/gen"]   # hal/: OPLBOX's own hardware files
    # the app (platform included) at -Os like Dinghy: at -O2 the USB stack babbles on the device (USB-MIDI IN,
    # dinghy tools/build.py; OPLBOX 0.4.1 dropped off USB on every SysEx). The OPL core, the budget, is its own
    # object at -O2.
    flags = [*CFLAGS, *inc, f'-DFELUCCA_ID="{PRODUCT}"', f'-DAPP_VER="{VERSION or "0.0"}"', f"-DDBOPL2_SCOPE_N={SCOPE_N}"]
    core = [*[f for f in CFLAGS if f != "-Os"], "-O2", *inc, "-DDBOPL2_ON_DEVICE", f"-DDBOPL2_SCOPE_N={SCOPE_N}"]
    cmds = [("cc", "-c", BSP / "chip" / "crt0.S", "-o", OUT / "crt0.o"),
            ("cc", "-c", BSP / "chip" / "fm1_vec.S", "-o", OUT / "fm1_vec.o"),
            ("cc", "-c", BSP / "chip" / "fm1_isr.S", "-o", OUT / "fm1_isr.o"),
            ("cc", *flags, "-c", FW / "src" / "oplbox.c", "-o", OUT / "oplbox.o"),
            ("cc", *core, "-c", FW / "src" / "dbopl2.c", "-o", OUT / "dbopl2.o"),
            # the WiFi library (firmware/wifi/, for the WiFi test page) and its port on felucca-bsp (hal/)
            ("cc", *CFLAGS, "-Ifirmware/wifi", "-c", FW / "wifi" / "fm1wifi.c", "-o", OUT / "fm1wifi.o"),
            ("cc", *CFLAGS, "-Ifirmware/wifi", "-c", FW / "wifi" / "fm1wifi_frame.c", "-o", OUT / "fm1wifi_frame.o"),
            ("cc", *CFLAGS, *BSP_INC, "-Ifirmware/wifi", "-c", FW / "hal" / "fm1wifi_port_bsp.c", "-o", OUT / "fm1wifi_port_bsp.o")]
    # stage 8 (docs/WIFI_STAGE8.md): the WPA2 station, the EAPOL handshakes, the key primitives and their BearSSL subset
    wpa = [FW / "wifi" / "fm1wifi_wpa.c", FW / "wifi" / "wpa_eapol.c", FW / "wifi" / "wpa_crypto.c",
           *sorted((FW / "wifi" / "bearssl").glob("*.c"))]
    wpa_objs = [OUT / ("wpa_" + s.stem + ".o") for s in wpa]
    cmds += [("cc", *CFLAGS, "-Ifirmware/wifi", "-c", s, "-o", o) for s, o in zip(wpa, wpa_objs)]
    tc_all(*cmds)
    elf = OUT / "oplbox.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", FW / "app.ld", OUT / "crt0.o", OUT / "fm1_vec.o",
       OUT / "fm1_isr.o", OUT / "oplbox.o", OUT / "dbopl2.o", OUT / "fm1wifi.o", OUT / "fm1wifi_frame.o",
       OUT / "fm1wifi_port_bsp.o", *wpa_objs, "-o", elf)
    for sect in ("text.bin", "data.bin", "ramtext.bin", "ramcode.bin"):
        (OUT / sect).unlink(missing_ok=True)
    *_, syms, dis, rt = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, OUT / "text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, OUT / "data.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, OUT / "ramtext.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_code", elf, OUT / "ramcode.bin"),
                               ("common/bin/objdump", "-t", elf),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-d", "-j", ".ram_text", elf))
    (OUT / "oplbox.dis").write_text(dis)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    img = bytearray((OUT / "text.bin").read_bytes())
    # .ram_text, .ram_code and .data follow .text at their load addresses; the start copies them by words:
    # .ram_text and .ram_code as one range (_rt_start.._rt_end), so their load addresses must be contiguous
    rc_size = (OUT / "ramcode.bin").stat().st_size if (OUT / "ramcode.bin").exists() else 0
    if rc_size and symv("_rc_load") - symv("_rt_load") != symv("_rc_start") - symv("_rt_start"):
        raise SystemExit(".ram_code is not contiguous with .ram_text (load vs run offsets differ)")
    for sect, lname in (("ramtext.bin", "_rt_load"), ("ramcode.bin", "_rc_load"), ("data.bin", "_data_load")):
        load = symv(lname)
        if load % 4:
            raise SystemExit(f"{lname} {load:#x} is not word aligned")
        blob = (OUT / sect).read_bytes() if (OUT / sect).exists() else b""
        if blob:
            if load - APP_XIP < len(img):
                raise SystemExit(f"{lname} overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img))
            img += blob
    img += b"\xff" * (-len(img) % 4)
    (OUT / "oplbox.bin").write_bytes(img)
    return bytes(img), syms, dis, rt


# ---- the WiFi library (firmware/wifi/, docs/WIFI.md): every file compiled alone with the app's flags and only its
# own directory on the include path, so a dependency on OPLBOX or the bsp fails here, and its size is known
# against the budget there. fm1wifi.c, fm1wifi_frame.c and stage 8's WPA2 files (fm1wifi_wpa.c, wpa_eapol.c,
# wpa_crypto.c, bearssl/) are linked (the WiFi test page); fm1wifi_net.c (IP) not yet.
# fm1wifi_net.c (stage 7) also gets lwIP's headers and options (wifi/lwip/, still inside the library); the vendored
# lwIP sources themselves are not compiled here (they need the C library's string.h: wifi/lwip/README.md).
# Object sizes without --gc-sections: an upper bound.

def build_wifi():
    srcs = sorted([*(FW / "wifi").glob("*.c"), *(FW / "wifi" / "bearssl").glob("*.c")])
    out = OUT / "wifi"
    out.mkdir(parents=True, exist_ok=True)
    objs = [out / f"{s.parent.name}_{s.stem}.o" for s in srcs]
    lwip = ["-Ifirmware/wifi/lwip/port", "-Ifirmware/wifi/lwip/src/include"]
    tc_all(*[("cc", *CFLAGS, "-Ifirmware/wifi", *lwip, "-c", s, "-o", o) for s, o in zip(srcs, objs)])
    code = data = 0
    for o in objs:
        for ln in tc("common/bin/objdump", "-h", o).splitlines():
            p = ln.split()
            if len(p) > 3 and re.match(r"\.(text|rodata)", p[1]):
                code += int(p[2], 16)
            elif len(p) > 3 and re.match(r"\.(data|bss)", p[1]):
                data += int(p[2], 16)
    return f"firmware/wifi/ alone: {len(objs)} objects, {code} B code+rodata, {data} B data+bss (fm1wifi_net.c not linked yet)"


# ---- the BLE HCI layer (firmware/ble/, docs/BLUETOOTH.md stage 1): compiled alone with the app's flags and only its
# own directory on the include path, like firmware/wifi/, and sized. Not linked: nothing calls it until a controller
# runs (stage 4). Object sizes without --gc-sections: an upper bound.

def build_ble():
    srcs = sorted((FW / "ble").glob("*.c"))
    out = OUT / "ble"
    out.mkdir(parents=True, exist_ok=True)
    objs = [out / f"{s.stem}.o" for s in srcs]
    tc_all(*[("cc", *CFLAGS, "-Ifirmware/ble", "-c", s, "-o", o) for s, o in zip(srcs, objs)])
    code = data = 0
    for o in objs:
        for ln in tc("common/bin/objdump", "-h", o).splitlines():
            p = ln.split()
            if len(p) > 3 and re.match(r"\.(text|rodata)", p[1]):
                code += int(p[2], 16)
            elif len(p) > 3 and re.match(r"\.(data|bss)", p[1]):
                data += int(p[2], 16)
    return f"firmware/ble/ alone: {len(objs)} objects, {code} B code+rodata, {data} B data+bss (not linked yet)"


# ---- the WiFi trim's RAM code (docs/WIFI_STAGE3.md "What runs from RAM"): the radio's trim must not touch the flash,
# so the sections it runs from (fm1wifi.c's .ram_code.fm1wifi, the port's .ram_code.fm1wifi_port) may refer only to
# RAM: other RAM code, .data, .bss and .noinit (fm1wifi_where). Read from the objects' own relocations (a small ELF32
# reader: the toolchain's objdump does not name this target's relocation symbols).

def elf_relocs(path):
    """[(target section, symbol, the symbol's section or None if undefined)], and {defined symbol: section}"""
    import struct
    b = Path(path).read_bytes()
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    sh = [struct.unpack_from("<IIIIIIIIII", b, shoff + i * shentsize) for i in range(shnum)]

    def cstr(off):
        return b[off:b.index(b"\0", off)].decode()
    names = [cstr(sh[shstrndx][4] + h[0]) for h in sh]
    out, defined = [], {}
    for i, h in enumerate(sh):
        if h[1] == 2:                                    # SHT_SYMTAB
            syms = []
            for k in range(h[5] // 16):
                st_name, _, _, _, _, shndx = struct.unpack_from("<IIIBBH", b, h[4] + 16 * k)
                nm = cstr(sh[h[6]][4] + st_name)
                sec = names[shndx] if 0 < shndx < len(names) else ("COMMON" if shndx == 0xFFF2 else None)
                syms.append((nm, sec))
                if sec and nm:
                    defined[nm] = sec
    for h in sh:
        if h[1] == 4:                                    # SHT_RELA
            for k in range(h[5] // 12):
                _, info, _ = struct.unpack_from("<IIi", b, h[4] + 12 * k)
                nm, sec = syms[info >> 8]
                out.append((names[h[7]], nm or sec, sec))
    return out, defined


def trim_ram_check(objs):
    strict = (".ram_code.fm1wifi", ".ram_code.fm1wifi_port")
    rels, defined = [], {}
    for o in objs:
        r, d = elf_relocs(o)
        rels += r
        defined.update(d)
    errors, n = [], 0
    for target, nm, sec in rels:
        if target not in strict:
            continue
        n += 1
        sec = sec or defined.get(nm)
        if not sec or not re.match(r"\.(ram_code|bss|sbss|data|sdata|noinit)\b|COMMON$", sec):
            errors.append(f"WiFi trim RAM code ({target}) refers to {nm} in {sec or 'no object'}: the flash during the trim")
    return errors, f"WiFi trim RAM code: {n} references, all to RAM"


def check(img, syms, dis, rt):
    errors, notes = [], []
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
    if rt_calls:                    # RAM code runs with the flash off: no calls into XIP
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    else:
        notes.append(f".ram_text: {len([ln for ln in rt.splitlines() if LINE.match(ln)])} insns, no calls")
    for ln in dis.splitlines():     # nothing may call or load an address in the chip ROM
        mm = LINE.match(ln)
        if not mm:
            continue
        for v in re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
            val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
            if 0xFFC00000 <= val < 0xFFD00000:
                errors.append(f"reference to ROM address {val:#010x}")
    if len(img) > APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot")

    def sym(name):
        mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
        return int(mm.group(1), 16) if mm else 0
    ram = sym("_bss_end") - 0x01C08000
    pool = sym("_pool_end") - 0x01C20000
    rtext = sym("_rt_end") - 0x01C00000
    notes.append(f"image {len(img)} B of {APP_SLOT}; RAM .data+.bss {ram} B of 98304; pool {pool} B of 344064; "
                 f"RAM code .ram_text+.ram_code {rtext} B of 24576")
    if rtext > 0x6000:
        errors.append(f"RAM code {rtext} B > 24 KiB")
    if ram > 96 * 1024:
        errors.append(f"RAM .data+.bss {ram} B > 96 KiB")
    return errors, notes


# Every hardware register access lives in the bsp (firmware/bsp/) or in OPLBOX's own hal/. In src/ and wifi/
# (comments stripped):
#  - no volatile pointer cast, except of a C object's address (`*(volatile T *)&x`: a read-once
#    of a RAM flag shared with an ISR, e.g. usb.c ota_wire_send);
#  - no literal in a register or reserved window (core SFRs 0x10000-0x13FFF, SFC 0x40000-0x43FFF,
#    GPIO/IOMAP 0x50000-0x51FFF, CPU 0x1EE0000-0x1EEFFFF, RAM top 0x01C7F000- (boot info,
#    mailbox, vectors), XIP 0x02000000-0x020FFFFF);
#  - no inline asm, except the empty compiler barrier RING_PUBLISH() (emits no instruction);
#  - no HAL register macro (a bsp/ or hal/ #define that is, or expands to, a volatile access).
# The vendored BearSSL in wifi/bearssl/ and lwIP in wifi/lwip/src/ skip the address-literal rule only: their constants
# (AES Rcon 0x02000000, lwIP's TCP MSS option 0x02040000) fall in the windows, and the other three rules still hold
# there. So does wifi/fm1wifi_regs.h, the WiFi register
# map: its addresses are numbers the library hands to its port (hal/fm1wifi_port_bsp.c makes the access), and
# the other rules keep any access out of firmware/wifi/; and wifi/fm1wifi_tables.h, generated register values
# (tools/wifi_ir.py) some of which look like addresses.
# Linker symbols (_bss_start[], _rt_load[], ...) are plain C objects and pass.
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|"
                      r"1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check():
    def strip(s):
        s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
        return re.sub(r"//[^\n]*", "", s)
    defs = {}
    for h in sorted([*(BSP / "chip").glob("*.h"), *(BSP / "boards" / "fm1").glob("*.h"), *(FW / "hal").glob("*.h")]):
        for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
            defs[m.group(1)] = m.group(2)
    regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
    while True:                                   # macros built on register macros (FM1_WR_LIMIT_H -> FM1_X2)
        more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
        if not more:
            break
        regs |= more
    errors = []
    for f in sorted([*(FW / "src").glob("*.[ch]"), *(FW / "wifi").rglob("*.[ch]"), *(FW / "ble").rglob("*.[ch]")]):
        for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
            where = f"{f.relative_to(FW)}:{no}"
            for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"),
                             (MMIO_ASM, "inline asm")):
                if rx is MMIO_LIT and (f.parent == FW / "wifi" / "bearssl" or FW / "wifi" / "lwip" / "src" in f.parents or
                                       f.name in ("fm1wifi_regs.h", "fm1wifi_tables.h")):
                    continue
                m = rx.search(ln)
                if m:
                    errors.append(f"{where}: {what} outside bsp/ and hal/ ({m.group(0).strip()}); add a hal/ helper")
            used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
            if used:
                errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside bsp/ and hal/; use a hal/ helper")
    return errors


def main():
    global PRODUCT, VERSION
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", metavar="X.Y", help="release build: identity FM-1_9XY, version string X.Y")
    ap.add_argument("--sdk", type=Path, help="JieLi AC79 SDK checkout (default: $AC79_SDK)")
    a = ap.parse_args()
    name = "oplbox.fwsc"
    if a.release:                   # one digit each: the identity has room for two (X.Y; a .Z only in the text)
        m = re.fullmatch(r"(\d)\.(\d)(\.\d)?(-[A-Za-z0-9]+)?", a.release)
        if not m:
            raise SystemExit(f"--release {a.release}: use X.Y, X.Y.Z or X.Y-suffix, one digit each")
        PRODUCT = "FM-1_96" + m[2]
        VERSION = m[1] + "." + m[2] + (m[3] or "")
        name = f"oplbox-{a.release}.fwsc"
    fm1pkg_make.SDK = a.sdk
    for rel, sha in SDK_SHA256.items():          # fail early without the SDK
        if hashlib.sha256(fm1pkg_make.sdk_file(rel)).hexdigest() != sha:
            print(f"warning: SDK {rel} differs from AC79NN_SDK_V1.2.1; the package will not match the reference")
    OUT.mkdir(parents=True, exist_ok=True)
    ota = build_loader()
    img, syms, dis, rt = build_app()
    errors, notes = check(img, syms, dis, rt)
    notes.append(build_wifi())
    notes.append(build_ble())
    trim_err, trim_note = trim_ram_check([OUT / "fm1wifi.o", OUT / "fm1wifi_port_bsp.o"])
    errors += trim_err
    if not trim_err:
        notes.append(trim_note)
    hal_err = mmio_check()
    errors += hal_err
    if not hal_err:
        notes.append("register access: bsp/ and hal/ only (src/, wifi/, ble/ clean)")
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"app      {OUT / 'oplbox.bin'}  {len(img)} B")
    print(f"loader   {LDR / 'ota.bin'}  {len(ota)} B")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
