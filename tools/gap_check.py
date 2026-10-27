#!/usr/bin/env python3
"""gap_check.py - lifter coverage check: code the game reaches that has no lifted function.

    py -3 -I tools/gap_check.py <default.xbe> <functions.json> <gen dir> [--seeds out.json] [--report out.txt]

The recompiler turns every direct call/jump to an address that is not a detected
function into a stub in gen/recomp_stubs_unresolved.c. A stub only pops the return
address (and `ret N` bytes), so the call is silently a no-op -- the game keeps
running with whatever the callee should have computed missing. This tool explains
every stub and finds the functions behind them:

  gap       target lies in bytes no function covers. The function it belongs to was
            never detected (frameless library code without a prologue, only called
            through a table, ...). The enclosing gap function start -- first byte
            after padding/ret, decoding cleanly up to a ret -- is written as a seed.
  inside    target lies inside a detected body: a mid-function entry the alias
            mechanism should have handled -- usually the body ends too early or
            a bogus alias split the real function. Reported, not seeded.
  outside   target is not in any section: the caller is data decoded as code.
            The caller is reported (a bogus function to look at), never seeded.

It also lists every gap function start in code sections (same strict test as
tools/scan_table_seeds.py: 16-byte aligned, after 0x90/0xCC padding that follows
a ret or jmp, decoding to a ret) whether or
not a stub points into it; with --all-gaps those are seeded too. Code sections:
executable flag in the XBE section header, minus the data sections. Seeds use the --seed-functions format.
Generic: no title-specific addresses.
"""
import argparse
import bisect
import collections
import glob
import json
import os
import re
import struct
import sys

import capstone

PAD = (0x90, 0xCC)
# same list as tools/disasm/loader.py: Xbox linkers mark data sections executable too
DATA_SECTIONS = {".data", ".data1", ".rdata", ".idata", ".edata", ".reloc", ".tls"}



def sections(d):
    base = struct.unpack_from('<I', d, 0x104)[0]
    count = struct.unpack_from('<I', d, 0x11C)[0]
    hdr = struct.unpack_from('<I', d, 0x120)[0] - base
    out = []
    for i in range(count):
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<6I', d, hdr + i * 56)
        n = name_va - base
        name = d[n:d.index(b'\0', n)].decode('ascii', 'replace')
        out.append({'name': name, 'va': va, 'vsize': vsize, 'raw': raw, 'rsize': rsize,
                    'exec': bool(flags & 4) and rsize > 0 and name not in DATA_SECTIONS})
    return out


class Image:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        self.secs = sections(self.d)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def sec(self, va):
        for s in self.secs:
            if s['va'] <= va < s['va'] + min(s['vsize'], s['rsize']):
                return s
        return None

    def byte(self, va):
        s = self.sec(va)
        return self.d[s['raw'] + va - s['va']] if s else None

    def code(self, va, n):
        s = self.sec(va)
        end = s['va'] + min(s['vsize'], s['rsize'])
        n = min(n, end - va)
        return self.d[s['raw'] + va - s['va']:s['raw'] + va - s['va'] + n]

    def after_terminator(self, va):
        """Padding before va (0x90/0xCC, < 16 bytes) follows a ret or jmp."""
        p = va - 1
        while va - p < 16 and self.byte(p) in PAD:
            p -= 1
        if va - p >= 16:
            return False
        tail = self.code(p - 4, 5)
        return (tail[-1] == 0xC3 or tail[-3] == 0xC2 or tail[0] == 0xE9
                or tail[-2] == 0xEB)

    def decodes_to_ret(self, va, limit=0x4000):
        """Linear decode from va: True if it reaches a ret/jmp before invalid bytes."""
        s = self.sec(va)
        if not s:
            return False
        pos = va
        end = min(va + limit, s['va'] + min(s['vsize'], s['rsize']))
        while pos < end:
            ins = next(self.md.disasm(self.code(pos, 16), pos), None)
            if ins is None:
                return False
            if ins.mnemonic in ('ret', 'retf', 'jmp'):
                return True
            if ins.mnemonic in ('int3', 'hlt') or ins.bytes == b'\0\0':
                return False
            pos += ins.size
        return False


def load_functions(path):
    fs = json.load(open(path))
    if isinstance(fs, dict):
        fs = list(fs.values())
    bodies = []
    for f in fs:
        bodies.append((int(f['start'], 16), int(f['end'], 16), f.get('detection_method', '')))
    bodies.sort()
    return bodies


def coverage(bodies):
    """Merged covered intervals of all bodies."""
    merged = []
    for s, e, _ in bodies:
        if merged and s <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], e)
        else:
            merged.append([s, e])
    return merged


def gap_starts(img, merged):
    """Strict gap function starts in executable sections: aligned, after padding/ret."""
    out = []
    starts = [m[0] for m in merged]
    for s in img.secs:
        if not s['exec']:
            continue
        lo, hi = s['va'], s['va'] + min(s['vsize'], s['rsize'])
        # walk uncovered ranges of this section
        cur = lo
        k = bisect.bisect_right(starts, cur) - 1
        ranges = []
        for a, b in merged[max(k, 0):]:
            if a >= hi:
                break
            if b <= cur:
                continue
            if a > cur:
                ranges.append((cur, min(a, hi)))
            cur = max(cur, b)
        if cur < hi:
            ranges.append((cur, hi))
        for a, b in ranges:
            va = (a + 15) & ~15
            while va < b:
                bt = img.byte(va)
                prev = img.byte(va - 1)
                if bt not in PAD and bt != 0 and prev in PAD and img.after_terminator(va) \
                        and img.decodes_to_ret(va):
                    out.append(va)
                va += 16
    return sorted(set(out))


def stubs(gen):
    text = open(os.path.join(gen, 'recomp_stubs_unresolved.c'), errors='replace').read()
    return [int(m, 16) for m in re.findall(r'void sub_([0-9A-F]{8})\(void\) \{', text)]


def callers(gen, wanted):
    """stub -> set of lifted functions whose body calls/jumps to it."""
    names = {f'sub_{a:08X}': a for a in wanted}
    out = collections.defaultdict(set)
    fn_re = re.compile(r'^void (sub_[0-9A-F]{8})\(void\)')
    call_re = re.compile(r'\b(sub_[0-9A-F]{8})\(\)')
    for path in sorted(glob.glob(os.path.join(gen, 'recomp_[0-9]*.c'))):
        cur = None
        with open(path, errors='replace') as fh:
            for line in fh:
                m = fn_re.match(line)
                if m:
                    cur = m.group(1)
                    continue
                if 'sub_' not in line:
                    continue
                for c in call_re.findall(line):
                    if c in names and cur:
                        out[names[c]].add(int(cur[4:], 16))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('xbe')
    ap.add_argument('functions')
    ap.add_argument('gen')
    ap.add_argument('--seeds', help='write gap seeds (--seed-functions format)')
    ap.add_argument('--report', help='write the full report here (default: stdout summary only)')
    ap.add_argument('--all-gaps', action='store_true', help='seed every strict gap start, not only those behind stubs')
    a = ap.parse_args()

    img = Image(a.xbe)
    bodies = load_functions(a.functions)
    merged = coverage(bodies)
    mstarts = [m[0] for m in merged]
    gaps = gap_starts(img, merged)
    st = stubs(a.gen)
    who = callers(a.gen, st)

    def covered(v):
        k = bisect.bisect_right(mstarts, v) - 1
        return k >= 0 and v < merged[k][1]

    rows, seeds = [], {}
    kind_count = collections.Counter()
    for v in st:
        s = img.sec(v)
        by = ' '.join(f'{c:08X}' for c in sorted(who.get(v, ())))
        if s is None:
            kind, note = 'outside', 'caller decodes data as code'
        elif covered(v):
            kind, note = 'inside', 'mid-function entry in a detected body'
        else:
            k = bisect.bisect_right(gaps, v) - 1
            g = gaps[k] if k >= 0 else None
            # the gap start must lie in the same uncovered run as the target
            if g is not None and not any(covered(x) for x in range(g, v, 4)) and s is img.sec(g):
                kind, note = 'gap', f'function start 0x{g:08X}'
                seeds.setdefault(g, []).append(v)
            else:
                kind, note = 'gap?', 'no strict function start before it'
        kind_count[kind] += 1
        rows.append(f'{v:08X} {s["name"] if s else "-":9s} {kind:7s} {note}; called by {by or "-"}')

    if a.all_gaps:
        for g in gaps:
            seeds.setdefault(g, [])

    out = [f'stubs: {len(st)}  ' + '  '.join(f'{k}={n}' for k, n in sorted(kind_count.items())),
           f'strict gap function starts in code sections: {len(gaps)}',
           f'seeds: {len(seeds)}']
    print('\n'.join(out))
    if a.report:
        with open(a.report, 'w') as fh:
            fh.write('\n'.join(out) + '\n\n' + '\n'.join(rows) + '\n\ngap starts:\n'
                     + '\n'.join(f'{g:08X}' for g in gaps) + '\n')
    if a.seeds:
        js = [{'start': f'0x{g:08X}',
               'source': ('gap check: undetected function in a code gap'
                          + (', stub targets ' + ' '.join(f'0x{t:08X}' for t in sorted(set(ts))) if ts else '')
                          + ' (xboxrecomp tools/gap_check.py)')}
              for g, ts in sorted(seeds.items())]
        with open(a.seeds, 'w') as fh:
            json.dump(js, fh, indent=1)
            fh.write('\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
