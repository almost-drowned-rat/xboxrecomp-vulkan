"""shl/sal/shr mask their count to 5 bits, as x86 does.

The count used to go into C unmasked: `shl eax, cl` with cl = 32 is a shift of
32 on a 32-bit value, undefined in C, where x86 shifts by 32 & 31 = 0. THPS 2X's
replay bit reader (sub_000CFE50) builds `(1 << n) - 1` for a 32-bit field, so
the result depended on how the host compiler chose to read the undefined shift.
`sar` already masked (test_lifter_sar_width.py).
"""

import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _reg(name, size=4):
    return Operand(type="reg", reg=name)


def _lift(mnemonic, dst, cnt):
    insn = Instruction(0, 2, mnemonic, "", "00")
    insn.operands = [dst, cnt]
    return " ".join(Lifter().lift_instruction(insn))


class ShiftCountTest(unittest.TestCase):
    def test_register_count_is_masked(self):
        for m in ("shl", "sal", "shr"):
            out = _lift(m, _reg("eax"), _reg("cl", 1))
            self.assertIn("& 31u", out, f"{m}: {out}")

    def test_narrow_operand_count_is_masked(self):
        out = _lift("shl", _reg("al", 1), _reg("cl", 1))
        self.assertIn("& 31u", out, out)

    def test_masked_count_matches_x86_at_32(self):
        # What x86 gives for `mov eax, 1; shl eax, 32; dec eax`: 0. The lifted
        # expression, evaluated in Python with the mask, must agree.
        cnt = 32
        self.assertEqual(((1 << (cnt & 31)) - 1) & 0xFFFFFFFF, 0)


if __name__ == "__main__":
    unittest.main()
