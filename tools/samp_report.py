#!/usr/bin/env python3
"""Aggregate the loader's profile windows by function.

usage: samp_report.py LOG KOTOR_SYMS PORT_SYMS FIRST_WINDOW LAST_WINDOW

Reads both profilers:
  Profile=1  [samp] lines: call sites and callees (sampler.c)
  Profile=3  [pcs]  lines: program counters, return addresses and the call
             sites found on the game thread's stack (pcsamp.c)

For Profile=3 it also prints an inclusive table: the share of samples taken
anywhere inside a function or the functions it called. That is the table
that says which engine system the frame belongs to.

The two symbol files are `arm-vita-eabi-nm -D -C --defined-only` of the
libKOTOR.so and libandroid_port.so the log was recorded with. An offset is
put down to the nearest exported symbol at or below it, so a static function
is folded into the exported one before it.
"""
import re, sys, bisect, collections


def load(p):
    L = []
    for l in open(p, errors='replace'):
        m = re.match(r'^([0-9a-f]+) (?:[0-9a-f]+ )?[TtWw] (.*)$', l.strip())
        if m:
            L.append((int(m.group(1), 16) & ~1, m.group(2)))
    L.sort()
    return [a for a, _ in L], [n for _, n in L]


K = load(sys.argv[2])
Pt = load(sys.argv[3])
lo, hi = int(sys.argv[4]), int(sys.argv[5])


def sym(tag):
    if len(tag) < 3 or tag[1] != '+':
        return tag
    a = int(tag[2:], 16)
    T = K if tag[0] == 'k' else Pt
    i = bisect.bisect_right(T[0], a) - 1
    return ('' if tag[0] == 'k' else 'port:') + (T[1][i] if i >= 0 else '?')


win = 0
tot = 0
fn = collections.Counter()       # by function: call site (samp) or PC (pcs)
cal = collections.Counter()      # callee (samp) or return address (pcs)
hot = collections.Counter()      # raw PC buckets, for the disassembly
stk = collections.Counter()      # call sites on the stack, by function (pcs)
listed = 0
kind = None
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'\[(samp|pcs)\] window (\d+): (?:area .*?; )?(\d+) samples', l)
    if m:
        kind = m.group(1)
        win = int(m.group(2))
        if lo <= win <= hi:
            tot += int(m.group(3))
        continue
    m = re.search(r'\[(?:samp|pcs)\]   (site|callee|pc|lr|stk) (.*)$', l)
    if m and lo <= win <= hi:
        for tag, n in re.findall(r'(\S+):(\d+)', m.group(2)):
            n = int(n)
            if m.group(1) == 'stk':
                stk[sym(tag)] += n
            elif m.group(1) in ('site', 'pc'):
                fn[sym(tag)] += n
                listed += n
                if m.group(1) == 'pc':
                    hot[tag] += n
            else:
                cal[sym(tag)] += n

if not tot:
    sys.exit('no samples in windows %d-%d' % (lo, hi))
print('samples', tot, 'listed', listed, '(%.0f%%)' % (100 * listed / tot))
first = 'PC' if kind == 'pcs' else 'call site'
second = 'return address' if kind == 'pcs' else 'callee (the call that took the sample)'
print('--- by function containing the', first)
for k, v in fn.most_common(45):
    print('%5.1f%%  %s' % (100 * v / tot, k[:100]))
print('--- by function of the', second)
for k, v in cal.most_common(30):
    print('%5.1f%%  %s' % (100 * v / tot, k[:100]))
if stk:
    incl = collections.Counter(fn)
    incl.update(stk)
    print('--- inclusive: samples inside the function or anything it called')
    print('    (stale words on the stack can add a little; recursion counts twice)')
    for k, v in incl.most_common(60):
        print('%5.1f%%  self %5.1f%%  %s' % (min(100.0, 100 * v / tot), 100 * fn[k] / tot, k[:90]))
if hot:
    print('--- hottest instructions (k+ libKOTOR, p+ port offset)')
    for k, v in hot.most_common(25):
        print('%5.1f%%  %-10s %s' % (100 * v / tot, k, sym(k)[:80]))
