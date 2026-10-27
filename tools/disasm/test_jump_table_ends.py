"""A function that starts on the byte after an embedded jump table.

THPS 2X's HANDICAP "ready" handler at 0x00115300 follows the switch table at
0x001152F0 and is reached only through a vtable. No ret within 64
instructions, so the data-pointer pass rejected it, and nothing else looks at
that address. _pass_jump_table_ends makes it a candidate when there is
evidence of a start (prologue, data word, immediate, direct call) and no sign
that it is the table owner's own code (a table arm, a conditional-branch
target).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402

TBL, END = 0x001152F0, 0x00115300


class _Insn:
    def __init__(self, addr, mnemonic="push", cond_target=None, call_target=None):
        self.address = addr
        self.mnemonic = mnemonic
        self.is_cond_jump = cond_target is not None
        self.jump_target = cond_target
        self.call_target = call_target


class _Section:
    def __init__(self, name, va, data=b""):
        self.name = name
        self.virtual_addr = va
        self.virtual_size = max(len(data), 0x100000)
        self.data = data


class _Image:
    def __init__(self, rdata):
        self.text = _Section(".text", 0x00100000)
        self.sections = [self.text, _Section(".rdata", 0x00220000, rdata)]

    def get_code_sections(self):
        return [self.text]

    def get_section_data(self, sec):
        return sec.data


class _Engine:
    def __init__(self, insns, prologues=(), arms=()):
        self.instructions = {i.address: i for i in insns}
        self.jump_tables = {TBL: END}
        self._prologues = set(prologues)
        self._arms = list(arms)

    def jump_table_entries(self, tbl):
        return self._arms

    def probes_as_prologue(self, addr):
        return addr in self._prologues


def _detector(insns, prologues=(), arms=(), rdata=b""):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, prologues, arms)
    det.image = _Image(rdata)
    det.xrefs = None
    det._candidates = {}
    return det


class JumpTableEndTest(unittest.TestCase):
    def test_vtable_reached_start_after_a_table_is_a_function(self):
        # push ecx: no prologue the probe knows, but a vtable slot names it.
        det = _detector([_Insn(END, "push")], rdata=END.to_bytes(4, "little"))
        self.assertEqual(det._pass_jump_table_ends(), 1)
        self.assertEqual(det._candidates[END][1], "after_jump_table")

    def test_prologue_alone_is_enough(self):
        det = _detector([_Insn(END, "push")], prologues={END})
        self.assertEqual(det._pass_jump_table_ends(), 1)

    def test_called_start_is_a_function(self):
        det = _detector([_Insn(END, "push"), _Insn(0x00100000, "call", call_target=END)])
        self.assertEqual(det._pass_jump_table_ends(), 1)

    def test_no_evidence_no_function(self):
        det = _detector([_Insn(END, "mov")])
        self.assertEqual(det._pass_jump_table_ends(), 0)

    def test_an_arm_of_the_table_is_not_split_off(self):
        det = _detector([_Insn(END, "push")], prologues={END}, arms=[END])
        self.assertEqual(det._pass_jump_table_ends(), 0)

    def test_a_conditional_branch_target_is_not_split_off(self):
        det = _detector([_Insn(END, "push"), _Insn(0x00115200, "jne", cond_target=END)],
                        prologues={END})
        self.assertEqual(det._pass_jump_table_ends(), 0)

    def test_padding_is_not_a_function(self):
        det = _detector([_Insn(END, "int3")], prologues={END})
        self.assertEqual(det._pass_jump_table_ends(), 0)


if __name__ == "__main__":
    unittest.main()
