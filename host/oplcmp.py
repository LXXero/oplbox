#!/usr/bin/env python3
"""Semantic comparison of two OPL2 register streams: a DOSBox .dro and my player's regs.txt.
Replays both into a chip-state model and lists every key-on as (channel, note regs, operator regs, C0),
then diffs the two event sequences. usage: oplcmp.py dosbox.dro mine.txt [rate]"""
import struct, sys

SLOT = [0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21]      # slot -> register offset
CH_OP = [(0, 3), (1, 4), (2, 5), (6, 9), (7, 10), (8, 11), (12, 15), (13, 16), (14, 17)]
RHY = {4: ('BD', 6, (12, 15)), 3: ('SD', 7, (16,)), 2: ('TOM', 8, (14,)), 1: ('TC', 8, (17,)), 0: ('HH', 7, (13,))}

def read_dro(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'DBRAWOPL' and struct.unpack('<HH', d[8:12]) == (2, 0)
    n = struct.unpack('<I', d[12:16])[0]; sdc, ldc, cml = d[23], d[24], d[25]
    cmap = d[26:26 + cml]; p = 26 + cml; t = 0; out = []
    for _ in range(n):
        c, v = d[p], d[p + 1]; p += 2
        if c == sdc: t += v + 1
        elif c == ldc: t += (v + 1) * 256
        elif not (c & 0x80): out.append((t, cmap[c & 0x7F], v))
    return out

def read_mine(path, rate):
    out = []
    for ln in open(path):
        s, r, v = ln.split(); out.append((int(s) * 1000 // rate, int(r, 16), int(v, 16)))
    return out

def events(stream):
    """replay; yield key-on events with the channel's state"""
    reg = [0] * 256; ev = []
    def opstate(slots):
        return tuple((reg[0x20 + SLOT[s]], reg[0x40 + SLOT[s]], reg[0x60 + SLOT[s]], reg[0x80 + SLOT[s]], reg[0xE0 + SLOT[s]]) for s in slots)
    for t, r, v in stream:
        old = reg[r]; reg[r] = v
        if 0xB0 <= r <= 0xB8 and (v & 0x20) and not (old & 0x20) and not (reg[0xBD] & 0x20 and r >= 0xB6):
            ch = r - 0xB0          # (in rhythm mode the key bits of channels 6-8 do nothing)
            ev.append((t, f"ch{ch}", (reg[0xA0 + ch], v & 0x1F), opstate(CH_OP[ch]), reg[0xC0 + ch]))
        if r == 0xBD:
            for bit, (name, fch, slots) in RHY.items():
                if (v >> bit) & 1 and not (old >> bit) & 1:
                    ev.append((t, name, (reg[0xA0 + fch], reg[0xB0 + fch] & 0x1F), opstate(slots), reg[0xC0 + 6] if name == 'BD' else None))
    return ev

def main():
    dro, mine = read_dro(sys.argv[1]), read_mine(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 49716)
    a, b = events(dro), events(mine)
    print(f"key-on events: dosbox {len(a)}, mine {len(b)}")
    n = min(len(a), len(b)); bad = 0; ta0, tb0 = a[0][0], b[0][0]
    for i in range(n):
        ea, eb = a[i], b[i]
        if ea[1:] != eb[1:]:
            bad += 1
            if bad <= 12:
                print(f"#{i} @ dosbox {ea[0]-ta0} ms / mine {eb[0]-tb0} ms")
                print("   dosbox", ea[1:]); print("   mine  ", eb[1:])
    if n:
        print(f"{n - bad} of {n} events identical in channel, note, operator and feedback registers; {bad} differ")
        print(f"timing drift over the stretch: dosbox {a[n-1][0]-ta0} ms vs mine {b[n-1][0]-tb0} ms")
    if len(a) != len(b):
        print("length mismatch: extra events in", "dosbox" if len(a) > len(b) else "mine", ":", (a if len(a) > len(b) else b)[n:n+4])

main()
