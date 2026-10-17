"""find a song inside a long DOSBox capture: best time offset, then exact-compare within the song's span"""
import sys; song, dro_path, mine_path, rate = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
sys.argv=['x']; exec(open('host/oplcmp.py').read().split('def main')[0])
dro=read_dro(dro_path); mine=read_mine(mine_path,49716)
a=[e for e in events(dro) if e[0]>=2]; b=events(mine); ROW=1000/rate
ta0=a[0][0]; tb0=b[0][0]
ra={}; rb={}
for t,ch,n,o,c in a: ra.setdefault(round((t-ta0)/ROW),{})[ch]=(n,o,c)
for t,ch,n,o,c in b: rb.setdefault(round((t-tb0)/ROW),{})[ch]=(n,o,c)
nb=max(rb)+1
def score(off):   # dosbox row r <-> mine row r-off
    s=0
    for r in range(off, off+nb):
        if r in ra and r-off in rb:
            s+=len({(ch,v[0]) for ch,v in ra[r].items()} & {(ch,v[0]) for ch,v in rb[r-off].items()})
    return s
best=max(range(0,max(ra)+1), key=score)
exact=tot=0; bad=[]
last=max(ra)
for r in range(best+1, min(best+nb, last-1)):      # inside the capture window only
    if r not in ra and r-best not in rb: continue
    A=ra.get(r,{}); B=rb.get(r-best,{})
    for ch in A.keys()|B.keys():
        tot+=1
        if A.get(ch)==B.get(ch): exact+=1
        else: bad.append((r-best,ch,A.get(ch),B.get(ch)))
print(f"{song}: starts at dosbox row {best} ({best*ROW/1000:.1f} s into the capture); events {exact} exact of {tot}; {len(bad)} differ")
for r,ch,x,y in bad[:8]: print(" song row",r,ch,"\n   dosbox",x,"\n   mine  ",y)
