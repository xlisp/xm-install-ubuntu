#!/usr/bin/env python3
"""Diff rpmh-regulator leaf constraints between two raw DTS dumps."""
import re, sys

def parse(path):
    regs = {}
    cur = None
    with open(path) as f:
        for line in f:
            m = re.match(r"=== (\S+) ===", line)
            if m:
                cur = m.group(1) if "/regulator-" in m.group(1) else None
                if cur:
                    regs.setdefault(cur, {})
                continue
            if cur is None:
                continue
            s = line.strip()
            mm = re.match(r"regulator-name = '(\S+)'", s)
            if mm:
                regs[cur]["name"] = mm.group(1)
                continue
            mm = re.match(r"(\S+) = <(\d+)", s)
            if mm:
                regs[cur][mm.group(1)] = int(mm.group(2))
                continue
            mm = re.match(r"(\S+) = '([^']+)'", s)
            if mm:
                regs[cur][mm.group(1)] = mm.group(2)
    return regs

a, b = parse(sys.argv[1]), parse(sys.argv[2])
keys = sorted(set(a) | set(b), key=lambda k: a.get(k, {}).get("name", b.get(k, {}).get("name", k)))
diffs = 0
for k in keys:
    va, vb = a.get(k), b.get(k)
    if va is None or vb is None:
        print(f"{'MISSING-in-' + ('diting' if va else 'thor')}: {k} name={ (va or vb or {}).get('name')}")
        diffs += 1
        continue
    if va != vb:
        for prop in sorted(set(va) | set(vb)):
            if va.get(prop) != vb.get(prop):
                print(f"{va.get('name', k):18s} {prop:38s} thor={va.get(prop)!r:12} diting={vb.get(prop)!r}")
        diffs += 1
print(f"\n[leaf regulators diff: {diffs} / {len(keys)}]")
