"""Per-game instruction hooks (--hooks FILE, TOML).

Design after XboxXBE-Recomp's ``[[hook]]`` (TGP482, MIT, commit 65e959d,
templates/game/config/game.toml): a hand-written function runs at one guest
instruction inside a recompiled function, before it or after it, and may
decide to leave the function or jump elsewhere in it. Game patches (frame
rate, widescreen) and the D3D8 HLE build on this without wrapping whole
functions.

    [[hook]]
    at = 0x0012ABCD        # guest instruction inside a recompiled function
    call = "thps_fix"      # void thps_fix(void), or int thps_fix(void) with on_true/on_false
    after = false          # true: after the instruction (not after a jump or ret)
    on_true = "ret"        # the hook returned nonzero: return as the function's ret does
    on_false = 0x0012AC00  # ... or zero: go to that instruction of the same function

The hook sees the guest registers (g_eax ... g_esp) and memory. It does not see
flags the lifter keeps lazily, so hook at a point where no flag is live (a block
start, not between a cmp and its jcc).
"""
import re

try:
    import tomllib
except ImportError:          # Python < 3.11
    tomllib = None

_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _addr(v, what):
    if isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 0xFFFFFFFF:
        raise ValueError(f"[[hook]] {what} must be a 32-bit address, got {v!r}")
    return v


def parse(text):
    """{address: [hook, ...]} from TOML text. A hook is a dict with keys at,
    call, after, on_true, on_false (None, "ret" or an address)."""
    if tomllib is None:
        raise RuntimeError("--hooks needs Python 3.11+ (tomllib)")
    doc = tomllib.loads(text)
    out = {}
    for h in doc.get("hook", []):
        at = _addr(h.get("at"), "at")
        call = h.get("call")
        if not isinstance(call, str) or not _NAME.match(call):
            raise ValueError(f"[[hook]] at 0x{at:08X}: call must be a C identifier, got {call!r}")
        hook = {"at": at, "call": call, "after": bool(h.get("after", False))}
        for k in ("on_true", "on_false"):
            v = h.get(k)
            if v is None:
                hook[k] = None
            elif v == "ret":
                hook[k] = "ret"
            else:
                hook[k] = _addr(v, k)
        unknown = set(h) - {"at", "call", "after", "on_true", "on_false"}
        if unknown:
            raise ValueError(f"[[hook]] at 0x{at:08X}: unknown key(s) {sorted(unknown)}")
        out.setdefault(at, []).append(hook)
    return out


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return parse(f.read())


def returns(hook):
    return hook["on_true"] is not None or hook["on_false"] is not None
