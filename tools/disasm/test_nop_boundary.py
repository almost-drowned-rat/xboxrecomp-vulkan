"""Self-check for the NOP-padding function boundary and the alias closure.

Run: py -3 -m pytest tools/disasm/test_nop_boundary.py

Early-XDK MSVC (THPS 2X, XDK 3947) pads to the next function with 0x90 and
multi-byte NOPs instead of int3. D3D 0x00013DCC in THPS 2X:

    0x00013DCC  ret 0x10
    0x00013DCF  nop                 <- padding to the 16-byte boundary
    0x00013DD0  mov eax, [0x28030]  <- frameless D3D function, no prologue

Without a boundary there, tail-jump aliases with wrong starts took the place of
such functions, and their loop branches back into the host became no-op stubs.
"""
import os
import sys
import types

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402

BASE = 0x00010000


class _Insn:
    def __init__(self, addr, size, is_ret=False, cond_to=None, jmp_to=None):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.is_ret = is_ret
        self.is_cond_jump = cond_to is not None
        self.is_jump = jmp_to is not None
        self.jump_target = cond_to if cond_to is not None else jmp_to

    @property
    def is_branch(self):
        return self.is_jump or self.is_cond_jump


class _Engine:
    def __init__(self, insns):
        self.instructions = {i.address: i for i in insns}
        self.tables = {}

    def jump_table_entries(self, tbl):
        return self.tables.get(tbl, [])

    def get_instructions_in_range(self, start, end):
        return [self.instructions[a] for a in sorted(self.instructions) if start <= a < end]


def _detect(data, insns):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.image = types.SimpleNamespace(get_section_data=lambda sec: bytes(data))
    det._candidates = {}
    found = []
    det._add_candidate = lambda addr, conf, why: found.append(addr)
    det._pass_nop_boundaries([types.SimpleNamespace(virtual_addr=BASE)])
    return found


def _layout(pad):
    """ret 0x10 at 0x2A, <pad> from 0x2D, next function right after it."""
    data = bytearray(b"\x55" * 0x80)
    data[0x2A:0x2D] = b"\xC2\x10\x00"
    data[0x2D:0x2D + len(pad)] = pad
    return data, 0x2D + len(pad)


def test_nop_padding_after_ret_starts_a_function():
    data, nxt = _layout(b"\x90" * 3)
    found = _detect(data, [_Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)])
    assert BASE + nxt in found


def test_multibyte_nop_padding_starts_a_function():
    for pad in (b"\x8D\x49\x00", b"\x8B\xFF\x90"):
        data, nxt = _layout(pad)
        found = _detect(data, [_Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)])
        assert nxt == 0x30 and BASE + nxt in found, pad


def test_block_after_early_return_is_not_a_function():
    # A conditional branch from before the ret into the code after it: same function.
    data, nxt = _layout(b"\x90" * 3)
    insns = [_Insn(BASE + 0x10, 2, cond_to=BASE + nxt + 4),
             _Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)]
    assert BASE + nxt not in _detect(data, insns)


def test_tail_jump_across_does_not_block_the_boundary():
    # An unconditional jmp to a function-start-like address past the boundary is a
    # tail call, not a local block.
    data, nxt = _layout(b"\x90" * 3)
    insns = [_Insn(BASE + 0x10, 5, jmp_to=BASE + nxt + 0x10),
             _Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)]
    assert BASE + nxt in _detect(data, insns)


def test_padding_not_reaching_alignment_is_not_a_boundary():
    data, nxt = _layout(b"\x90")           # code at 0x2E, not 16-aligned
    insns = [_Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)]
    assert _detect(data, insns) == []


def test_alias_closure_adds_entry_for_branch_back_into_host():
    det = FunctionDetector.__new__(FunctionDetector)
    host = types.SimpleNamespace(start=BASE, end=BASE + 0x60)
    alias = types.SimpleNamespace(start=BASE + 0x40, end=BASE + 0x60)
    det.functions = {host.start: host, alias.start: alias}
    det._alias_entries = {alias.start: alias.end}
    det.engine = _Engine([_Insn(BASE + 0x20, 2), _Insn(BASE + 0x40, 2),
                          _Insn(BASE + 0x50, 2, cond_to=BASE + 0x20)])
    det.image = types.SimpleNamespace(get_section_at_va=lambda va: None)
    assert det._pass_alias_closure()
    assert det._alias_entries[BASE + 0x20] == BASE + 0x60
    assert not det._pass_alias_closure()


def test_tail_call_jmp_before_padding_starts_a_function():
    data, nxt = _layout(b"\x90" * 3)
    insns = [_Insn(BASE + 0x2A, 3, jmp_to=BASE + 0x1000), _Insn(BASE + nxt, 4)]
    assert BASE + nxt in _detect(data, insns)


def test_jmp_to_loop_condition_after_aligned_head_is_not_a_function():
    data, nxt = _layout(b"\x90" * 3)
    insns = [_Insn(BASE + 0x2A, 3, jmp_to=BASE + nxt + 0x20), _Insn(BASE + nxt, 4)]
    assert BASE + nxt not in _detect(data, insns)


def test_padding_longer_than_sixteen_bytes_is_a_boundary():
    data, nxt = _layout(b"\x90" * 0x13)
    insns = [_Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)]
    assert nxt == 0x40 and BASE + nxt in _detect(data, insns)


def test_alias_closure_adds_gap_entry_after_a_body_cut_short():
    # SEH function cut at its __finally block: the error path after it is a gap.
    det = FunctionDetector.__new__(FunctionDetector)
    fn = types.SimpleNamespace(start=BASE, end=BASE + 0x40)
    nxt = types.SimpleNamespace(start=BASE + 0x80, end=BASE + 0x90)
    det.functions = {fn.start: fn, nxt.start: nxt}
    det._alias_entries = {}
    det.engine = _Engine([_Insn(BASE + 0x10, 2, cond_to=BASE + 0x50), _Insn(BASE + 0x50, 2)])
    det.image = types.SimpleNamespace(
        get_section_at_va=lambda va: types.SimpleNamespace(executable=True))
    assert det._pass_alias_closure()
    assert det._alias_entries[BASE + 0x50] == BASE + 0x80


def _detect_with_table(data, insns, table):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.engine.tables[BASE + 0x70] = table
    det.image = types.SimpleNamespace(get_section_data=lambda sec: bytes(data))
    det._candidates = {}
    found = []
    det._add_candidate = lambda addr, conf, why: found.append(addr)
    det._pass_nop_boundaries([types.SimpleNamespace(virtual_addr=BASE)])
    return found


def test_switch_arm_after_table_jump_is_not_a_function():
    # CRT memmove: `jmp [edx*4 + table]`, nop, aligned arm (THPS 2X 0x001DE030).
    data, nxt = _layout(b"\x90" * 3)
    sw = _Insn(BASE + 0x2A, 3)
    sw.is_jump = True
    sw.jump_table = BASE + 0x70
    assert _detect_with_table(data, [sw, _Insn(BASE + nxt, 4)], [BASE + nxt]) == []


def test_ret_before_switch_arm_is_not_a_boundary():
    data, nxt = _layout(b"\x90" * 3)
    sw = _Insn(BASE + 0x10, 7)
    sw.jump_table = BASE + 0x70
    insns = [sw, _Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)]
    assert _detect_with_table(data, insns, [BASE + nxt + 8]) == []


def test_jmp_to_block_after_early_return_is_not_a_function():
    # `jmp L` from the body over the ret to an unaligned block: same function.
    data, nxt = _layout(b"\x90" * 3)
    insns = [_Insn(BASE + 0x10, 5, jmp_to=BASE + nxt + 4),
             _Insn(BASE + 0x2A, 3, is_ret=True), _Insn(BASE + nxt, 4)]
    assert BASE + nxt not in _detect(data, insns)
