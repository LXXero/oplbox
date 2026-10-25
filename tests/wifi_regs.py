# SPDX-License-Identifier: GPL-3.0-only
# Check firmware/wifi/fm1wifi_regs.h against the tables in docs/WIFI.md (and the MACRegTable address list in
# docs/WIFI_REVIEW.md): every address the doc gives must be the header's, and every register name the doc's
# WiFi-core table names must exist in the header.
#   python3 tests/wifi_regs.py <output of build/host/wifi_regs>
# Exits 1 on the first run with a mismatch and lists them all.
import re
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
hdr = {}
for ln in Path(sys.argv[1]).read_text().splitlines():
    name, val = ln.split()
    hdr[name] = int(val, 16)
doc = (root / "docs/WIFI.md").read_text()
review = (root / "docs/WIFI_REVIEW.md").read_text()

bad, seen = [], 0
def want(name, val, where):
    global seen
    seen += 1
    if name not in hdr:
        bad.append(f"{where}: {name} is not in the header")
    elif hdr[name] != val:
        bad.append(f"{where}: {name} is {hdr[name]:#x} in the header, {val:#x} in the doc")

def section(title):
    m = re.search(r"^### " + re.escape(title) + r"\n(.*?)(?=^### |\Z)", doc, re.M | re.S)
    if not m:
        bad.append(f"docs/WIFI.md has no section '{title}'")
        return ""
    return m.group(1)

HEX = r"0x[0-9A-Fa-f]+"

# "CPU-side registers": the block ranges and the internal SDIO host's addresses
cpu = section("CPU-side registers (memory-mapped)")
for first, last, block, wl in [("FM1_JL_ANA_BASE", "FM1_WLA_CON(30)", "JL_ANA", "WLA_CON"),
                               ("FM1_JL_WL_BASE", "FM1_WF_CON(9)", "JL_WL", "WL_CON")]:
    m = re.search(rf"^\| ({HEX})-({HEX}) \| `{block}` \(`lsfr_base \+ map_adr\((0x[0-9a-f]+), 0\)`\)", cpu, re.M)
    if not m:
        bad.append(f"CPU-side table: no {block} row")
        continue
    want(first, int(m[1], 16), f"CPU-side table {block} start")
    want(last, int(m[2], 16), f"CPU-side table {block} end")
    want(first, 0x10000 + 64 * int(m[3], 16) * 4, f"CPU-side table {block} map_adr")
m = re.search(rf"^\| ({HEX})-({HEX}) \| not named in WL82\.h[^|]*\| ([^|]*)\|", cpu, re.M)
if not m:
    bad.append("CPU-side table: no internal SDIO host row")
else:
    want("FM1_WL30_HOST_BASE", int(m[1], 16), "CPU-side table SDIO host start")
    want("FM1_WL30_HOST_LAST", int(m[2], 16), "CPU-side table SDIO host end")
    used = []
    for a, b in re.findall(rf"({HEX})(?:-({HEX}))?", m[3]):
        used += range(int(a, 16), int(b or a, 16) + 1, 4)
    for a in used:
        want(f"FM1_WL30_HOST_{a - 0x30F00:02X}", a, "CPU-side table SDIO host register")
m = re.search(r"`IRQ_WF_IDX` = (\d+)", cpu)
want("FM1_IRQ_WF", int(m[1]) if m else -1, "IRQ_WF_IDX")

# "WiFi core registers": rows of addresses and rt2800.h names, paired in order
core = section("WiFi core registers (SDIO function 1)")
rows = 0
for ln in core.splitlines():
    cols = [c.strip() for c in re.split(r"(?<!\\)\|", ln)[1:-1]]   # a \| inside a cell is not a column
    if len(cols) != 3 or not cols[0].startswith(("0x", "22 ", "function")):
        continue
    rows += 1
    addrs = [int(a, 16) for a in re.findall(HEX, cols[0])]
    names = []
    for n in re.findall(r"`([A-Z][A-Z0-9_]+(?:/\d)*)`", cols[1]):   # `TX_SW_CFG0/1/2` is three names
        base, *more = n.split("/")
        names += [base] + [base[:-1] + d for d in more]
    if cols[0].startswith("22 "):                    # the MACRegTable row: names only, addresses from the review
        for n in names:
            if n not in hdr:
                bad.append(f"WiFi core table MACRegTable row: {n} is not in the header")
        continue
    if cols[0].startswith("function"):               # frame data: write address 0, read address 12
        want("FM1_WF_TX_FIFO", int(re.search(r"address (\d+) \(write\)", cols[0])[1]), "WiFi core frame data")
        want("FM1_WF_RX_FIFO", int(re.search(r"and (\d+) \(read\)", cols[0])[1]), "WiFi core frame data")
        continue
    if cols[1].startswith("none"):                   # JieLi's own: named by address
        for a in addrs:
            want(f"FM1_WF_REG_{a:04X}", a, "WiFi core table, JieLi's own")
        continue
    if len(names) != len(addrs):
        bad.append(f"WiFi core table row '{cols[0]}': {len(addrs)} addresses, {len(names)} names")
        continue
    for n, a in zip(names, addrs):
        want(n, a, f"WiFi core table row '{cols[0]}'")
    for a in re.findall(rf"with ({HEX}), not in rt2800\.h", cols[2]):
        want(f"FM1_WF_REG_{int(a, 16):04X}", int(a, 16), "WiFi core table, not in rt2800.h")
if rows < 8:
    bad.append(f"WiFi core table: only {rows} rows parsed")

# BBP_CSR_CFG and MAC_SYS_CTRL values the doc quotes, built from the header's masks
row = re.search(r"^\| 0x101C \|.*$", core, re.M)
if row:
    r = row[0]
    m = re.search(r"polls bit (\d+) \(`BUSY`\)", r)
    want("BBP_CSR_CFG_BUSY", 1 << int(m[1]) if m else -1, "BBP_CSR_CFG busy bit")
    m = re.search(rf"\\?\| ({HEX})` \(`BBP_RW_MODE`\) then `\\?\| ({HEX})`", r)
    if m:
        want("BBP_CSR_CFG_BBP_RW_MODE", int(m[1], 16), "BBP_CSR_CFG write")
        if hdr.get("BBP_CSR_CFG_BBP_RW_MODE", 0) | hdr.get("BBP_CSR_CFG_BUSY", 0) != int(m[2], 16):
            bad.append("BBP_CSR_CFG: RW_MODE | BUSY is not the doc's write value")
    else:
        bad.append("BBP_CSR_CFG row: write values not found")
    m = re.search(rf"a read uses `({HEX})`/`({HEX})` \(`READ_CONTROL`\)", r)
    rw, busy, rd = (hdr.get(k, 0) for k in ("BBP_CSR_CFG_BBP_RW_MODE", "BBP_CSR_CFG_BUSY", "BBP_CSR_CFG_READ_CONTROL"))
    if not m or (rw | rd, rw | rd | busy) != (int(m[1], 16), int(m[2], 16)):
        bad.append("BBP_CSR_CFG: RW_MODE | READ_CONTROL (| BUSY) is not the doc's read value")
    if "reg<<8" in r:
        want("BBP_CSR_CFG_REGNUM", 0xFF00, "BBP_CSR_CFG register number in bits 8-15")
else:
    bad.append("WiFi core table: no BBP_CSR_CFG row")
row = re.search(r"^\| 0x1004 \|.*$", core, re.M)
m = row and re.search(r"writes (\d+) \(reset CSR and BBP\).*writes (\d+) \(enable TX and RX\)", row[0])
if m:
    if hdr.get("MAC_SYS_CTRL_RESET_CSR", 0) | hdr.get("MAC_SYS_CTRL_RESET_BBP", 0) != int(m[1]):
        bad.append("MAC_SYS_CTRL: RESET_CSR | RESET_BBP is not the doc's reset value")
    if hdr.get("MAC_SYS_CTRL_ENABLE_TX", 0) | hdr.get("MAC_SYS_CTRL_ENABLE_RX", 0) != int(m[2]):
        bad.append("MAC_SYS_CTRL: ENABLE_TX | ENABLE_RX is not the doc's enable value")
else:
    bad.append("WiFi core table: MAC_SYS_CTRL values not found")

# the four BBP windows: "cca_reg_set(a, d)` = BBP 0xC9 = a, 0xCA = d, 0xC8 = 1; `rx11n_reg_set` = 0xCD/0xCE/0xCC; ..."
m = re.search(rf"`cca_reg_set\(a, d\)` = BBP ({HEX}) = a, ({HEX}) = d, ({HEX}) = 1", core)
win = [("CCA", m.groups() if m else None)]
for fn, key in (("rx11n_reg_set", "RX11N"), ("tx_backend_wr", "TXBE"), ("rx_frontend_wr", "RXFE")):
    m = re.search(rf"`{fn}` = ({HEX})/({HEX})/({HEX})", core)
    win.append((key, m.groups() if m else None))
for key, g in win:
    if not g:
        bad.append(f"BBP window {key} not found in docs/WIFI.md")
        continue
    for part, a in zip(("ADDR", "DATA", "STROBE"), g):
        want(f"FM1_BBP_{key}_{part}", int(a, 16), f"BBP window {key}")

# stage 2: the core's window ("function 1 address A is the word at CPU address `0x30000` + A") and CLK_CON1
m = re.search(rf"function 1 address A is the word at CPU address `({HEX})` \+ A", core)
want("FM1_WIFI_CORE_BASE", int(m[1], 16) if m else -1, "the core's window")
m = re.search(rf"`CLK_CON1` \(({HEX})\), bits 15-14", doc)
want("FM1_CLK_CON1_ADDR", int(m[1], 16) if m else -1, "CLK_CON1")
want("FM1_CLK_CON1_WIFI", 0xC000, "CLK_CON1 bits 15-14")

# stage 2 reads MAC_CSR0 at 0x1000
m = re.search(rf"`MAC_CSR0` \(({HEX}), ASIC version\)", doc)
want("MAC_CSR0", int(m[1], 16) if m else -1, "stage 2 MAC_CSR0")

# the 22 MACRegTable addresses (docs/WIFI_REVIEW.md, address/value pairs; only the addresses are used)
m = re.search(r"reproduces this `MACRegTable` \(address, value\):\s*```text\n(.*?)```", review, re.S)
pairs = re.findall(r"\b([0-9A-F]{4}) [0-9A-F]{8}\b", m[1]) if m else []
if len(pairs) != 22:
    bad.append(f"WIFI_REVIEW.md MACRegTable: {len(pairs)} addresses, want 22")
rt = [n for n in hdr if not n.startswith("FM1_")]
by_addr = {}
for n in rt:
    if not any(n.startswith(m + "_") for m in rt):   # a register, not one of its field masks
        by_addr.setdefault(hdr[n], []).append(n)
for a in pairs:
    if not by_addr.get(int(a, 16)):
        bad.append(f"MACRegTable address 0x{a} has no rt2800.h name in the header")
seen += len(pairs)

for b in bad:
    print("FAIL:", b, file=sys.stderr)
if bad:
    sys.exit(1)
print(f"checks passed: WiFi register header, {seen} addresses and fields match docs/WIFI.md")
