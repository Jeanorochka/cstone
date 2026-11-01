"""Compare Cstone's documented encodings with Clang's independent assembler.

Clang/objcopy are development-only references, not Cstone dependencies.
Generated files are retained under work/differential for inspection.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]
R32 = "eax ecx edx ebx esp ebp esi edi r8d r9d r10d r11d r12d r13d r14d r15d".split()
R64 = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15".split()


def run(args: list[str]) -> None:
    subprocess.run(args, check=True, capture_output=True, text=True)


def register_cases() -> list[str]:
    cases = [
        f"{instruction} {destination}, {source}"
        for instruction in ("mov", "add", "or", "and", "sub", "xor", "cmp", "test")
        for registers in (R32, R64)
        for destination in registers
        for source in registers
    ]
    cases += [f"{instruction} {register}" for instruction in ("inc", "dec") for register in R32 + R64]
    cases += [f"{instruction} {register}" for instruction in ("push", "pop", "call", "jmp") for register in R64]
    return cases


def immediate_cases() -> list[str]:
    cases: list[str] = []
    values = (-2147483648, -129, -128, -1, 0, 1, 127, 128, 255, 256, 2147483647)
    for registers in (R32, R64):
        for register in registers:
            for value in values:
                cases.append(f"mov {register}, {value}")
                for instruction in ("add", "or", "and", "sub", "xor", "cmp"):
                    cases.append(f"{instruction} {register}, {value}")
    for register in R32:
        for value in (0x80000000, 0xFFFFFF7F, 0xFFFFFF80, 0xFFFFFFFF):
            for instruction in ("mov", "add", "or", "and", "sub", "xor", "cmp"):
                cases.append(f"{instruction} {register}, {value}")
    for register in R64:
        for value in (2147483648, 0xFFFFFFFF, 0x123456789ABCDEF0, -2147483649, -9223372036854775808):
            cases.append(f"mov {register}, {value}")
    cases += [f"push {value}" for value in values]
    return cases


def branch_cases() -> tuple[list[str], int]:
    names = "jo jno jb jc jnae jae jnb jnc je jz jne jnz jbe jna ja jnbe js jns jp jpe jnp jpo jl jnge jge jnl jle jng jg jnle jmp call".split()
    lines: list[str] = []
    for index, instruction in enumerate(names):
        # Enough distance forces rel32 in the reference assembler as well.
        lines += [f"{instruction} ahead_{index}", *(["nop"] * 200), f"ahead_{index}:"]
        lines += [f"behind_{index}:", *(["nop"] * 200), f"{instruction} behind_{index}"]
    return lines, len(names) * 2


def compare(name: str, lines: list[str], cli: str, clang: str, objcopy: str) -> int:
    directory = ROOT / "work" / "differential"
    directory.mkdir(parents=True, exist_ok=True)
    source = directory / f"{name}.asm"
    reference = directory / f"{name}.s"
    object_file = directory / f"{name}.o"
    actual_file = directory / f"{name}.actual.bin"
    expected_file = directory / f"{name}.reference.bin"
    source.write_text("\n".join(lines) + "\n", encoding="ascii")
    reference_lines = [line.replace("db ", ".byte ", 1) if line.startswith("db ") else line for line in lines]
    reference.write_text(".intel_syntax noprefix\n.text\n" + "\n".join(reference_lines) + "\n", encoding="ascii")
    run([cli, str(source), "-o", str(actual_file)])
    run([clang, "--target=x86_64-unknown-linux-gnu", "-c", str(reference), "-o", str(object_file)])
    run([objcopy, "-O", "binary", "-j", ".text", str(object_file), str(expected_file)])
    actual, expected = actual_file.read_bytes(), expected_file.read_bytes()
    if actual != expected:
        mismatch = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b), min(len(actual), len(expected)))
        raise AssertionError(
            f"{name}: mismatch at byte {mismatch}; lengths {len(actual)} vs {len(expected)}; "
            f"Cstone={actual[mismatch:mismatch + 16].hex()} reference={expected[mismatch:mismatch + 16].hex()}"
        )
    print(f"{name}: {len(actual)} bytes match Clang exactly")
    return len(actual)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cstone", required=True, type=Path)
    parser.add_argument("--clang", default=shutil.which("clang"))
    parser.add_argument("--objcopy", default=shutil.which("objcopy"))
    args = parser.parse_args()
    if not args.clang or not args.objcopy:
        parser.error("Clang and objcopy must be available for this development check")
    cli = str(args.cstone.resolve())
    registers, immediates = register_cases(), immediate_cases()
    branches, branch_count = branch_cases()
    total = 0
    total += compare("registers", registers, cli, args.clang, args.objcopy)
    total += compare("immediates", immediates, cli, args.clang, args.objcopy)
    total += compare("branches", branches, cli, args.clang, args.objcopy)
    total += compare("misc", ["nop", "ret", "leave", "int3", "ud2", "db 0,127,128,255,-1,-128"], cli, args.clang, args.objcopy)
    print(f"PASS: {len(registers)} register cases, {len(immediates)} immediate cases, {branch_count} branches; {total} bytes compared")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(error.stderr or str(error)) from error
