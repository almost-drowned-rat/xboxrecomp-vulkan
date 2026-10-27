"""movsx from a 16-bit register sign-extends for every one of them, bp and sp
included.

The 16-bit source list in _lift_movsx stopped at si/di, so `movsx ebp, bp`
lifted to a plain `ebp = LO16(ebp)` -- a zero extension. Bink's 8x8 integer
IDCT in THPS 2X (sub_001D87B0) loads a coefficient with `mov bp, [ebx+0x60]`
and widens it with `movsx ebp, bp`; every negative coefficient became a large
positive one, and the decoded movie frames had wrong pixels along every edge.
Found by replaying the decoder against the original code
(tools/conformance/replay.py). movzx always had bp/sp.
"""
import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _lift(mnemonic, dst, src):
    insn = Instruction(0x001D886D, 3, mnemonic, f"{dst}, {src}", "",
                       operands=[Operand(type="reg", reg=dst),
                                 Operand(type="reg", reg=src)])
    return "\n".join(Lifter().lift_instruction(insn))


class MovsxSixteenBitSourceTest(unittest.TestCase):

    def test_every_16_bit_register_is_sign_extended(self):
        for src in ("ax", "bx", "cx", "dx", "si", "di", "bp", "sp"):
            with self.subTest(src=src):
                self.assertIn("SX16(", _lift("movsx", "eax", src))

    def test_movsx_ebp_bp_is_not_a_bare_low_half(self):
        out = _lift("movsx", "ebp", "bp")
        self.assertIn("SX16(LO16(ebp))", out)
        self.assertNotIn("= LO16(ebp);", out)

    def test_movzx_still_zero_extends(self):
        self.assertIn("ZX16(", _lift("movzx", "ebp", "bp"))


if __name__ == "__main__":
    unittest.main()
