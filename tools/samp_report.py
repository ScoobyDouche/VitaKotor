#!/usr/bin/env python3
"""Aggregate the loader's [samp] profile windows by function.

usage: samp_report.py LOG KOTOR_SYMS PORT_SYMS FIRST_WINDOW LAST_WINDOW
The two symbol files are `arm-vita-eabi-nm -D -C --defined-only` of the
libKOTOR.so and libandroid_port.so the log was recorded with.
"""
import re,sys,bisect,collections
def load(p):
    L=[]
    for l in open(p,errors='replace'):
        m=re.match(r'^([0-9a-f]+) (?:[0-9a-f]+ )?[TtWw] (.*)$',l.strip())
        if m: L.append((int(m.group(1),16)&~1,m.group(2)))
    L.sort(); return [a for a,_ in L],[n for _,n in L]
K=load(sys.argv[2]); Pt=load(sys.argv[3])
lo,hi=int(sys.argv[4]),int(sys.argv[5])
def sym(tag):
    if tag[1]!='+': return tag
    a=int(tag[2:],16); T=K if tag[0]=='k' else Pt
    i=bisect.bisect_right(T[0],a)-1
    return ('' if tag[0]=='k' else 'port:')+(T[1][i] if i>=0 else '?')
win=0; tot=0; site=collections.Counter(); fn=collections.Counter(); cal=collections.Counter(); listed=0
for l in open(sys.argv[1],errors='replace'):
    m=re.search(r'\[samp\] window (\d+): (\d+) samples',l)
    if m:
        win=int(m.group(1))
        if lo<=win<=hi: tot+=int(m.group(2))
        continue
    m=re.search(r'\[samp\]   (site|callee) (.*)$',l)
    if m and lo<=win<=hi:
        for tag,n in re.findall(r'(\S+):(\d+)',m.group(2)):
            n=int(n)
            if m.group(1)=='site': site[tag]+=n; fn[sym(tag)]+=n; listed+=n
            else: cal[sym(tag) if tag[1]=='+' else tag]+=n
print('samples',tot,'listed in top sites',listed,'(%.0f%%)'%(100*listed/tot))
print('--- by function containing the call site')
for k,v in fn.most_common(45): print('%5.1f%%  %s'%(100*v/tot,k[:100]))
print('--- by callee (the call that took the sample)')
for k,v in cal.most_common(30): print('%5.1f%%  %s'%(100*v/tot,k[:100]))
