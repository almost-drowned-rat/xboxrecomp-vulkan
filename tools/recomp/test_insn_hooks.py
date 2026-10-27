"""Per-game instruction hooks (--hooks, hooks_config.py) and --weak-functions."""
import pytest

from tools.recomp import config, hooks_config, translator

BASE = 0x10000
# mov eax, 1 ; mov ecx, 2 ; add eax, ecx ; ret 8
IMAGE = bytes.fromhex("b801000000" "b902000000" "01c8" "c20800")
MOV_ECX = BASE + 5
ADD = BASE + 10


def _translate(monkeypatch, hooks):
    config._install([config.Section(".text", BASE, len(IMAGE), 0, len(IMAGE), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="insn-hooks")
    monkeypatch.setattr(translator, "INSN_HOOKS", hooks_config.parse(hooks))
    db = {BASE: {"end": BASE + len(IMAGE), "size": len(IMAGE)}}
    return translator.FunctionTranslator(IMAGE, db).translate_function(BASE, db[BASE])


def test_parse_rejects_bad_entries():
    with pytest.raises(ValueError):
        hooks_config.parse('[[hook]]\nat = 0x10\ncall = "not a name"\n')
    with pytest.raises(ValueError):
        hooks_config.parse('[[hook]]\nat = 0x10\ncall = "f"\nbogus = 1\n')
    h = hooks_config.parse('[[hook]]\nat = 0x10\ncall = "f"\non_true = "ret"\n')
    assert h[0x10][0]["on_true"] == "ret" and hooks_config.returns(h[0x10][0])


def test_before_hook_runs_ahead_of_its_instruction(monkeypatch):
    code = _translate(monkeypatch, f'[[hook]]\nat = 0x{MOV_ECX:X}\ncall = "fix"\n')
    assert code.count("fix();") == 1
    assert code.index("eax = 1;") < code.index("fix();") < code.index("ecx = 2;")
    assert f"loc_{MOV_ECX:08X}:" in code


def test_after_hook_runs_behind_its_instruction(monkeypatch):
    code = _translate(monkeypatch, f'[[hook]]\nat = 0x{MOV_ECX:X}\ncall = "fix"\nafter = true\n')
    assert code.index("ecx = 2;") < code.index("fix();")
    assert f"loc_{ADD:08X}:" in code


def test_returning_hook_leaves_like_the_ret_or_jumps(monkeypatch):
    code = _translate(monkeypatch, f'[[hook]]\nat = 0x{MOV_ECX:X}\ncall = "skip"\n'
                                   f'on_true = "ret"\non_false = 0x{ADD:X}\n')
    line = next(ln for ln in code.splitlines() if "skip()" in ln)
    assert "extern int skip(void);" in line
    assert "if (hook_) { esp += 12; return; }" in line          # ret 8
    assert f"if (!hook_) {{ goto loc_{ADD:08X}; }}" in line
    assert f"loc_{ADD:08X}:" in code


def test_after_a_ret_is_refused(monkeypatch):
    with pytest.raises(ValueError):
        _translate(monkeypatch, f'[[hook]]\nat = 0x{BASE + 12:X}\ncall = "f"\nafter = true\n')


def test_weak_function_is_overridden_at_link_time(monkeypatch, tmp_path):
    """The generated sub_X becomes a weak alias: alone it runs the generated
    body; linked with a strong sub_X, that one runs and can call sub_X_gen."""
    import shutil
    import subprocess
    cc = shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    name = f"sub_{BASE:08X}"
    code = translator.weaken(name, f"/* body */\nvoid {name}(void)\n{{\n    g = 1;\n}}\n")
    assert f"void {name}_gen(void)" in code and f"RECOMP_WEAK_ALIAS({name}, {name}_gen)" in code
    hdr = """#define RECOMP_WEAK_ALIAS(name, target) \\
    extern void name(void) __attribute__((weak, alias(#target)));
"""
    gen = tmp_path / "gen.c"
    gen.write_text("int g;\nvoid %s(void);\n%s%s\nvoid caller(void) { %s(); }\n" % (name, hdr, code, name))
    main = tmp_path / "main.c"
    main.write_text("#include <stdio.h>\nextern int g; void caller(void);\n"
                    "int main(void) { caller(); printf(\"%d\\n\", g); return 0; }\n")
    man = tmp_path / "man.c"
    man.write_text("extern int g; void %s_gen(void);\nvoid %s(void) { %s_gen(); g += 10; }\n"
                   % (name, name, name))
    exe = str(tmp_path / "t")
    for srcs, want in (([gen, main], "1"), ([gen, main, man], "11")):
        built = subprocess.run([cc, "-O2", *map(str, srcs), "-o", exe], capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([exe], capture_output=True, text=True)
        assert ran.stdout.strip() == want
