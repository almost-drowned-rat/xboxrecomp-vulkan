"""Replay one call of a title's function in a CPU emulator and compare it with
what the lifted code did in the running game.

    py -3 -m tools.conformance.replay ENTRY_VA before.bin after.bin

The game writes the two snapshots (xbox_StateSnapshot in the kernel runtime):
the guest state right before a lifted function ran, and right after it
returned. This tool loads the *before* state into Unicorn (QEMU's CPU core:
exact integer, MMX and x87 semantics), runs the title's original machine code
from ENTRY_VA until it returns to the address on top of the stack, and
compares the result with *after*:

  lifted wrong   the original code wrote the byte and the lifted code left a
                 different value -- a lifting error, with its address
  extra write    only the lifted run changed the byte: another guest thread
                 between the two snapshots (audio, ...) or a stray write

This is the oracle for code the snippet and --xbe phases cannot reach: a
whole decoder working on real data, with all its callees.

Needs `pip install unicorn`. The emulated code may not leave the snapshot:
a call into the kernel or a read of unmapped memory stops the run with its
address.
"""

import argparse
import struct
import sys

PAGE = 0x1000


def _load(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"XSNP" or struct.unpack_from("<I", d, 4)[0] != 1:
        raise SystemExit(f"{path}: not an XSNP v1 snapshot")
    o = 8
    regs = struct.unpack_from("<9I", d, o); o += 36
    fp_top, fp_cw = struct.unpack_from("<iI", d, o); o += 8
    fp = struct.unpack_from("<8d", d, o); o += 64
    mm = struct.unpack_from("<8Q", d, o); o += 64
    ram, cbase, csize = struct.unpack_from("<3I", d, o); o += 12
    ram_b = memoryview(d)[o:o + ram]; o += ram
    con_b = memoryview(d)[o:o + csize]
    names = ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "fs")
    return {"regs": dict(zip(names, regs)), "fp_top": fp_top, "fp_cw": fp_cw,
            "fp": fp, "mm": mm, "ram": ram_b, "cbase": cbase, "contig": con_b}


def _round(n):
    return (n + PAGE - 1) & ~(PAGE - 1)


def replay(entry, before, max_insns):
    from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_INVALID
    from unicorn import x86_const as X

    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    regions = [(0, before["ram"]), (before["cbase"], before["contig"])]
    for base, blob in regions:
        if len(blob):
            mu.mem_map(base, _round(len(blob)))
            mu.mem_write(base, bytes(blob))
    r = before["regs"]
    for name, reg in (("eax", X.UC_X86_REG_EAX), ("ecx", X.UC_X86_REG_ECX),
                      ("edx", X.UC_X86_REG_EDX), ("ebx", X.UC_X86_REG_EBX),
                      ("esp", X.UC_X86_REG_ESP), ("ebp", X.UC_X86_REG_EBP),
                      ("esi", X.UC_X86_REG_ESI), ("edi", X.UC_X86_REG_EDI)):
        mu.reg_write(reg, r[name])
    mu.reg_write(X.UC_X86_REG_FPCW, before["fp_cw"] or 0x027F)
    ret = struct.unpack_from("<I", before["ram"], r["esp"])[0] \
        if r["esp"] + 4 <= len(before["ram"]) else 0
    bad = []

    def on_invalid(uc, access, addr, size, value, _):
        bad.append((uc.reg_read(X.UC_X86_REG_EIP), addr, access))
        return False

    mu.hook_add(UC_HOOK_MEM_INVALID, on_invalid)
    err = None
    try:
        mu.emu_start(entry, ret, count=max_insns)
    except UcError as e:
        err = str(e)
    eip = mu.reg_read(X.UC_X86_REG_EIP)
    return mu, regions, ret, eip, err, bad, mu.reg_read(X.UC_X86_REG_EAX)


def compare(mu, regions, before, after):
    """[(addr, before, original, lifted)] for 'lifted wrong' and the number of
    'extra write' bytes."""
    wrong, extra = [], 0
    after_map = {0: after["ram"], before["cbase"]: after["contig"]}
    for base, blob in regions:
        if not len(blob):
            continue
        emu = mu.mem_read(base, len(blob))
        aft = after_map.get(base)
        if aft is None:
            continue
        n = min(len(blob), len(aft))
        b, e, a = bytes(blob[:n]), bytes(emu[:n]), bytes(aft[:n])
        # Whole-page skip first: most of guest RAM is untouched by both.
        for p in range(0, n, PAGE):
            q = min(p + PAGE, n)
            if e[p:q] == a[p:q]:
                continue
            for i in range(p, q):
                if e[i] != a[i]:
                    if e[i] != b[i]:
                        wrong.append((base + i, b[i], e[i], a[i]))
                    else:
                        extra += 1
    return wrong, extra


def _ranges(addrs):
    out = []
    for a in addrs:
        if out and a <= out[-1][1] + 16:
            out[-1][1] = a
            out[-1][2] += 1
        else:
            out.append([a, a, 1])
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m tools.conformance.replay",
                                 description=__doc__.splitlines()[0])
    ap.add_argument("entry", help="function VA (hex) the snapshots bracket")
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--max-insns", type=int, default=2_000_000_000)
    ap.add_argument("--show", type=int, default=24, help="differing bytes to list")
    args = ap.parse_args(argv)

    entry = int(args.entry, 16)
    before, after = _load(args.before), _load(args.after)
    if before["fp_top"] not in (0, 8):
        print(f"note: x87 stack not empty at entry (top {before['fp_top']}); "
              "its values are not loaded into the emulator")
    mu, regions, ret, eip, err, bad, eax = replay(entry, before, args.max_insns)
    print(f"entry {entry:08X}, return address {ret:08X}, emulation stopped at {eip:08X}"
          + (f" ({err})" if err else ""))
    for ip, addr, acc in bad[:5]:
        print(f"  invalid access at eip {ip:08X}: address {addr:08X} (type {acc})")
    if eip != ret:
        print("did not return -- comparison is of a partial run")
    print(f"eax: original {eax:08X}, lifted {after['regs']['eax']:08X}")
    wrong, extra = compare(mu, regions, before, after)
    print(f"lifted wrong: {len(wrong)} byte(s); extra writes (other threads or "
          f"stray): {extra} byte(s)")
    for lo, hi, n in _ranges([w[0] for w in wrong])[:16]:
        print(f"  {lo:08X}..{hi:08X}  {n} byte(s)")
    for a, b, e, l in wrong[:args.show]:
        print(f"    {a:08X}: before {b:02X}  original {e:02X}  lifted {l:02X}")
    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
