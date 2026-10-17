"""compare key-on event sequences from a DOSBox .dro and a regs.txt: best time offset (ms), then matches within +-tol ms
usage: evcmp.py dosbox.dro mine.txt [tol_ms]"""
import sys
sys.argv, args = ['x'], sys.argv[1:]
exec(open('host/oplcmp.py').read().split('def main')[0])
dro, mine = read_dro(args[0]), read_mine(args[1], 49716); tol = float(args[2]) if len(args) > 2 else 25
a = [e for e in events(dro) if e[0] >= 2]; b = events(mine)
print(f"dosbox key-ons {len(a)} over {a[-1][0]-a[0][0]} ms; mine {len(b)} over {b[-1][0]-b[0][0]} ms")
# candidate offsets: every pairing of the first 40 dosbox events with the first 40 of mine
from collections import Counter
sig = lambda e: e[1:]
cands = Counter()
for x in a[:60]:
    for y in b[:60]:
        if sig(x) == sig(y): cands[round(x[0] - y[0])] += 1
if not cands: print("no common events at all"); sys.exit(1)
off = cands.most_common(1)[0][0]
print(f"best offset: dosbox = mine + {off} ms (votes {cands.most_common(3)})")
# match greedily in time order
bi = 0; ok = miss = 0; first_bad = []
used = [False] * len(b)
for x in a:
    t = x[0] - off
    hit = None
    for j in range(len(b)):
        if not used[j] and abs(b[j][0] - t) <= tol and sig(b[j]) == sig(x): hit = j; break
    if hit is None:
        miss += 1
        if len(first_bad) < 6: first_bad.append((x[0], x[1:]))
    else: used[hit] = True; ok += 1
span_b = [y for y in b if y[0] + off <= a[-1][0] + tol]
unmatched_b = sum(1 for j, y in enumerate(b) if y[0] + off <= a[-1][0] and not used[j] and y[0] + off >= a[0][0])
print(f"dosbox events matched {ok} of {len(a)} ({miss} unmatched); mine unmatched inside the capture window: {unmatched_b}")
for t, s in first_bad: print("  dosbox unmatched @", t, "ms:", s)
