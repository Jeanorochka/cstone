from __future__ import annotations

import argparse
import json
from pathlib import Path
import random
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
if (ROOT / "work" / "oracle-deps").exists():
    sys.path.insert(0, str(ROOT / "work" / "oracle-deps"))

import unicorn as uc
from unicorn import arm_const, arm64_const, mips_const, riscv_const, x86_const


PROFILES = {
    "x86_64": (uc.UC_ARCH_X86, uc.UC_MODE_64, x86_const.UC_X86_REG_RAX, x86_const.UC_X86_REG_RCX, x86_const.UC_X86_REG_RSP, None, 64),
    "x86": (uc.UC_ARCH_X86, uc.UC_MODE_32, x86_const.UC_X86_REG_EAX, x86_const.UC_X86_REG_ECX, x86_const.UC_X86_REG_ESP, None, 32),
    "aarch64": (uc.UC_ARCH_ARM64, uc.UC_MODE_ARM, arm64_const.UC_ARM64_REG_X0, arm64_const.UC_ARM64_REG_X1, arm64_const.UC_ARM64_REG_SP, arm64_const.UC_ARM64_REG_LR, 64),
    "arm": (uc.UC_ARCH_ARM, uc.UC_MODE_ARM, arm_const.UC_ARM_REG_R0, arm_const.UC_ARM_REG_R1, arm_const.UC_ARM_REG_SP, arm_const.UC_ARM_REG_LR, 32),
    "thumb": (uc.UC_ARCH_ARM, uc.UC_MODE_THUMB, arm_const.UC_ARM_REG_R0, arm_const.UC_ARM_REG_R1, arm_const.UC_ARM_REG_SP, arm_const.UC_ARM_REG_LR, 32),
    "riscv32": (uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV32, riscv_const.UC_RISCV_REG_A0, riscv_const.UC_RISCV_REG_A1, riscv_const.UC_RISCV_REG_SP, riscv_const.UC_RISCV_REG_RA, 32),
    "riscv64": (uc.UC_ARCH_RISCV, uc.UC_MODE_RISCV64, riscv_const.UC_RISCV_REG_A0, riscv_const.UC_RISCV_REG_A1, riscv_const.UC_RISCV_REG_SP, riscv_const.UC_RISCV_REG_RA, 64),
    "mips32le": (uc.UC_ARCH_MIPS, uc.UC_MODE_MIPS32 | uc.UC_MODE_LITTLE_ENDIAN, mips_const.UC_MIPS_REG_V0, mips_const.UC_MIPS_REG_A0, mips_const.UC_MIPS_REG_SP, mips_const.UC_MIPS_REG_RA, 32),
    "mips32be": (uc.UC_ARCH_MIPS, uc.UC_MODE_MIPS32 | uc.UC_MODE_BIG_ENDIAN, mips_const.UC_MIPS_REG_V0, mips_const.UC_MIPS_REG_A0, mips_const.UC_MIPS_REG_SP, mips_const.UC_MIPS_REG_RA, 32),
}


def constant(arch: str, value: int) -> list[str]:
    if arch == "x86_64":
        return [f"mov rax, {value}", "ret"]
    if arch == "x86":
        return [f"mov eax, {value}", "ret"]
    if arch == "aarch64":
        return [f"mov x0, #{value}", "ret"]
    if arch in ("arm", "thumb"):
        return [f"movw r0, #{value & 65535}", f"movt r0, #{value >> 16}", "bx lr"]
    if arch.startswith("riscv"):
        return [f"li a0, {value}", "ret"]
    return [f"li $2, {value}", "jr $31", "nop"]


def scenarios(arch: str) -> tuple[list[str], list[str]]:
    if arch.startswith("x86"):
        if arch == "x86_64":
            return (["xor eax,eax", "again: add eax,ecx", "dec ecx", "jnz again", "ret"], ["sub rsp,16", "mov [rsp+8],rcx", "mov rax,[rsp+8]", "add rsp,16", "ret"])
        return (["xor eax,eax", "again: add eax,ecx", "dec ecx", "jnz again", "ret"], ["sub esp,16", "mov [esp+8],ecx", "mov eax,[esp+8]", "add esp,16", "ret"])
    if arch == "aarch64":
        return (["mov x0,#0", "again: add x0,x0,x1", "subs x1,x1,#1", "b.ne again", "ret"], ["sub sp,sp,#16", "str x1,[sp,#8]", "ldr x0,[sp,#8]", "add sp,sp,#16", "ret"])
    if arch in ("arm", "thumb"):
        thumb = arch == "thumb"
        return ([f"{'movs' if thumb else 'mov'} r0,#0", f"again: {'adds' if thumb else 'add'} r0,r0,r1", "subs r1,r1,#1", "bne again", "bx lr"], ["sub sp,#16" if thumb else "sub sp,sp,#16", "str r1,[sp,#12]", "ldr r0,[sp,#12]", "add sp,#16" if thumb else "add sp,sp,#16", "bx lr"])
    if arch.startswith("riscv"):
        wide = arch.endswith("64")
        return (["li a0,0", "again: add a0,a0,a1", "addi a1,a1,-1", "bnez a1,again", "ret"], ["addi sp,sp,-16", f"{'sd' if wide else 'sw'} a1,8(sp)", f"{'ld' if wide else 'lw'} a0,8(sp)", "addi sp,sp,16", "ret"])
    return (["or $2,$0,$0", "again: addu $2,$2,$4", "addiu $4,$4,-1", "bne $4,$0,again", "nop", "jr $31", "nop"], ["addiu $29,$29,-16", "sw $4,12($29)", "lw $2,12($29)", "addiu $29,$29,16", "jr $31", "nop"])


def verify(arch: str, cli: Path) -> dict:
    architecture, mode, result_reg, argument_reg, sp_reg, return_reg, width = PROFILES[arch]
    mask = (1 << width) - 1
    rng = random.Random(0xC570)
    values = [0, 1, 127, 128, 2047, 2048, 32767, 32768, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF]
    if width == 64:
        values += [0x100000000, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF]
    values += [rng.getrandbits(width) for _ in range(100)]
    lines = []
    expected = {}
    for k, value in enumerate(values):
        name = f"value_{k}"
        lines += [name + ":", *constant(arch, value & mask)]
        expected[name] = value & mask
    loop, memory = scenarios(arch)
    lines += ["loop_test:", *loop, "memory_test:", *memory]
    directory = ROOT / "work" / "execution-tests"
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / f"{arch}.asm"
    source.write_text("\n".join(lines) + "\n", encoding="ascii")
    run = subprocess.run([str(cli), "--arch", arch, str(source), "--base", "0x1000", "--json"], capture_output=True, text=True)
    if run.returncode:
        raise AssertionError(f"{arch}: {run.stdout}{run.stderr}")
    record = json.loads(run.stdout)
    code = bytes.fromhex(record["hex"])
    addresses = {label["name"]: int(label["address"], 16) for label in record["labels"]}
    machine = uc.Uc(architecture, mode)
    machine.mem_map(0x1000, 0x10000)
    machine.mem_write(0x1000, code)
    machine.mem_map(0x200000, 0x10000)
    stop, stack = 0x10000, 0x208000

    def execute(name: str, argument: int) -> int:
        machine.reg_write(sp_reg, stack)
        machine.reg_write(argument_reg, argument)
        machine.reg_write(result_reg, mask)
        if return_reg is None:
            machine.mem_write(stack, stop.to_bytes(width // 8, "little"))
        else:
            machine.reg_write(return_reg, stop | (arch == "thumb"))
        machine.emu_start(addresses[name] | (arch == "thumb"), stop, timeout=1000000, count=10000)
        return machine.reg_read(result_reg) & mask

    for name, value in expected.items():
        actual = execute(name, 0)
        if actual != value:
            raise AssertionError(f"{arch}/{name}: {actual:#x} != {value:#x}")
    if execute("loop_test", 10) != 55:
        raise AssertionError(f"{arch}: loop result is wrong")
    pattern = 0xFEDCBA9876543210 & mask
    if execute("memory_test", pattern) != pattern:
        raise AssertionError(f"{arch}: stack memory round trip failed")
    count = len(values) + 2
    print(f"{arch}: {count} execution cases passed")
    return {"arch": arch, "cases": count, "oracle": f"Unicorn {uc.__version__}"}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cstone", required=True, type=Path)
    parser.add_argument("--arch", action="append")
    args = parser.parse_args()
    reports = [verify(arch, args.cstone.resolve()) for arch in PROFILES if not args.arch or arch in args.arch]
    (ROOT / "work" / "execution-tests" / "report.json").write_text(json.dumps(reports, indent=2) + "\n", encoding="utf-8")
    print(f"PASS: {sum(r['cases'] for r in reports)} execution cases")


if __name__ == "__main__":
    main()
