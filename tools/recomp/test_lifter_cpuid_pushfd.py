"""cpuid, pushfd and popfd are translated, not RECOMP_UNIMPL.

All three used to be untranslated no-ops. For cpuid that left EAX..EDX as they
were, so a feature test read garbage; for pushfd/popfd it also left esp four
bytes off per instruction. Bink's MMX probe (THPS 2X, 0x001D7510) is all three:

    pushfd; pop eax; mov ebx, eax; xor eax, 0x200000; push eax; popfd
    pushfd; pop eax; push ebx; popfd; cmp eax, ebx; je no_cpuid
    mov eax, 1; cpuid; test edx, 0x800000          ; MMX

and with every one of them a no-op it took the non-MMX path and left the
blitter function pointers it selects NULL.

cpuid answers as the Xbox Pentium III (xemu's "pentium3" under XBOX).
"""
import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _lift(mnemonic, operands=(), op_str=""):
    insn = Instruction(0x00012340, 1, mnemonic, op_str, "",
                       operands=list(operands))
    lifter = Lifter()
    return lifter, "\n".join(lifter.lift_instruction(insn))


class CpuidTest(unittest.TestCase):

    def test_cpuid_is_translated(self):
        lifter, out = _lift("cpuid")
        self.assertNotIn("RECOMP_UNIMPL", out)
        self.assertNotIn("cpuid", lifter.unimplemented)

    def test_vendor_is_genuine_intel_with_max_leaf_2(self):
        _, out = _lift("cpuid")
        self.assertIn("eax = 2u; ebx = 0x756E6547u; edx = 0x49656E69u;"
                      " ecx = 0x6C65746Eu;", out)

    def test_leaf_1_is_a_pentium_iii_with_mmx_and_sse_but_no_sse2(self):
        _, out = _lift("cpuid")
        self.assertIn("eax = 0x0000068Au", out)      # family 6, model 8, stepping 10
        self.assertIn("edx = RECOMP_CPUID_EDX1;", out)
        edx = 0x0383FBFF                 # the runtime's default for it
        self.assertIn(f"g_recomp_cpuid_edx1 = 0x{edx:08X}u", open(
            __import__("os").path.join(__import__("os").path.dirname(__file__),
                "..", "..", "src", "kernel", "xbox_memory_layout.c"), encoding="utf-8").read())
        self.assertTrue(edx & (1 << 23))   # MMX
        self.assertTrue(edx & (1 << 25))   # SSE
        self.assertFalse(edx & (1 << 26))  # no SSE2

    def test_unsupported_leaves_read_zero(self):
        _, out = _lift("cpuid")
        self.assertIn("else { eax = 0u; ebx = 0u; ecx = 0u; edx = 0u; }", out)


class PushfdPopfdTest(unittest.TestCase):

    def test_pushfd_pushes_and_popfd_pops(self):
        for m, needle in (("pushfd", "PUSH32(esp, RECOMP_EFLAGS_BASE())"),
                          ("popfd", "POP32(esp, _fl)")):
            lifter, out = _lift(m)
            self.assertIn(needle, out)
            self.assertNotIn("RECOMP_UNIMPL", out)
            self.assertNotIn(m, lifter.unimplemented)

    def test_popfd_restores_df_and_the_id_bit(self):
        _, out = _lift("popfd")
        self.assertIn("g_df = (int)((_fl >> 10) & 1u)", out)
        self.assertIn("g_eflags_sys = _fl & 0x00240000u", out)   # AC | ID

    def test_pushfd_after_a_compare_carries_its_flags(self):
        cmp = Instruction(0, 2, "cmp", "eax, ebx", "3bc3")
        cmp.operands = [Operand(type="reg", reg="eax"),
                        Operand(type="reg", reg="ebx")]
        pushfd = Instruction(2, 1, "pushfd", "", "9c")
        pushfd.operands = []
        out = "\n".join(lift_basic_block(
            Lifter(), BasicBlock(start=0, instructions=[cmp, pushfd]))[0])
        self.assertIn("PUSH32(esp, RECOMP_EFLAGS_BASE() | (uint32_t)(", out)
        self.assertIn("? 0x40u", out)   # ZF from the compare
        self.assertIn("? 0x1u", out)    # CF from the compare
        self.assertIn("/* pushfd (cmp) */", out)


if __name__ == "__main__":
    unittest.main()
